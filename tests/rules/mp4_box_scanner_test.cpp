#include <streamview/rules/mp4_box_scanner.h>
#include <streamview/core/cancellation.h>
#include <streamview/core/source.h>

#include <QObject>
#include <QTest>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <vector>

using streamview::core::CancellationSource;
using streamview::core::RandomAccessSource;
using streamview::core::SourceBitAddress;
using streamview::core::SourceReadResult;
using streamview::core::SourceReadStatus;
using streamview::core::SourceSpan;
using streamview::rules::Mp4BoxRecord;
using streamview::rules::Mp4BoxScanBatch;
using streamview::rules::Mp4BoxScanner;
using streamview::rules::Mp4BoxScanStatus;

namespace {

class MemorySource final : public RandomAccessSource {
public:
    explicit MemorySource(std::vector<std::byte> data,
                          bool failReads = false,
                          quint64 overrideSizeBytes = 0)
        : data_(std::move(data)),
          failReads_(failReads),
          overrideSizeBytes_(overrideSizeBytes) {}

    [[nodiscard]] quint64 sizeBytes() const noexcept override {
        if (overrideSizeBytes_ > 0) {
            return overrideSizeBytes_;
        }
        return static_cast<quint64>(data_.size());
    }

    [[nodiscard]] QString identity() const override { return QStringLiteral("memory"); }

    [[nodiscard]] SourceReadResult
    readAt(quint64 byteOffset, std::span<std::byte> destination) const override {
        if (failReads_) {
            return {SourceReadStatus::Error, 0, QStringLiteral("Simulated I/O read failure")};
        }
        if (destination.empty()) {
            return {SourceReadStatus::Complete, 0, {}};
        }
        if (byteOffset >= sizeBytes()) {
            return {SourceReadStatus::EndOfSource, 0, {}};
        }
        const auto offset = static_cast<std::size_t>(byteOffset);
        const std::size_t available = (offset < data_.size()) ? (data_.size() - offset) : 0;
        const std::size_t count = std::min(destination.size(), available);
        if (count > 0) {
            std::copy_n(data_.begin() + static_cast<std::ptrdiff_t>(offset),
                        static_cast<std::ptrdiff_t>(count),
                        destination.begin());
        }
        if (overrideSizeBytes_ > 0 && destination.size() > count) {
            std::fill(destination.begin() + static_cast<std::ptrdiff_t>(count),
                      destination.end(),
                      std::byte{0x00});
            return {SourceReadStatus::Complete, destination.size(), {}};
        }
        return {count == destination.size() ? SourceReadStatus::Complete
                                            : SourceReadStatus::EndOfSource,
                count,
                {}};
    }

private:
    std::vector<std::byte> data_;
    bool failReads_ = false;
    quint64 overrideSizeBytes_ = 0;
};

class FailOnceMemorySource final : public RandomAccessSource {
public:
    explicit FailOnceMemorySource(std::vector<std::byte> data)
        : data_(std::move(data)) {}

    [[nodiscard]] quint64 sizeBytes() const noexcept override {
        return static_cast<quint64>(data_.size());
    }

    [[nodiscard]] QString identity() const override { return QStringLiteral("fail-once"); }

    [[nodiscard]] SourceReadResult
    readAt(quint64 byteOffset, std::span<std::byte> destination) const override {
        ++readCount_;
        if (readCount_ == 1) {
            return {SourceReadStatus::Error, 0, QStringLiteral("Initial transient read failure")};
        }
        if (destination.empty()) {
            return {SourceReadStatus::Complete, 0, {}};
        }
        if (byteOffset >= data_.size()) {
            return {SourceReadStatus::EndOfSource, 0, {}};
        }
        const auto offset = static_cast<std::size_t>(byteOffset);
        const std::size_t count = std::min(destination.size(), data_.size() - offset);
        std::copy_n(data_.begin() + static_cast<std::ptrdiff_t>(offset),
                    static_cast<std::ptrdiff_t>(count),
                    destination.begin());
        return {count == destination.size() ? SourceReadStatus::Complete
                                            : SourceReadStatus::EndOfSource,
                count,
                {}};
    }

