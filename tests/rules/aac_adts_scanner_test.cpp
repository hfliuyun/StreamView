#include <streamview/rules/aac_adts_scanner.h>
#include <streamview/core/cancellation.h>
#include <streamview/core/source.h>

#include <QObject>
#include <QTest>

#include <algorithm>
#include <cstddef>
#include <vector>

using streamview::core::CancellationSource;
using streamview::core::RandomAccessSource;
using streamview::core::SourceReadResult;
using streamview::core::SourceReadStatus;
using streamview::rules::AacAdtsRecord;
using streamview::rules::AacAdtsScanBatch;
using streamview::rules::AacAdtsScanner;
using streamview::rules::AacAdtsScanStatus;

namespace {

class MemorySource final : public RandomAccessSource {
public:
    explicit MemorySource(std::vector<std::byte> data) : data_(std::move(data)) {}

    [[nodiscard]] quint64 sizeBytes() const noexcept override {
        return static_cast<quint64>(data_.size());
    }
    [[nodiscard]] QString identity() const override { return QStringLiteral("memory"); }

    [[nodiscard]] SourceReadResult
    readAt(quint64 byteOffset, std::span<std::byte> destination) const override {
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

private:
    std::vector<std::byte> data_;
};

[[nodiscard]] std::vector<std::byte> makeAdtsFrame(quint16 frameLength,
                                                    bool protectionAbsent = true,
                                                    quint8 profile = 1,
                                                    quint8 samplingFreqIndex = 4,
                                                    quint8 channelConfig = 2,
                                                    std::byte fillByte = std::byte{0xAB}) {
    const std::size_t headerLen = protectionAbsent ? 7U : 9U;
    std::vector<std::byte> frame(frameLength, fillByte);
    if (frameLength < headerLen) {
        return frame;
    }
    // Byte 0: 0xFF
    frame[0] = std::byte{0xFF};
    // Byte 1: 0xF0 | (id=0 << 3) | (layer=0 << 1) | (protectionAbsent ? 1 : 0)
    frame[1] = std::byte{static_cast<quint8>(0xF0U | (protectionAbsent ? 1U : 0U))};
    // Byte 2: (profile << 6) | (samplingFreqIndex << 2) | (private_bit=0) | (channelConfig >> 2)
    frame[2] = std::byte{static_cast<quint8>(
        ((profile & 0x03U) << 6U) |
        ((samplingFreqIndex & 0x0FU) << 2U) |
        ((channelConfig >> 2U) & 0x01U))};
    // Byte 3: ((channelConfig & 0x03) << 6) | (copy=0) | (home=0) | (copy_id=0) | (copy_start=0) | (frameLength >> 11)
    frame[3] = std::byte{static_cast<quint8>(
        ((channelConfig & 0x03U) << 6U) |
        ((frameLength >> 11U) & 0x03U))};
    // Byte 4: (frameLength >> 3) & 0xFF
    frame[4] = std::byte{static_cast<quint8>((frameLength >> 3U) & 0xFFU)};
    // Byte 5: ((frameLength & 0x07) << 5) | (buffer_fullness=0x7FF >> 6)
    frame[5] = std::byte{static_cast<quint8>(
        ((frameLength & 0x07U) << 5U) | 0x1FU)};
    // Byte 6: ((buffer_fullness=0x7FF & 0x3F) << 2) | (raw_blocks=0)
    frame[6] = std::byte{0xFC};
    if (!protectionAbsent) {
        frame[7] = std::byte{0x12};
        frame[8] = std::byte{0x34};
    }
    return frame;
}

} // namespace

class AacAdtsScannerTest : public QObject {
    Q_OBJECT

private slots:
    void scansValidConsecutiveAdtsFrames() {
        std::vector<std::byte> stream;
        const auto f1 = makeAdtsFrame(150, true);
        const auto f2 = makeAdtsFrame(200, true);
        const auto f3 = makeAdtsFrame(180, true);
        stream.insert(stream.end(), f1.begin(), f1.end());
        stream.insert(stream.end(), f2.begin(), f2.end());
        stream.insert(stream.end(), f3.begin(), f3.end());

        const MemorySource source(std::move(stream));
        AacAdtsScanner scanner(source);

        const auto batch = scanner.scanBatch();
        QVERIFY(batch.complete());
        QCOMPARE(batch.records.size(), std::size_t(3));

        // Frame 1
        QCOMPARE(batch.records[0].frameOffset, quint64(0));
        QCOMPARE(batch.records[0].aacFrameLength, quint16(150));
        QCOMPARE(batch.records[0].headerLength, quint64(7));
        QCOMPARE(batch.records[0].payloadLength, quint64(143));
        QCOMPARE(batch.records[0].crcPresent, false);
        QCOMPARE(batch.records[0].truncated, false);
        QCOMPARE(batch.records[0].frameSpan->bitLength(), quint64(150 * 8));
        QCOMPARE(batch.records[0].headerSpan->bitLength(), quint64(7 * 8));
        QCOMPARE(batch.records[0].payloadSpan->bitLength(), quint64(143 * 8));

        // Frame 2
        QCOMPARE(batch.records[1].frameOffset, quint64(150));
        QCOMPARE(batch.records[1].aacFrameLength, quint16(200));
        QCOMPARE(batch.records[1].headerLength, quint64(7));
        QCOMPARE(batch.records[1].payloadLength, quint64(193));
        QCOMPARE(batch.records[1].crcPresent, false);
        QCOMPARE(batch.records[1].truncated, false);

        // Frame 3
        QCOMPARE(batch.records[2].frameOffset, quint64(350));
        QCOMPARE(batch.records[2].aacFrameLength, quint16(180));
        QCOMPARE(batch.records[2].headerLength, quint64(7));
        QCOMPARE(batch.records[2].payloadLength, quint64(173));
        QCOMPARE(batch.records[2].crcPresent, false);
        QCOMPARE(batch.records[2].truncated, false);

        QVERIFY(scanner.finished());
    }

