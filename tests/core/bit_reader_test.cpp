#include <streamview/core/bit_reader.h>

#include <QByteArray>
#include <QTest>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <utility>

using streamview::core::BitReadStatus;
using streamview::core::BitReader;
using streamview::core::LogicalViewId;
using streamview::core::RandomAccessSource;
using streamview::core::SourceBitAddress;
using streamview::core::SourceMapping;
using streamview::core::SourceReadResult;
using streamview::core::SourceReadStatus;
using streamview::core::SourceSpan;

namespace {

class ByteArraySource final : public RandomAccessSource {
public:
    explicit ByteArraySource(QByteArray bytes) : bytes_(std::move(bytes)) {}

    [[nodiscard]] quint64 sizeBytes() const noexcept override {
        return static_cast<quint64>(bytes_.size());
    }

    [[nodiscard]] QString identity() const override { return QStringLiteral("test-memory"); }

    [[nodiscard]] SourceReadResult
    readAt(quint64 byteOffset, std::span<std::byte> destination) const override {
        if (failReads_) {
            return {SourceReadStatus::Error, 0, QStringLiteral("injected read error")};
        }
        if (failAfterReadCount_ && readCount_ >= *failAfterReadCount_) {
            return {SourceReadStatus::Error, 0, QStringLiteral("injected later read error")};
        }
        ++readCount_;
        if (destination.empty()) {
            return {SourceReadStatus::Complete, 0, {}};
        }
        if (byteOffset >= static_cast<quint64>(bytes_.size())) {
            return {SourceReadStatus::EndOfSource, 0, {}};
        }

        const std::size_t available =
            static_cast<std::size_t>(bytes_.size()) - static_cast<std::size_t>(byteOffset);
        const std::size_t count = std::min(available, destination.size());
        std::memcpy(destination.data(),
                    bytes_.constData() + static_cast<qsizetype>(byteOffset),
                    count);
        if ((incompleteSuccess_ ||
             (incompleteAfterReadCount_ && readCount_ > *incompleteAfterReadCount_)) &&
            count > 0) {
            return {SourceReadStatus::Complete, count - 1, {}};
        }
        return {count == destination.size() ? SourceReadStatus::Complete
                                            : SourceReadStatus::EndOfSource,
                count,
                {}};
    }

    void setFailReads(bool failReads) noexcept { failReads_ = failReads; }
    void setFailAfterReadCount(std::size_t readCount) noexcept {
        failAfterReadCount_ = readCount;
    }
    void setIncompleteSuccess(bool incompleteSuccess) noexcept {
        incompleteSuccess_ = incompleteSuccess;
    }
    void setIncompleteAfterReadCount(std::size_t readCount) noexcept {
        incompleteAfterReadCount_ = readCount;
    }

private:
    QByteArray bytes_;
    bool failReads_ = false;
    std::optional<std::size_t> failAfterReadCount_;
    std::optional<std::size_t> incompleteAfterReadCount_;
    mutable std::size_t readCount_ = 0;
    bool incompleteSuccess_ = false;
};

} // namespace

class BitReaderTest final : public QObject {
    Q_OBJECT

private slots:
    void readsAcrossMappedSpansWithoutReadingTheGap() {
        ByteArraySource source(QByteArray::fromHex("ab00cd"));
        const auto firstSpan = SourceSpan::create(SourceBitAddress(0), 8);
        const auto secondSpan = SourceSpan::create(SourceBitAddress(16), 8);
        QVERIFY(firstSpan.has_value());
        QVERIFY(secondSpan.has_value());
        const auto mapping =
            SourceMapping::create(LogicalViewId(1), {*firstSpan, *secondSpan});
        QVERIFY(mapping.has_value());
        BitReader reader(source, *mapping);

        QCOMPARE(reader.logicalBitLength(), quint64{16});
        QCOMPARE(reader.backingSpans().size(), std::size_t{2});

        const auto first = reader.readBits(12);
        QVERIFY(first.complete());
        QCOMPARE(first.value, quint64{0xabc});

        const auto second = reader.readBits(4);
        QVERIFY(second.complete());
        QCOMPARE(second.value, quint64{0xd});
        QCOMPARE(reader.position(), quint64{16});
        QCOMPARE(reader.remainingBits(), quint64{0});
    }

