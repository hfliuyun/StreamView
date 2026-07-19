#include <streamview/core/source.h>

#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <array>
#include <cstddef>
#include <span>

using streamview::core::FileSource;
using streamview::core::SourceReadStatus;

class SourceTest final : public QObject {
    Q_OBJECT

private slots:
    void opensAndReadsWithoutModifyingTheFile() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("source.bin"));
        const QByteArray original = QByteArray::fromHex("10203040");

        QFile output(path);
        QVERIFY(output.open(QIODevice::WriteOnly));
        QCOMPARE(output.write(original), original.size());
        output.close();

        QString error;
        auto source = FileSource::open(path, &error);
        QVERIFY2(source != nullptr, qPrintable(error));
        QCOMPARE(source->sizeBytes(), quint64{4});
        QCOMPARE(source->identity(), path);

        std::array<std::byte, 2> middle{};
        const auto middleResult = source->readAt(1, middle);
        QVERIFY(middleResult.status == SourceReadStatus::Complete);
        QCOMPARE(middleResult.bytesRead, std::size_t{2});
        QCOMPARE(std::to_integer<unsigned int>(middle.at(0)), 0x20U);
        QCOMPARE(std::to_integer<unsigned int>(middle.at(1)), 0x30U);

        std::array<std::byte, 3> tail{};
        const auto tailResult = source->readAt(3, tail);
        QVERIFY(tailResult.status == SourceReadStatus::EndOfSource);
        QCOMPARE(tailResult.bytesRead, std::size_t{1});
        QCOMPARE(std::to_integer<unsigned int>(tail.at(0)), 0x40U);

        const auto pastEndResult = source->readAt(9, tail);
        QVERIFY(pastEndResult.status == SourceReadStatus::EndOfSource);
        QCOMPARE(pastEndResult.bytesRead, std::size_t{0});

        source.reset();
        QFile verification(path);
        QVERIFY(verification.open(QIODevice::ReadOnly));
        QCOMPARE(verification.readAll(), original);
    }

    void reportsOpenErrors() {
        QString error;
        const auto source = FileSource::open(QStringLiteral("/path/that/does/not/exist"), &error);
        QVERIFY(source == nullptr);
        QVERIFY(!error.isEmpty());
    }
};

QTEST_GUILESS_MAIN(SourceTest)

#include "source_test.moc"