    void scansValidAdtsFramesWithCrc() {
        std::vector<std::byte> stream;
        const auto f1 = makeAdtsFrame(120, false);
        const auto f2 = makeAdtsFrame(160, false);
        stream.insert(stream.end(), f1.begin(), f1.end());
        stream.insert(stream.end(), f2.begin(), f2.end());

        const MemorySource source(std::move(stream));
        AacAdtsScanner scanner(source);

        const auto batch = scanner.scanBatch();
        QVERIFY(batch.complete());
        QCOMPARE(batch.records.size(), std::size_t(2));

        QCOMPARE(batch.records[0].frameOffset, quint64(0));
        QCOMPARE(batch.records[0].aacFrameLength, quint16(120));
        QCOMPARE(batch.records[0].headerLength, quint64(9));
        QCOMPARE(batch.records[0].payloadLength, quint64(111));
        QCOMPARE(batch.records[0].crcPresent, true);
        QCOMPARE(batch.records[0].truncated, false);
        QCOMPARE(batch.records[0].headerSpan->bitLength(), quint64(9 * 8));
        QCOMPARE(batch.records[0].payloadSpan->bitLength(), quint64(111 * 8));

        QCOMPARE(batch.records[1].frameOffset, quint64(120));
        QCOMPARE(batch.records[1].aacFrameLength, quint16(160));
        QCOMPARE(batch.records[1].headerLength, quint64(9));
        QCOMPARE(batch.records[1].payloadLength, quint64(151));
        QCOMPARE(batch.records[1].crcPresent, true);
        QCOMPARE(batch.records[1].truncated, false);
    }