    [[nodiscard]] std::size_t readCount() const noexcept { return readCount_; }

private:
    std::vector<std::byte> data_;
    mutable std::size_t readCount_ = 0;
};

[[nodiscard]] std::vector<std::byte> makeNormalBox(quint32 size, quint32 type, std::byte fill = std::byte{0x00}) {
    std::vector<std::byte> box(size >= 8 ? size : 8, fill);
    box[0] = static_cast<std::byte>((size >> 24) & 0xFF);
    box[1] = static_cast<std::byte>((size >> 16) & 0xFF);
    box[2] = static_cast<std::byte>((size >> 8) & 0xFF);
    box[3] = static_cast<std::byte>(size & 0xFF);
    box[4] = static_cast<std::byte>((type >> 24) & 0xFF);
    box[5] = static_cast<std::byte>((type >> 16) & 0xFF);
    box[6] = static_cast<std::byte>((type >> 8) & 0xFF);
    box[7] = static_cast<std::byte>(type & 0xFF);
    return box;
}

[[nodiscard]] std::vector<std::byte> makeLargeBox(quint64 largesize, quint32 type, std::byte fill = std::byte{0x00}) {
    std::vector<std::byte> box(largesize >= 16 ? static_cast<std::size_t>(largesize) : 16U, fill);
    box[0] = std::byte{0x00};
    box[1] = std::byte{0x00};
    box[2] = std::byte{0x00};
    box[3] = std::byte{0x01};
    box[4] = static_cast<std::byte>((type >> 24) & 0xFF);
    box[5] = static_cast<std::byte>((type >> 16) & 0xFF);
    box[6] = static_cast<std::byte>((type >> 8) & 0xFF);
    box[7] = static_cast<std::byte>(type & 0xFF);
    for (std::size_t i = 0; i < 8; ++i) {
        box[8 + i] = static_cast<std::byte>((largesize >> ((7 - i) * 8)) & 0xFF);
    }
    return box;
}

} // namespace

class Mp4BoxScannerTest : public QObject {
    Q_OBJECT

private slots:
    void scansNormalBoxesConsecutively() {
        std::vector<std::byte> data;
        const auto b1 = makeNormalBox(32, 0x66747970);
        const auto b2 = makeNormalBox(8,  0x66726565);
        const auto b3 = makeNormalBox(64, 0x6D6F6F76);
        data.insert(data.end(), b1.begin(), b1.end());
        data.insert(data.end(), b2.begin(), b2.end());
        data.insert(data.end(), b3.begin(), b3.end());

        MemorySource source(data);
        Mp4BoxScanner scanner(source);
        const auto batch = scanner.scanBatch();

        QCOMPARE(batch.status, Mp4BoxScanStatus::Complete);
        QCOMPARE(batch.records.size(), std::size_t(3));
        QVERIFY(scanner.finished());
        QCOMPARE(scanner.cursor(), quint64(104));

        // Box 1 (offset 0, size 32)
        QCOMPARE(batch.records[0].boxOffset, quint64(0));
        QCOMPARE(batch.records[0].declaredBoxSize, quint64(32));
        QCOMPARE(batch.records[0].truncated, false);
        QCOMPARE(batch.records[0].terminal, false);
        QVERIFY(batch.records[0].boxSpan.has_value());
        QCOMPARE(batch.records[0].boxSpan->start().absoluteBitOffset(), quint64(0));
        QCOMPARE(batch.records[0].boxSpan->bitLength(), quint64(32 * 8));

        // Box 2 (offset 32, size 8)
        QCOMPARE(batch.records[1].boxOffset, quint64(32));
        QCOMPARE(batch.records[1].declaredBoxSize, quint64(8));
        QCOMPARE(batch.records[1].truncated, false);
        QCOMPARE(batch.records[1].terminal, false);
        QVERIFY(batch.records[1].boxSpan.has_value());
        QCOMPARE(batch.records[1].boxSpan->start().absoluteBitOffset(), quint64(32 * 8));
        QCOMPARE(batch.records[1].boxSpan->bitLength(), quint64(8 * 8));

        // Box 3 (offset 40, size 64)
        QCOMPARE(batch.records[2].boxOffset, quint64(40));
        QCOMPARE(batch.records[2].declaredBoxSize, quint64(64));
        QCOMPARE(batch.records[2].truncated, false);
        QCOMPARE(batch.records[2].terminal, false);
        QVERIFY(batch.records[2].boxSpan.has_value());
        QCOMPARE(batch.records[2].boxSpan->start().absoluteBitOffset(), quint64(40 * 8));
        QCOMPARE(batch.records[2].boxSpan->bitLength(), quint64(64 * 8));
    }

