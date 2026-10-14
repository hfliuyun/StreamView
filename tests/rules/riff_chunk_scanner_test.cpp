#include <streamview/rules/riff_chunk_scanner.h>
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
using streamview::rules::RiffChunkRecord;
using streamview::rules::RiffChunkScanBatch;
using streamview::rules::RiffChunkScanner;
using streamview::rules::RiffChunkScanStatus;

namespace {

constexpr quint32 kRiffMagic = 0x52494646U;   // 'RIFF'
constexpr quint32 kWaveForm = 0x57415645U;    // 'WAVE'
constexpr quint32 kFmtId = 0x666D7420U;       // 'fmt '
constexpr quint32 kDataId = 0x64617461U;      // 'data'

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

void appendU32Le(std::vector<std::byte>& data, quint32 value) {
    data.push_back(static_cast<std::byte>(value & 0xFF));
    data.push_back(static_cast<std::byte>((value >> 8) & 0xFF));
    data.push_back(static_cast<std::byte>((value >> 16) & 0xFF));
    data.push_back(static_cast<std::byte>((value >> 24) & 0xFF));
}

void appendU32Be(std::vector<std::byte>& data, quint32 value) {
    data.push_back(static_cast<std::byte>((value >> 24) & 0xFF));
    data.push_back(static_cast<std::byte>((value >> 16) & 0xFF));
    data.push_back(static_cast<std::byte>((value >> 8) & 0xFF));
    data.push_back(static_cast<std::byte>(value & 0xFF));
}

[[nodiscard]] std::vector<std::byte> makeFormHeader(quint32 riffSize) {
    std::vector<std::byte> data;
    appendU32Be(data, kRiffMagic);
    appendU32Le(data, riffSize);
    appendU32Be(data, kWaveForm);
    return data;
}

[[nodiscard]] std::vector<std::byte> makeChunk(quint32 id, quint32 size, std::byte fill = std::byte{0x00}) {
    std::vector<std::byte> data;
    appendU32Be(data, id);
    appendU32Le(data, size);
    data.insert(data.end(), size, fill);
    if ((size & 1U) != 0U) {
        data.push_back(std::byte{0x7E});
    }
    return data;
}

} // namespace

class RiffChunkScannerTest : public QObject {
    Q_OBJECT

private slots:
    void scansFormHeaderAndChunksConsecutively() {
        std::vector<std::byte> data = makeFormHeader(4U + 8U + 16U + 8U + 4U);
        const auto fmt = makeChunk(kFmtId, 16U);
        const auto payload = makeChunk(kDataId, 4U);
        data.insert(data.end(), fmt.begin(), fmt.end());
        data.insert(data.end(), payload.begin(), payload.end());

        MemorySource source(data);
        RiffChunkScanner scanner(source);
        const auto batch = scanner.scanBatch();

        QCOMPARE(batch.status, RiffChunkScanStatus::Complete);
        QCOMPARE(batch.records.size(), std::size_t(3));
        QVERIFY(scanner.finished());
        QCOMPARE(scanner.cursor(), quint64(data.size()));

        // Record 0: the 12-byte form header
        QCOMPARE(batch.records[0].chunkOffset, quint64(0));
        QCOMPARE(batch.records[0].chunkId, kRiffMagic);
        QCOMPARE(batch.records[0].declaredSize, quint64(4U + 8U + 16U + 8U + 4U));
        QCOMPARE(batch.records[0].isFormHeader, true);
        QCOMPARE(batch.records[0].truncated, false);
        QCOMPARE(batch.records[0].terminal, false);
        QVERIFY(batch.records[0].chunkSpan.has_value());
        QCOMPARE(batch.records[0].chunkSpan->start().absoluteBitOffset(), quint64(0));
        QCOMPARE(batch.records[0].chunkSpan->bitLength(), quint64(12 * 8));

        // Record 1: fmt chunk at offset 12
        QCOMPARE(batch.records[1].chunkOffset, quint64(12));
        QCOMPARE(batch.records[1].chunkId, kFmtId);
        QCOMPARE(batch.records[1].declaredSize, quint64(16));
        QCOMPARE(batch.records[1].isFormHeader, false);
        QVERIFY(batch.records[1].chunkSpan.has_value());
        QCOMPARE(batch.records[1].chunkSpan->start().absoluteBitOffset(), quint64(12 * 8));
        QCOMPARE(batch.records[1].chunkSpan->bitLength(), quint64((8 + 16) * 8));

        // Record 2: data chunk at offset 36
        QCOMPARE(batch.records[2].chunkOffset, quint64(36));
        QCOMPARE(batch.records[2].chunkId, kDataId);
        QCOMPARE(batch.records[2].declaredSize, quint64(4));
        QVERIFY(batch.records[2].chunkSpan.has_value());
        QCOMPARE(batch.records[2].chunkSpan->start().absoluteBitOffset(), quint64(36 * 8));
        QCOMPARE(batch.records[2].chunkSpan->bitLength(), quint64((8 + 4) * 8));
    }