    void resynchronizesAcrossLeadingGarbageBytes() {
        std::vector<std::byte> stream(100, std::byte{0xEE}); // 100 bytes garbage
        const auto f1 = makeAdtsFrame(100, true);
        const auto f2 = makeAdtsFrame(120, true);
        stream.insert(stream.end(), f1.begin(), f1.end());
        stream.insert(stream.end(), f2.begin(), f2.end());

        const MemorySource source(std::move(stream));
        AacAdtsScanner scanner(source);

        const auto batch = scanner.scanBatch();
        QVERIFY(batch.complete());
        QCOMPARE(batch.records.size(), std::size_t(2));
        QCOMPARE(batch.records[0].frameOffset, quint64(100));
        QCOMPARE(batch.records[1].frameOffset, quint64(200));
    }

    void handlesTruncatedFrameAtEof() {
        auto f1 = makeAdtsFrame(200, true);
        f1.resize(100); // truncated to 100 bytes

        const MemorySource source(std::move(f1));
        AacAdtsScanner scanner(source);

        const auto batch = scanner.scanBatch();
        QVERIFY(batch.complete());
        QCOMPARE(batch.records.size(), std::size_t(1));
        QCOMPARE(batch.records[0].frameOffset, quint64(0));
        QCOMPARE(batch.records[0].aacFrameLength, quint16(200));
        QCOMPARE(batch.records[0].headerLength, quint64(7));
        QCOMPARE(batch.records[0].payloadLength, quint64(93));
        QCOMPARE(batch.records[0].truncated, true);
        QCOMPARE(batch.records[0].frameSpan->bitLength(), quint64(100 * 8));
        QCOMPARE(batch.records[0].payloadSpan->bitLength(), quint64(93 * 8));
        QVERIFY(scanner.finished());
    }

    void resynchronizesAfterCorruptedFrameInLengthChain() {
        std::vector<std::byte> stream;
        const auto f1 = makeAdtsFrame(100, true);
        std::vector<std::byte> corrupt(50, std::byte{0x77}); // corrupted block
        const auto f2 = makeAdtsFrame(120, true);
        const auto f3 = makeAdtsFrame(130, true);

        stream.insert(stream.end(), f1.begin(), f1.end());
        stream.insert(stream.end(), corrupt.begin(), corrupt.end());
        stream.insert(stream.end(), f2.begin(), f2.end());
        stream.insert(stream.end(), f3.begin(), f3.end());

        const MemorySource source(std::move(stream));
        AacAdtsScanner scanner(source);

        const auto batch = scanner.scanBatch();
        QVERIFY(batch.complete());
        QCOMPARE(batch.records.size(), std::size_t(3));
        QCOMPARE(batch.records[0].frameOffset, quint64(0));
        QCOMPARE(batch.records[1].frameOffset, quint64(150));
        QCOMPARE(batch.records[2].frameOffset, quint64(270));
    }

    void rejectsFalseSyncwordsInsidePayload() {
        std::vector<std::byte> stream;
        auto f1 = makeAdtsFrame(200, true);
        // Inject false syncword 0xFFF inside payload of f1 at byte offset 50
        f1[50] = std::byte{0xFF};
        f1[51] = std::byte{0xF1};
        f1[52] = std::byte{0x50};
        f1[53] = std::byte{0x80};
        f1[54] = std::byte{0x10}; // length = 32
        f1[55] = std::byte{0x1F};
        f1[56] = std::byte{0xFC};
        // But offset 50 + 32 = 82 does not have a valid syncword! Lookahead will reject it.

        const auto f2 = makeAdtsFrame(150, true);
        stream.insert(stream.end(), f1.begin(), f1.end());
        stream.insert(stream.end(), f2.begin(), f2.end());

        const MemorySource source(std::move(stream));
        AacAdtsScanner scanner(source);

        const auto batch = scanner.scanBatch();
        QVERIFY(batch.complete());
        QCOMPARE(batch.records.size(), std::size_t(2));
        QCOMPARE(batch.records[0].frameOffset, quint64(0));
        QCOMPARE(batch.records[0].aacFrameLength, quint16(200));
        QCOMPARE(batch.records[1].frameOffset, quint64(200));
        QCOMPARE(batch.records[1].aacFrameLength, quint16(150));
    }