    void scansLargeSizeBox() {
        const auto b = makeLargeBox(100, 0x6D646174);
        MemorySource source(b);
        Mp4BoxScanner scanner(source);
        const auto batch = scanner.scanBatch();

        QCOMPARE(batch.status, Mp4BoxScanStatus::Complete);
        QCOMPARE(batch.records.size(), std::size_t(1));
        QCOMPARE(batch.records[0].boxOffset, quint64(0));
        QCOMPARE(batch.records[0].declaredBoxSize, quint64(100));
        QCOMPARE(batch.records[0].truncated, false);
        QCOMPARE(batch.records[0].terminal, false);
        QVERIFY(batch.records[0].boxSpan.has_value());
        QCOMPARE(batch.records[0].boxSpan->start().absoluteBitOffset(), quint64(0));
        QCOMPARE(batch.records[0].boxSpan->bitLength(), quint64(100 * 8));
        QCOMPARE(scanner.cursor(), quint64(100));
    }

    void scansSizeZeroBoxExtendingToEof() {
        std::vector<std::byte> data;
        const auto b1 = makeNormalBox(16, 0x66747970);
        const auto b2 = makeNormalBox(0,  0x6D646174);
        data.insert(data.end(), b1.begin(), b1.end());
        data.insert(data.end(), b2.begin(), b2.end());
        data.insert(data.end(), 50, std::byte{0x55});

        MemorySource source(data);
        Mp4BoxScanner scanner(source);
        const auto batch = scanner.scanBatch();

        QCOMPARE(batch.status, Mp4BoxScanStatus::Complete);
        QCOMPARE(batch.records.size(), std::size_t(2));
        QVERIFY(scanner.finished());
        QCOMPARE(scanner.cursor(), quint64(data.size()));

        // Box 2 is size 0 terminal
        QCOMPARE(batch.records[1].boxOffset, quint64(16));
        QCOMPARE(batch.records[1].declaredBoxSize, quint64(0));
        QCOMPARE(batch.records[1].truncated, false);
        QCOMPARE(batch.records[1].terminal, true);
        QVERIFY(batch.records[1].boxSpan.has_value());
        QCOMPARE(batch.records[1].boxSpan->start().absoluteBitOffset(), quint64(16 * 8));
        QCOMPARE(batch.records[1].boxSpan->bitLength(), quint64((data.size() - 16) * 8));
    }

    void scansTruncatedBoxExceedingRegion() {
        const auto b = makeNormalBox(50, 0x6D6F6F76);
        std::vector<std::byte> truncatedData(b.begin(), b.begin() + 20);

        MemorySource source(truncatedData);
        Mp4BoxScanner scanner(source);
        const auto batch = scanner.scanBatch();

        QCOMPARE(batch.status, Mp4BoxScanStatus::Complete);
        QCOMPARE(batch.records.size(), std::size_t(1));
        QCOMPARE(batch.records[0].boxOffset, quint64(0));
        QCOMPARE(batch.records[0].declaredBoxSize, quint64(50));
        QCOMPARE(batch.records[0].truncated, true);
        QCOMPARE(batch.records[0].terminal, true);
        QVERIFY(batch.records[0].boxSpan.has_value());
        QCOMPARE(batch.records[0].boxSpan->start().absoluteBitOffset(), quint64(0));
        QCOMPARE(batch.records[0].boxSpan->bitLength(), quint64(20 * 8));
        QVERIFY(scanner.finished());
        QCOMPARE(scanner.cursor(), quint64(20));
    }

    void handlesTruncatedHeaderLessThanEightBytes() {
        std::vector<std::byte> data = {std::byte{0x00}, std::byte{0x00}, std::byte{0x00}};
        MemorySource source(data);
        Mp4BoxScanner scanner(source);
        const auto batch = scanner.scanBatch();

        QCOMPARE(batch.status, Mp4BoxScanStatus::Complete);
        QCOMPARE(batch.records.size(), std::size_t(0));
        QVERIFY(scanner.finished());
    }

    void handlesIncompleteLargeHeaderLessThanSixteenBytes() {
        std::vector<std::byte> data(12, std::byte{0x00});
        data[3] = std::byte{0x01};
        data[4] = std::byte{0x6D};
        data[5] = std::byte{0x64};
        data[6] = std::byte{0x61};
        data[7] = std::byte{0x74};

        MemorySource source(data);
        Mp4BoxScanner scanner(source);
        const auto batch = scanner.scanBatch();

        QCOMPARE(batch.status, Mp4BoxScanStatus::Complete);
        QCOMPARE(batch.records.size(), std::size_t(0));
        QVERIFY(scanner.finished());
    }