    void readsUnalignedBitsAcrossMappedSpanBoundaries() {
        ByteArraySource source(QByteArray::fromHex("d34b"));
        const auto firstSpan = SourceSpan::create(SourceBitAddress(1), 5);
        const auto secondSpan = SourceSpan::create(SourceBitAddress(12), 4);
        QVERIFY(firstSpan.has_value());
        QVERIFY(secondSpan.has_value());
        const auto mapping =
            SourceMapping::create(LogicalViewId(1), {*firstSpan, *secondSpan});
        QVERIFY(mapping.has_value());
        BitReader reader(source, *mapping);

        const auto result = reader.readBits(9);
        QVERIFY(result.complete());
        QCOMPARE(result.value, quint64{0b101001011});
        QCOMPARE(reader.position(), quint64{9});
    }

    void readsA64BitValueAcrossMappedSpans() {
        ByteArraySource source(QByteArray::fromHex("01230045670089ab00cdef"));
        const auto firstSpan = SourceSpan::create(SourceBitAddress(0), 16);
        const auto secondSpan = SourceSpan::create(SourceBitAddress(24), 16);
        const auto thirdSpan = SourceSpan::create(SourceBitAddress(48), 16);
        const auto fourthSpan = SourceSpan::create(SourceBitAddress(72), 16);
        QVERIFY(firstSpan.has_value());
        QVERIFY(secondSpan.has_value());
        QVERIFY(thirdSpan.has_value());
        QVERIFY(fourthSpan.has_value());
        const auto mapping = SourceMapping::create(
            LogicalViewId(1), {*firstSpan, *secondSpan, *thirdSpan, *fourthSpan});
        QVERIFY(mapping.has_value());
        BitReader reader(source, *mapping);

        const auto result = reader.readBits(64);
        QVERIFY(result.complete());
        QCOMPARE(result.value, quint64{0x0123456789abcdefULL});
        QCOMPARE(reader.position(), quint64{64});
    }

    void doesNotAdvanceWhenALaterMappedSpanFails() {
        ByteArraySource source(QByteArray::fromHex("ab00cd"));
        source.setFailAfterReadCount(1);
        const auto firstSpan = SourceSpan::create(SourceBitAddress(0), 8);
        const auto secondSpan = SourceSpan::create(SourceBitAddress(16), 8);
        QVERIFY(firstSpan.has_value());
        QVERIFY(secondSpan.has_value());
        const auto mapping =
            SourceMapping::create(LogicalViewId(1), {*firstSpan, *secondSpan});
        QVERIFY(mapping.has_value());
        BitReader reader(source, *mapping);

        const auto result = reader.readBits(16);
        QVERIFY(result.status == BitReadStatus::SourceError);
        QCOMPARE(result.errorMessage, QStringLiteral("injected later read error"));
        QCOMPARE(reader.position(), quint64{0});
    }

    void doesNotAdvanceWhenALaterMappedSpanReachesEndOfSource() {
        ByteArraySource source(QByteArray::fromHex("ab"));
        const auto firstSpan = SourceSpan::create(SourceBitAddress(0), 8);
        const auto secondSpan = SourceSpan::create(SourceBitAddress(16), 8);
        QVERIFY(firstSpan.has_value());
        QVERIFY(secondSpan.has_value());
        const auto mapping =
            SourceMapping::create(LogicalViewId(1), {*firstSpan, *secondSpan});
        QVERIFY(mapping.has_value());
        BitReader reader(source, *mapping);

        const auto result = reader.readBits(16);
        QVERIFY(result.status == BitReadStatus::EndOfSource);
        QCOMPARE(reader.position(), quint64{0});
    }

    void rejectsAnIncompleteSuccessfulReadInALaterMappedSpan() {
        ByteArraySource source(QByteArray::fromHex("ab00cd"));
        source.setIncompleteAfterReadCount(1);
        const auto firstSpan = SourceSpan::create(SourceBitAddress(0), 8);
        const auto secondSpan = SourceSpan::create(SourceBitAddress(16), 8);
        QVERIFY(firstSpan.has_value());
        QVERIFY(secondSpan.has_value());
        const auto mapping =
            SourceMapping::create(LogicalViewId(1), {*firstSpan, *secondSpan});
        QVERIFY(mapping.has_value());
        BitReader reader(source, *mapping);

        const auto result = reader.readBits(16);
        QVERIFY(result.status == BitReadStatus::SourceError);
        QCOMPARE(result.errorMessage,
                 QStringLiteral("Source reported an incomplete successful read"));
        QCOMPARE(reader.position(), quint64{0});
    }