    void consumesOddSizePadBytes() {
        // fmt with an odd 17-byte payload: the next chunk starts after the pad.
        std::vector<std::byte> data = makeFormHeader(8U + 17U + 1U + 8U + 4U);
        const auto fmt = makeChunk(kFmtId, 17U);
        const auto payload = makeChunk(kDataId, 4U);
        data.insert(data.end(), fmt.begin(), fmt.end());
        data.insert(data.end(), payload.begin(), payload.end());

        MemorySource source(data);
        RiffChunkScanner scanner(source);
        const auto batch = scanner.scanBatch();

        QCOMPARE(batch.status, RiffChunkScanStatus::Complete);
        QCOMPARE(batch.records.size(), std::size_t(3));

        // fmt record covers id + size + 17 payload bytes + 1 pad byte.
        QVERIFY(batch.records[1].chunkSpan.has_value());
        QCOMPARE(batch.records[1].chunkSpan->bitLength(), quint64((8 + 17 + 1) * 8));

        // The data chunk starts right after the pad byte.
        QCOMPARE(batch.records[2].chunkOffset, quint64(12 + 8 + 17 + 1));
        QCOMPARE(batch.records[2].chunkId, kDataId);
        QVERIFY(scanner.finished());
    }

    void completesWithoutRecordsWhenMagicsAreMissing() {
        std::vector<std::byte> data(64, std::byte{0x11});
        appendU32Be(data, kFmtId);

        MemorySource source(data);
        RiffChunkScanner scanner(source);
        const auto batch = scanner.scanBatch();

        QCOMPARE(batch.status, RiffChunkScanStatus::Complete);
        QCOMPARE(batch.records.size(), std::size_t(0));
        QVERIFY(scanner.finished());
    }

    void emitsNoChunkRecordsForDegenerateFormSize() {
        std::vector<std::byte> data = makeFormHeader(2U);
        const auto payload = makeChunk(kDataId, 4U);
        data.insert(data.end(), payload.begin(), payload.end());

        MemorySource source(data);
        RiffChunkScanner scanner(source);
        const auto batch = scanner.scanBatch();

        QCOMPARE(batch.status, RiffChunkScanStatus::Complete);
        QCOMPARE(batch.records.size(), std::size_t(1));
        QCOMPARE(batch.records[0].isFormHeader, true);
        QVERIFY(scanner.finished());
    }

    void clampsTruncatedChunkAtSourceEnd() {
        // data declares 64 payload bytes but the source ends after 8 of them.
        // The clamped record covers id + size + the 8 available payload bytes.
        std::vector<std::byte> data = makeFormHeader(8U + 64U);
        appendU32Be(data, kDataId);
        appendU32Le(data, 64U);
        data.insert(data.end(), 8, std::byte{0x42});

        MemorySource source(data);
        RiffChunkScanner scanner(source);
        const auto batch = scanner.scanBatch();

        QCOMPARE(batch.status, RiffChunkScanStatus::Complete);
        QCOMPARE(batch.records.size(), std::size_t(2));
        QCOMPARE(batch.records[1].chunkId, kDataId);
        QCOMPARE(batch.records[1].declaredSize, quint64(64));
        QCOMPARE(batch.records[1].truncated, true);
        QCOMPARE(batch.records[1].terminal, true);
        QVERIFY(batch.records[1].chunkSpan.has_value());
        QCOMPARE(batch.records[1].chunkSpan->start().absoluteBitOffset(), quint64(12 * 8));
        QCOMPARE(batch.records[1].chunkSpan->bitLength(), quint64(16 * 8));
        QVERIFY(scanner.finished());
        QCOMPARE(scanner.cursor(), quint64(data.size()));
    }