    void handlesMalformedSizeTwoToSeven() {
        std::vector<std::byte> data;
        const auto b1 = makeNormalBox(16, 0x66747970);
        const auto b2 = makeNormalBox(5,  0x6D6F6F76);
        data.insert(data.end(), b1.begin(), b1.end());
        data.insert(data.end(), b2.begin(), b2.end());

        MemorySource source(data);
        Mp4BoxScanner scanner(source);
        const auto batch = scanner.scanBatch();

        QCOMPARE(batch.status, Mp4BoxScanStatus::Complete);
        QCOMPARE(batch.records.size(), std::size_t(1));
        QVERIFY(scanner.finished());
        QCOMPARE(scanner.cursor(), quint64(16));
    }

    void handlesMalformedLargeSizeLessThanSixteen() {
        const auto b = makeLargeBox(12, 0x6D646174);
        MemorySource source(b);
        Mp4BoxScanner scanner(source);
        const auto batch = scanner.scanBatch();

        QCOMPARE(batch.status, Mp4BoxScanStatus::Complete);
        QCOMPARE(batch.records.size(), std::size_t(0));
        QVERIFY(scanner.finished());
    }

    void handlesReadError() {
        std::vector<std::byte> data = makeNormalBox(16, 0x66747970);
        MemorySource source(data, /*failReads=*/true);
        Mp4BoxScanner scanner(source);
        const auto batch = scanner.scanBatch();

        QCOMPARE(batch.status, Mp4BoxScanStatus::SourceError);
        QVERIFY(!batch.errorMessage.isEmpty());
    }

    void handlesHeaderCrossingBufferBoundary() {
        // Construct Box 1 of size 65533 (offset 0..65532)
        // Box 2 (16 bytes) starts at offset 65533.
        // Bytes 65533..65535 (3 bytes) are in chunk 0; byte 65536 (4th byte of size) and beyond are in chunk 1.
        // The 4-byte size field itself crosses the 64 KiB boundary.
        std::vector<std::byte> data;
        const auto b1 = makeNormalBox(65533, 0x66747970);
        const auto b2 = makeNormalBox(16,    0x6D6F6F76);
        data.insert(data.end(), b1.begin(), b1.end());
        data.insert(data.end(), b2.begin(), b2.end());

        MemorySource source(data);
        Mp4BoxScanner scanner(source);
        const auto batch = scanner.scanBatch();

        QCOMPARE(batch.status, Mp4BoxScanStatus::Complete);
        QCOMPARE(batch.records.size(), std::size_t(2));
        QCOMPARE(batch.records[0].boxOffset, quint64(0));
        QCOMPARE(batch.records[0].declaredBoxSize, quint64(65533));
        QCOMPARE(batch.records[1].boxOffset, quint64(65533));
        QCOMPARE(batch.records[1].declaredBoxSize, quint64(16));
        QVERIFY(scanner.finished());
        QCOMPARE(scanner.cursor(), quint64(65533 + 16));
    }

    void handlesLargeHeaderCrossingBufferBoundary() {
        // Construct Box 1 of size 65524 (offset 0..65523)
        // Box 2 is a large header box starting at offset 65524.
        // Header bytes 0..7 (size=1, type='mdat') are at 65524..65531.
        // largesize (8 bytes) is at start+8 = 65532..65539, which spans across 65536.
        std::vector<std::byte> data;
        const auto b1 = makeNormalBox(65524, 0x66747970);
        const auto b2 = makeLargeBox(100,    0x6D646174);
        data.insert(data.end(), b1.begin(), b1.end());
        data.insert(data.end(), b2.begin(), b2.end());

        MemorySource source(data);
        Mp4BoxScanner scanner(source);
        const auto batch = scanner.scanBatch();

        QCOMPARE(batch.status, Mp4BoxScanStatus::Complete);
        QCOMPARE(batch.records.size(), std::size_t(2));
        QCOMPARE(batch.records[0].boxOffset, quint64(0));
        QCOMPARE(batch.records[0].declaredBoxSize, quint64(65524));
        QCOMPARE(batch.records[1].boxOffset, quint64(65524));
        QCOMPARE(batch.records[1].declaredBoxSize, quint64(100));
        QVERIFY(scanner.finished());
        QCOMPARE(scanner.cursor(), quint64(65524 + 100));
    }

    void handlesZeroMaximumInspectedPositions() {
        const auto b = makeNormalBox(16, 0x66747970);
        MemorySource source(b);
        Mp4BoxScanner scanner(source);
        const auto batch = scanner.scanBatch(256, 0);

        QCOMPARE(batch.status, Mp4BoxScanStatus::InvalidBatchSize);
        QVERIFY(!batch.errorMessage.isEmpty());
    }