    void seeksAndBoundsReadsInMappedLogicalOffsets() {
        ByteArraySource source(QByteArray::fromHex("ab00cd"));
        const auto firstSpan = SourceSpan::create(SourceBitAddress(0), 8);
        const auto secondSpan = SourceSpan::create(SourceBitAddress(16), 8);
        QVERIFY(firstSpan.has_value());
        QVERIFY(secondSpan.has_value());
        const auto mapping =
            SourceMapping::create(LogicalViewId(1), {*firstSpan, *secondSpan});
        QVERIFY(mapping.has_value());
        BitReader reader(source, *mapping);

        QVERIFY(!reader.seek(17));
        QCOMPARE(reader.position(), quint64{0});
        QVERIFY(reader.seek(8));
        QVERIFY(reader.readBits(9).status == BitReadStatus::EndOfRange);
        QCOMPARE(reader.position(), quint64{8});

        const auto result = reader.readBits(8);
        QVERIFY(result.complete());
        QCOMPARE(result.value, quint64{0xcd});
        QCOMPARE(reader.position(), quint64{16});
    }

    void createsAReaderFromAMappedLogicalSlice() {
        ByteArraySource source(QByteArray::fromHex("f000aa00cc"));
        const auto firstSpan = SourceSpan::create(SourceBitAddress(0), 8);
        const auto secondSpan = SourceSpan::create(SourceBitAddress(16), 8);
        const auto thirdSpan = SourceSpan::create(SourceBitAddress(32), 8);
        QVERIFY(firstSpan.has_value());
        QVERIFY(secondSpan.has_value());
        QVERIFY(thirdSpan.has_value());
        const auto mapping = SourceMapping::create(
            LogicalViewId(1), {*firstSpan, *secondSpan, *thirdSpan});
        QVERIFY(mapping.has_value());

        auto reader = BitReader::fromMappingSlice(source, *mapping, 4, 16);
        QVERIFY(reader.has_value());
        QCOMPARE(reader->logicalBitLength(), quint64{16});
        QCOMPARE(reader->backingSpans().size(), std::size_t{3});
        QCOMPARE(reader->backingSpans().at(0).start().absoluteBitOffset(), quint64{4});
        QCOMPARE(reader->backingSpans().at(0).bitLength(), quint64{4});
        QCOMPARE(reader->backingSpans().at(1).start().absoluteBitOffset(), quint64{16});
        QCOMPARE(reader->backingSpans().at(1).bitLength(), quint64{8});
        QCOMPARE(reader->backingSpans().at(2).start().absoluteBitOffset(), quint64{32});
        QCOMPARE(reader->backingSpans().at(2).bitLength(), quint64{4});

        const auto result = reader->readBits(16);
        QVERIFY(result.complete());
        QCOMPARE(result.value, quint64{0x0aac});
    }

    void createsAZeroLengthReaderFromAnEmptyMapping() {
        ByteArraySource source(QByteArray{});
        const auto mapping = SourceMapping::create(LogicalViewId(1), {});
        QVERIFY(mapping.has_value());
        BitReader reader(source, *mapping);

        QCOMPARE(reader.logicalBitLength(), quint64{0});
        QVERIFY(reader.backingSpans().empty());
        QCOMPARE(reader.remainingBits(), quint64{0});
        QVERIFY(reader.seek(0));
        QVERIFY(reader.readBits(1).status == BitReadStatus::EndOfRange);
        QCOMPARE(reader.position(), quint64{0});
    }

    void rejectsInvalidMappedLogicalSlices() {
        ByteArraySource source(QByteArray::fromHex("abcd"));
        const auto span = SourceSpan::create(SourceBitAddress(0), 16);
        QVERIFY(span.has_value());
        const auto mapping = SourceMapping::create(LogicalViewId(1), {*span});
        QVERIFY(mapping.has_value());

        const auto emptyAtEnd = BitReader::fromMappingSlice(source, *mapping, 16, 0);
        QVERIFY(emptyAtEnd.has_value());
        QCOMPARE(emptyAtEnd->logicalBitLength(), quint64{0});
        QVERIFY(emptyAtEnd->backingSpans().empty());

        QVERIFY(!BitReader::fromMappingSlice(source, *mapping, 17, 0).has_value());
        QVERIFY(!BitReader::fromMappingSlice(source, *mapping, 8, 9).has_value());
        QVERIFY(!BitReader::fromMappingSlice(source,
                                             *mapping,
                                             std::numeric_limits<quint64>::max(),
                                             1)
                     .has_value());
    }