    void stopsAtTheFormEndAndLeavesTrailingBytesUnclaimed() {
        // riff_size bounds the form at offset 24; the extra bytes are outside
        // the form and stay unclaimed.
        std::vector<std::byte> data = makeFormHeader(16U);
        const auto payload = makeChunk(kDataId, 4U);
        data.insert(data.end(), payload.begin(), payload.end());
        data.insert(data.end(), 16, std::byte{0x99});

        MemorySource source(data);
        RiffChunkScanner scanner(source);
        const auto batch = scanner.scanBatch();

        QCOMPARE(batch.status, RiffChunkScanStatus::Complete);
        QCOMPARE(batch.records.size(), std::size_t(2));
        QCOMPARE(batch.records[1].chunkId, kDataId);
        QCOMPARE(batch.records[1].truncated, false);
        QVERIFY(scanner.finished());
        QCOMPARE(scanner.cursor(), quint64(12 + 8 + 4));
    }

    void finishesWhenTrailingBytesCannotFrameAChunk() {
        std::vector<std::byte> data = makeFormHeader(16U + 3U);
        const auto payload = makeChunk(kDataId, 4U);
        data.insert(data.end(), payload.begin(), payload.end());
        data.insert(data.end(), 3, std::byte{0x55});

        MemorySource source(data);
        RiffChunkScanner scanner(source);
        const auto batch = scanner.scanBatch();

        QCOMPARE(batch.status, RiffChunkScanStatus::Complete);
        QCOMPARE(batch.records.size(), std::size_t(2));
        QCOMPARE(batch.records[1].truncated, false);
        QVERIFY(scanner.finished());
    }

    void batchesRecordsAcrossCalls() {
        std::vector<std::byte> data = makeFormHeader(4U + 8U + 16U + 8U + 4U);
        const auto fmt = makeChunk(kFmtId, 16U);
        const auto payload = makeChunk(kDataId, 4U);
        data.insert(data.end(), fmt.begin(), fmt.end());
        data.insert(data.end(), payload.begin(), payload.end());

        MemorySource source(data);
        RiffChunkScanner scanner(source);

        const auto first = scanner.scanBatch(1);
        QCOMPARE(first.status, RiffChunkScanStatus::InProgress);
        QCOMPARE(first.records.size(), std::size_t(1));
        QCOMPARE(first.records[0].isFormHeader, true);
        QVERIFY(!scanner.finished());

        const auto second = scanner.scanBatch(1);
        QCOMPARE(second.status, RiffChunkScanStatus::InProgress);
        QCOMPARE(second.records.size(), std::size_t(1));
        QCOMPARE(second.records[0].chunkId, kFmtId);

        const auto third = scanner.scanBatch(1);
        QCOMPARE(third.status, RiffChunkScanStatus::Complete);
        QCOMPARE(third.records.size(), std::size_t(1));
        QCOMPARE(third.records[0].chunkId, kDataId);
        QVERIFY(scanner.finished());
    }

    void propagatesSourceErrors() {
        std::vector<std::byte> data = makeFormHeader(8U + 4U);
        const auto payload = makeChunk(kDataId, 4U);
        data.insert(data.end(), payload.begin(), payload.end());

        MemorySource source(data, true);
        RiffChunkScanner scanner(source);
        const auto batch = scanner.scanBatch();

        QCOMPARE(batch.status, RiffChunkScanStatus::SourceError);
        QVERIFY(batch.errorMessage.contains(QStringLiteral("Simulated I/O read failure")));

        // The failure is sticky.
        const auto next = scanner.scanBatch();
        QCOMPARE(next.status, RiffChunkScanStatus::SourceError);
    }

    void reportsCancelledBatches() {
        std::vector<std::byte> data = makeFormHeader(8U + 4U);
        const auto payload = makeChunk(kDataId, 4U);
        data.insert(data.end(), payload.begin(), payload.end());

        CancellationSource cancellationSource;
        (void)cancellationSource.requestCancellation();

        MemorySource source(data);
        RiffChunkScanner scanner(source, cancellationSource.token());
        const auto batch = scanner.scanBatch();

        QCOMPARE(batch.status, RiffChunkScanStatus::Cancelled);
        QVERIFY(!scanner.finished());
    }

    void rejectsInvalidBatchParameters() {
        std::vector<std::byte> data = makeFormHeader(8U + 4U);
        MemorySource source(data);
        RiffChunkScanner scanner(source);

        const auto zeroRecords = scanner.scanBatch(0);
        QCOMPARE(zeroRecords.status, RiffChunkScanStatus::InvalidBatchSize);

        const auto zeroBudget = scanner.scanBatch(16, 0);
        QCOMPARE(zeroBudget.status, RiffChunkScanStatus::InvalidBatchSize);
    }
};

QTEST_GUILESS_MAIN(RiffChunkScannerTest)

#include "riff_chunk_scanner_test.moc"