    void persistsSourceErrorStateOnSubsequentCalls() {
        std::vector<std::byte> data = makeNormalBox(16, 0x66747970);
        FailOnceMemorySource source(data);
        Mp4BoxScanner scanner(source);

        // First call fails with transient SourceError
        const auto batch1 = scanner.scanBatch();
        QCOMPARE(batch1.status, Mp4BoxScanStatus::SourceError);
        QCOMPARE(batch1.errorMessage, QStringLiteral("Initial transient read failure"));
        QCOMPARE(source.readCount(), std::size_t(1));

        // Subsequent valid call must persistently return SourceError without performing further reads
        const auto batch2 = scanner.scanBatch();
        QCOMPARE(batch2.status, Mp4BoxScanStatus::SourceError);
        QCOMPARE(batch2.errorMessage, QStringLiteral("Initial transient read failure"));
        QCOMPARE(source.readCount(), std::size_t(1));
    }

    void handlesBitCoordinateOverflowWithOverrideSize() {
        // Defensive coverage: size 0 box with synthetic huge source size exceeding 64-bit bit limit
        const auto b = makeNormalBox(0, 0x6D646174);
        constexpr quint64 hugeSize = (std::numeric_limits<quint64>::max() / 8U) + 100U;
        MemorySource source(b, /*failReads=*/false, /*overrideSizeBytes=*/hugeSize);

        Mp4BoxScanner scanner(source);
        const auto batch = scanner.scanBatch();

        QCOMPARE(batch.status, Mp4BoxScanStatus::SourceError);
        QCOMPARE(batch.errorMessage, QStringLiteral("Coordinate arithmetic overflow"));
    }

    void supportsBatchPagingAndWorkBudget() {
        std::vector<std::byte> data;
        for (int i = 0; i < 5; ++i) {
            const auto b = makeNormalBox(16, 0x66726565);
            data.insert(data.end(), b.begin(), b.end());
        }

        MemorySource source(data);
        Mp4BoxScanner scanner(source);

        // First batch: max 2 records
        const auto batch1 = scanner.scanBatch(2);
        QCOMPARE(batch1.status, Mp4BoxScanStatus::InProgress);
        QCOMPARE(batch1.records.size(), std::size_t(2));
        QCOMPARE(scanner.cursor(), quint64(32));

        // Second batch: max 2 records
        const auto batch2 = scanner.scanBatch(2);
        QCOMPARE(batch2.status, Mp4BoxScanStatus::InProgress);
        QCOMPARE(batch2.records.size(), std::size_t(2));
        QCOMPARE(scanner.cursor(), quint64(64));

        // Third batch: max 2 records
        const auto batch3 = scanner.scanBatch(2);
        QCOMPARE(batch3.status, Mp4BoxScanStatus::Complete);
        QCOMPARE(batch3.records.size(), std::size_t(1));
        QCOMPARE(scanner.cursor(), quint64(80));
        QVERIFY(scanner.finished());
    }

    void handlesInvalidBatchSize() {
        const auto b = makeNormalBox(16, 0x66747970);
        MemorySource source(b);
        Mp4BoxScanner scanner(source);
        const auto batch = scanner.scanBatch(0);

        QCOMPARE(batch.status, Mp4BoxScanStatus::InvalidBatchSize);
        QVERIFY(!batch.errorMessage.isEmpty());
    }

    void supportsCancellationAndResume() {
        std::vector<std::byte> data;
        for (int i = 0; i < 4; ++i) {
            const auto b = makeNormalBox(16, 0x66726565);
            data.insert(data.end(), b.begin(), b.end());
        }

        MemorySource source(data);
        CancellationSource cancelSource;
        Mp4BoxScanner scanner(source, cancelSource.token());

        QVERIFY(cancelSource.requestCancellation());
        const auto batch1 = scanner.scanBatch();
        QCOMPARE(batch1.status, Mp4BoxScanStatus::Cancelled);

        // Resume with new token
        CancellationSource newCancelSource;
        scanner.replaceCancellationToken(newCancelSource.token());
        const auto batch2 = scanner.scanBatch();
        QCOMPARE(batch2.status, Mp4BoxScanStatus::Complete);
        QCOMPARE(batch2.records.size(), std::size_t(4));
    }
};

QTEST_MAIN(Mp4BoxScannerTest)
#include "mp4_box_scanner_test.moc"
