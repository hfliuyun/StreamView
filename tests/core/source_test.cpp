#include <streamview/core/source.h>
#include <streamview/core/source_fingerprint.h>

#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <array>
#include <cstddef>
#include <span>

using streamview::core::FileSource;
using streamview::core::SourceFingerprint;
using streamview::core::SourceFingerprintMode;
using streamview::core::SourceFingerprintStatus;
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

    void fingerprintsSmallFilesWithFullSha256() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("small.bin"));
        const QByteArray bytes = QByteArray::fromHex("00112233445566778899aabbccddeeff");
        QFile output(path);
        QVERIFY(output.open(QIODevice::WriteOnly));
        QCOMPARE(output.write(bytes), bytes.size());
        output.close();

        QString error;
        const auto source = FileSource::open(path, &error);
        QVERIFY2(source != nullptr, qPrintable(error));
        const auto result = source->fingerprint();
        QVERIFY2(result.succeeded(), qPrintable(result.errorMessage));
        QCOMPARE(result.fingerprint->version(), SourceFingerprint::algorithmVersion());
        QCOMPARE(result.fingerprint->mode(), SourceFingerprintMode::FullContentSha256);
        QCOMPARE(result.fingerprint->sizeBytes(), static_cast<quint64>(bytes.size()));
        QVERIFY(!result.fingerprint->modificationTimeNanoseconds().has_value());
        QCOMPARE(result.fingerprint->digest(),
                 QCryptographicHash::hash(bytes, QCryptographicHash::Sha256));
        QCOMPARE(result.fingerprint->digestText(),
                 QString::fromLatin1(result.fingerprint->digest().toHex()));
    }

    void fingerprintsLargeFilesWithFirstMiddleAndLastSamples() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("large.bin"));
        const qsizetype size =
            static_cast<qsizetype>(SourceFingerprint::fullContentLimitBytes() + 257U);
        QByteArray bytes(size, Qt::Uninitialized);
        for (qsizetype index = 0; index < bytes.size(); ++index) {
            bytes[index] = static_cast<char>((static_cast<quint64>(index) * 31U + 7U) & 0xFFU);
        }
        QFile output(path);
        QVERIFY(output.open(QIODevice::WriteOnly));
        QCOMPARE(output.write(bytes), bytes.size());
        output.close();

        QString error;
        const auto source = FileSource::open(path, &error);
        QVERIFY2(source != nullptr, qPrintable(error));
        const auto result = source->fingerprint();
        QVERIFY2(result.succeeded(), qPrintable(result.errorMessage));
        QCOMPARE(result.fingerprint->mode(), SourceFingerprintMode::SampledSha256);
        QCOMPARE(result.fingerprint->sizeBytes(), static_cast<quint64>(bytes.size()));
        QVERIFY(result.fingerprint->modificationTimeNanoseconds().has_value());

        const qsizetype sampleSize =
            static_cast<qsizetype>(SourceFingerprint::sampleSizeBytes());
        const qsizetype middleOffset = (bytes.size() - sampleSize) / 2;
        const qsizetype tailOffset = bytes.size() - sampleSize;
        QCryptographicHash expected(QCryptographicHash::Sha256);
        expected.addData(QByteArrayView(bytes.constData(), sampleSize));
        expected.addData(QByteArrayView(bytes.constData() + middleOffset, sampleSize));
        expected.addData(QByteArrayView(bytes.constData() + tailOffset, sampleSize));
        QCOMPARE(result.fingerprint->digest(), expected.result());
    }

    void changedContentAtTheSamePathHasAnotherFingerprint() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("replaceable.bin"));
        const auto writeContents = [&path](QByteArrayView contents) {
            QFile file(path);
            return file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
                   file.write(contents.data(), contents.size()) == contents.size();
        };
        QVERIFY(writeContents(QByteArrayView("first-content", 13)));
        QString error;
        auto firstSource = FileSource::open(path, &error);
        QVERIFY2(firstSource != nullptr, qPrintable(error));
        const auto first = firstSource->fingerprint();
        QVERIFY2(first.succeeded(), qPrintable(first.errorMessage));
        firstSource.reset();

        QVERIFY(writeContents(QByteArrayView("other-content", 13)));
        auto secondSource = FileSource::open(path, &error);
        QVERIFY2(secondSource != nullptr, qPrintable(error));
        const auto second = secondSource->fingerprint();
        QVERIFY2(second.succeeded(), qPrintable(second.errorMessage));
        QVERIFY(first.fingerprint != second.fingerprint);
    }

    void modificationTimeOnlyAffectsSampledFingerprints() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto writeFile = [](const QString& path, QByteArrayView contents) {
            QFile file(path);
            return file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
                   file.write(contents.data(), contents.size()) == contents.size();
        };
        const auto setLaterTime = [](const QString& path) {
            QFile file(path);
            return file.open(QIODevice::ReadWrite) &&
                   file.setFileTime(QDateTime::currentDateTimeUtc().addSecs(60),
                                    QFileDevice::FileModificationTime);
        };

        const QString smallPath = directory.filePath(QStringLiteral("small-time.bin"));
        QVERIFY(writeFile(smallPath, QByteArrayView("content", 7)));
        QString error;
        auto smallSource = FileSource::open(smallPath, &error);
        QVERIFY2(smallSource != nullptr, qPrintable(error));
        const auto smallBefore = smallSource->fingerprint();
        QVERIFY(smallBefore.succeeded());
        smallSource.reset();
        QVERIFY(setLaterTime(smallPath));
        smallSource = FileSource::open(smallPath, &error);
        QVERIFY2(smallSource != nullptr, qPrintable(error));
        const auto smallAfter = smallSource->fingerprint();
        QVERIFY(smallAfter.succeeded());
        QCOMPARE(smallBefore.fingerprint, smallAfter.fingerprint);

        const QString largePath = directory.filePath(QStringLiteral("large-time.bin"));
        const qsizetype largeSize =
            static_cast<qsizetype>(SourceFingerprint::fullContentLimitBytes() + 1U);
        const QByteArray largeBytes(largeSize, '\x5a');
        QVERIFY(writeFile(largePath, largeBytes));
        auto largeSource = FileSource::open(largePath, &error);
        QVERIFY2(largeSource != nullptr, qPrintable(error));
        const auto largeBefore = largeSource->fingerprint();
        QVERIFY(largeBefore.succeeded());
        largeSource.reset();
        QVERIFY(setLaterTime(largePath));
        largeSource = FileSource::open(largePath, &error);
        QVERIFY2(largeSource != nullptr, qPrintable(error));
        const auto largeAfter = largeSource->fingerprint();
        QVERIFY(largeAfter.succeeded());
        QVERIFY(largeBefore.fingerprint != largeAfter.fingerprint);
        QCOMPARE(largeBefore.fingerprint->digest(), largeAfter.fingerprint->digest());
    }

    void rejectsAFileWhoseSizeChangedAfterOpen() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("growing.bin"));
        QFile output(path);
        QVERIFY(output.open(QIODevice::WriteOnly));
        QCOMPARE(output.write("initial", 7), qint64{7});
        output.close();

        QString error;
        const auto source = FileSource::open(path, &error);
        QVERIFY2(source != nullptr, qPrintable(error));
        QFile appender(path);
        QVERIFY(appender.open(QIODevice::WriteOnly | QIODevice::Append));
        QCOMPARE(appender.write("more", 4), qint64{4});
        appender.close();

        const auto result = source->fingerprint();
        QCOMPARE(result.status, SourceFingerprintStatus::SourceChanged);
        QVERIFY(!result.fingerprint.has_value());
        QVERIFY(!result.errorMessage.isEmpty());
    }

    void rejectsMalformedFingerprintValues() {
        const QByteArray digest(QCryptographicHash::hashLength(QCryptographicHash::Sha256), '\0');
        QString error;
        QVERIFY(!SourceFingerprint::create(SourceFingerprint::algorithmVersion() + 1U,
                                           SourceFingerprintMode::FullContentSha256, 0, std::nullopt,
                                           digest, &error)
                     .has_value());
        QVERIFY(!error.isEmpty());
        QVERIFY(!SourceFingerprint::create(SourceFingerprint::algorithmVersion(),
                                           SourceFingerprintMode::SampledSha256,
                                           SourceFingerprint::fullContentLimitBytes(), 0,
                                           digest, &error)
                     .has_value());
        QVERIFY(!SourceFingerprint::create(SourceFingerprint::algorithmVersion(),
                                           SourceFingerprintMode::FullContentSha256,
                                           SourceFingerprint::fullContentLimitBytes() + 1U,
                                           std::nullopt,
                                           digest, &error)
                     .has_value());
        QVERIFY(!SourceFingerprint::create(SourceFingerprint::algorithmVersion(),
                                           SourceFingerprintMode::FullContentSha256, 0, std::nullopt,
                                           QByteArrayLiteral("short"), &error)
                     .has_value());
        QVERIFY(!SourceFingerprint::create(SourceFingerprint::algorithmVersion(),
                                           SourceFingerprintMode::FullContentSha256, 0, 0,
                                           digest, &error)
                     .has_value());
        QVERIFY(!SourceFingerprint::create(
                     SourceFingerprint::algorithmVersion(),
                     SourceFingerprintMode::SampledSha256,
                     SourceFingerprint::fullContentLimitBytes() + 1U, std::nullopt,
                     digest, &error)
                     .has_value());
    }
};

QTEST_GUILESS_MAIN(SourceTest)

#include "source_test.moc"