    void respectsBatchLimitsAndWorkBudget() {
        std::vector<std::byte> stream;
        const auto f1 = makeAdtsFrame(100, true);
        const auto f2 = makeAdtsFrame(100, true);
        const auto f3 = makeAdtsFrame(100, true);
        stream.insert(stream.end(), f1.begin(), f1.end());
        stream.insert(stream.end(), f2.begin(), f2.end());
        stream.insert(stream.end(), f3.begin(), f3.end());

        const MemorySource source(std::move(stream));
        AacAdtsScanner scanner(source);

        const auto b1 = scanner.scanBatch(1);
        QCOMPARE(b1.status, AacAdtsScanStatus::InProgress);
        QCOMPARE(b1.records.size(), std::size_t(1));
        QCOMPARE(b1.records[0].frameOffset, quint64(0));

        const auto b2 = scanner.scanBatch(1);
        QCOMPARE(b2.status, AacAdtsScanStatus::InProgress);
        QCOMPARE(b2.records.size(), std::size_t(1));
        QCOMPARE(b2.records[0].frameOffset, quint64(100));

        const auto b3 = scanner.scanBatch(1);
        QCOMPARE(b3.status, AacAdtsScanStatus::Complete);
        QCOMPARE(b3.records.size(), std::size_t(1));
        QCOMPARE(b3.records[0].frameOffset, quint64(200));
        QVERIFY(scanner.finished());
    }

    void respectsCancellationAndResumesInPlace() {
        std::vector<std::byte> stream;
        for (int i = 0; i < 50; ++i) {
            const auto f = makeAdtsFrame(100, true);
            stream.insert(stream.end(), f.begin(), f.end());
        }

        const MemorySource source(std::move(stream));
        CancellationSource cancelSource;
        AacAdtsScanner scanner(source, cancelSource.token());

        (void)cancelSource.requestCancellation();
        const auto batch = scanner.scanBatch();
        QCOMPARE(batch.status, AacAdtsScanStatus::Cancelled);

        // Resume after clearing cancellation
        CancellationSource newCancelSource;
        scanner.replaceCancellationToken(newCancelSource.token());
        const auto resumeBatch = scanner.scanBatch();
        QCOMPARE(resumeBatch.status, AacAdtsScanStatus::Complete);
        QVERIFY(!resumeBatch.records.empty());
        QVERIFY(scanner.finished());
    }

    void handlesEmptyAndSmallSources() {
        const MemorySource emptySource(std::vector<std::byte>{});
        AacAdtsScanner emptyScanner(emptySource);
        const auto b1 = emptyScanner.scanBatch();
        QVERIFY(b1.complete());
        QCOMPARE(b1.records.size(), std::size_t(0));

        const MemorySource smallSource(std::vector<std::byte>{std::byte{0xFF}, std::byte{0xF1}});
        AacAdtsScanner smallScanner(smallSource);
        const auto b2 = smallScanner.scanBatch();
        QVERIFY(b2.complete());
        QCOMPARE(b2.records.size(), std::size_t(0));
    }

    void handlesInvalidBatchArguments() {
        const MemorySource source(std::vector<std::byte>{std::byte{0x00}});
        AacAdtsScanner scanner(source);
        const auto b1 = scanner.scanBatch(0, 100);
        QCOMPARE(b1.status, AacAdtsScanStatus::InvalidBatchSize);
        const auto b2 = scanner.scanBatch(10, 0);
        QCOMPARE(b2.status, AacAdtsScanStatus::InvalidBatchSize);
    }
};

QTEST_MAIN(AacAdtsScannerTest)
#include "aac_adts_scanner_test.moc"
