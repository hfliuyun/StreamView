#include "raw_data_model.h"

#include <streamview/core/source.h>
#include <streamview/core/source_pager.h>

#include <QTest>

#include <algorithm>
#include <cstddef>
#include <span>
#include <vector>

using streamview::app::RawDataModel;
using streamview::app::RawDisplayMode;
using streamview::core::RandomAccessSource;
using streamview::core::SourcePager;
using streamview::core::SourceReadResult;
using streamview::core::SourceReadStatus;

namespace {

class RecordingSource final : public RandomAccessSource {
public:
    explicit RecordingSource(quint64 sizeBytes) : sizeBytes_(sizeBytes) {}

    [[nodiscard]] quint64 sizeBytes() const noexcept override { return sizeBytes_; }
    [[nodiscard]] QString identity() const override { return QStringLiteral("raw-fixture"); }

    [[nodiscard]] SourceReadResult
    readAt(quint64 byteOffset, std::span<std::byte> destination) const override {
        ++readCount_;
        lastOffset_ = byteOffset;
        lastSize_ = destination.size();
        if (byteOffset >= sizeBytes_) {
            return {SourceReadStatus::EndOfSource, 0, {}};
        }

        const auto count = static_cast<std::size_t>(std::min<quint64>(
            sizeBytes_ - byteOffset, static_cast<quint64>(destination.size())));
        for (std::size_t index = 0; index < count; ++index) {
            destination[index] = std::byte((byteOffset + index) & 0xFFU);
        }
        const auto status = count == destination.size() ? SourceReadStatus::Complete
                                                        : SourceReadStatus::EndOfSource;
        return {status, count, {}};
    }

    [[nodiscard]] std::size_t readCount() const noexcept { return readCount_; }
    [[nodiscard]] quint64 lastOffset() const noexcept { return lastOffset_; }
    [[nodiscard]] std::size_t lastSize() const noexcept { return lastSize_; }

private:
    quint64 sizeBytes_ = 0;
    mutable std::size_t readCount_ = 0;
    mutable quint64 lastOffset_ = 0;
    mutable std::size_t lastSize_ = 0;
};

} // namespace

class RawDataModelTest final : public QObject {
    Q_OBJECT

private slots:
    void loadsOnlyTheFirstPage() {
        RecordingSource source(SourcePager::pageSizeBytes() * 3U);
        RawDataModel model;

        QVERIFY(model.setSource(&source));

        QCOMPARE(source.readCount(), std::size_t{1});
        QCOMPARE(source.lastOffset(), quint64{0});
        QCOMPARE(source.lastSize(),
                 static_cast<std::size_t>(SourcePager::pageSizeBytes()));
        QCOMPARE(model.pageCount(), quint64{3});
        QCOMPARE(model.pageIndex(), quint64{0});
        QCOMPARE(model.rowCount(), 4096);
        QCOMPARE(model.columnCount(), RawDataModel::ColumnCount);
    }

    void adoptsAPreparedFirstPageWithoutReadingAgain() {
        RecordingSource source(4);
        SourcePager pager(source);
        auto firstPage = pager.loadPage(0);
        QCOMPARE(source.readCount(), std::size_t{1});
        RawDataModel model;

        QVERIFY(model.setSource(&source, std::move(firstPage)));

        QCOMPARE(source.readCount(), std::size_t{1});
        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(model.data(model.index(0, RawDataModel::FirstByte + 3)).toString(),
                 QStringLiteral("03"));
    }

    void formatsEachDisplayModeWithoutReadingAgain() {
        RecordingSource source(1);
        RawDataModel model;
        QVERIFY(model.setSource(&source));
        const QModelIndex byteCell = model.index(0, RawDataModel::FirstByte);

        QCOMPARE(model.data(byteCell).toString(), QStringLiteral("00"));

        model.setDisplayMode(RawDisplayMode::Binary);
        QCOMPARE(model.data(byteCell).toString(), QStringLiteral("00000000"));

        model.setDisplayMode(RawDisplayMode::Combined);
        QCOMPARE(model.data(byteCell).toString(), QStringLiteral("00\n00000000"));
        QCOMPARE(source.readCount(), std::size_t{1});
    }

    void loadsAShortFinalPage() {
        RecordingSource source(SourcePager::pageSizeBytes() + 3U);
        RawDataModel model;
        QVERIFY(model.setSource(&source));

        QVERIFY(model.loadPage(1));

        QCOMPARE(model.pageIndex(), quint64{1});
        QCOMPARE(model.pageByteOffset(), SourcePager::pageSizeBytes());
        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(model.data(model.index(0, RawDataModel::FirstByte)).toString(),
                 QStringLiteral("00"));
        QCOMPARE(model.data(model.index(0, RawDataModel::FirstByte + 2)).toString(),
                 QStringLiteral("02"));
        QVERIFY(!model.data(model.index(0, RawDataModel::FirstByte + 3)).isValid());
    }

    void exposesAbsoluteOffsetsForByteCells() {
        RecordingSource source(SourcePager::pageSizeBytes() + 32U);
        RawDataModel model;
        QVERIFY(model.setSource(&source));
        QVERIFY(model.loadPage(1));

        const QModelIndex byteCell = model.index(1, RawDataModel::FirstByte + 4);

        QCOMPARE(model.data(byteCell, RawDataModel::ByteOffsetRole).toULongLong(),
                 SourcePager::pageSizeBytes() + 20U);
        QCOMPARE(model.data(byteCell, RawDataModel::ByteValueRole).toUInt(), 20U);
    }

    void rejectsAnOutOfRangePageWithoutReplacingTheCurrentPage() {
        RecordingSource source(8);
        RawDataModel model;
        QVERIFY(model.setSource(&source));

        QString errorMessage;
        QVERIFY(!model.loadPage(1, &errorMessage));

        QCOMPARE(model.pageIndex(), quint64{0});
        QCOMPARE(model.rowCount(), 1);
        QVERIFY(errorMessage.contains(QStringLiteral("range")));
        QCOMPARE(source.readCount(), std::size_t{1});
    }

    void rejectsAPreparedPageThatDoesNotCoverItsDeclaredRange() {
        RecordingSource source(4);
        RawDataModel model;
        QVERIFY(model.setSource(&source));
        const QString before =
            model.data(model.index(0, RawDataModel::FirstByte)).toString();
        streamview::core::SourcePage incomplete;
        incomplete.status = streamview::core::SourcePageStatus::Ready;
        incomplete.bytes = {std::byte{0xFF}};
        QString errorMessage;

        QVERIFY(!model.setSource(&source, std::move(incomplete), &errorMessage));

        QVERIFY(errorMessage.contains(QStringLiteral("cover")));
        QCOMPARE(model.data(model.index(0, RawDataModel::FirstByte)).toString(), before);
        QCOMPARE(source.readCount(), std::size_t{1});
    }
};

QTEST_GUILESS_MAIN(RawDataModelTest)

#include "raw_data_model_test.moc"