    void readsMostSignificantBitsFirst() {
        ByteArraySource source(QByteArray::fromHex("b26c"));
        const auto range = SourceSpan::create(SourceBitAddress(0), 16);
        QVERIFY(range.has_value());
        BitReader reader(source, *range);

        QCOMPARE(reader.logicalBitLength(), quint64{16});
        QCOMPARE(reader.backingSpans().size(), std::size_t{1});
        QCOMPARE(reader.backingSpans().front().start().absoluteBitOffset(), quint64{0});
        QCOMPARE(reader.backingSpans().front().bitLength(), quint64{16});

        const auto first = reader.readBits(3);
        QVERIFY(first.complete());
        QCOMPARE(first.value, quint64{0b101});
        QCOMPARE(first.bitCount, quint8{3});

        const auto second = reader.readBits(5);
        QVERIFY(second.complete());
        QCOMPARE(second.value, quint64{0b10010});

        const auto third = reader.readBits(8);
        QVERIFY(third.complete());
        QCOMPARE(third.value, quint64{0x6c});
        QCOMPARE(reader.position(), quint64{16});
        QCOMPARE(reader.remainingBits(), quint64{0});
    }

    void readsAnUnaligned64BitValue() {
        ByteArraySource source(QByteArray::fromHex("0123456789abcdef80"));
        const auto range = SourceSpan::create(SourceBitAddress(4), 64);
        QVERIFY(range.has_value());
        BitReader reader(source, *range);

        const auto result = reader.readBits(64);
        QVERIFY(result.complete());
        QCOMPARE(result.value, quint64{0x123456789abcdef8ULL});
        QCOMPARE(reader.position(), quint64{64});
    }

    void doesNotAdvanceOnInvalidOrBoundedReads() {
        ByteArraySource source(QByteArray::fromHex("ff"));
        const auto range = SourceSpan::create(SourceBitAddress(0), 8);
        QVERIFY(range.has_value());
        BitReader reader(source, *range);

        QVERIFY(reader.readBits(0).status == BitReadStatus::InvalidBitCount);
        QVERIFY(reader.readBits(65).status == BitReadStatus::InvalidBitCount);
        QVERIFY(reader.readBits(9).status == BitReadStatus::EndOfRange);
        QCOMPARE(reader.position(), quint64{0});
        QVERIFY(!reader.seek(9));
        QVERIFY(reader.seek(8));
        QCOMPARE(reader.position(), quint64{8});
    }

    void doesNotAdvanceWhenTheSourceIsTruncated() {
        ByteArraySource source(QByteArray::fromHex("aa"));
        const auto range = SourceSpan::create(SourceBitAddress(0), 16);
        QVERIFY(range.has_value());
        BitReader reader(source, *range);

        const auto result = reader.readBits(12);
        QVERIFY(result.status == BitReadStatus::EndOfSource);
        QCOMPARE(reader.position(), quint64{0});
    }

    void doesNotAdvanceOnSourceErrors() {
        ByteArraySource source(QByteArray::fromHex("aa"));
        source.setFailReads(true);
        const auto range = SourceSpan::create(SourceBitAddress(0), 8);
        QVERIFY(range.has_value());
        BitReader reader(source, *range);

        const auto result = reader.readBits(1);
        QVERIFY(result.status == BitReadStatus::SourceError);
        QCOMPARE(result.errorMessage, QStringLiteral("injected read error"));
        QCOMPARE(reader.position(), quint64{0});
    }

    void rejectsInconsistentSuccessfulReads() {
        ByteArraySource source(QByteArray::fromHex("aa"));
        source.setIncompleteSuccess(true);
        const auto range = SourceSpan::create(SourceBitAddress(0), 8);
        QVERIFY(range.has_value());
        BitReader reader(source, *range);

        const auto result = reader.readBits(1);
        QVERIFY(result.status == BitReadStatus::SourceError);
        QCOMPARE(reader.position(), quint64{0});
    }
};

QTEST_GUILESS_MAIN(BitReaderTest)

#include "bit_reader_test.moc"
