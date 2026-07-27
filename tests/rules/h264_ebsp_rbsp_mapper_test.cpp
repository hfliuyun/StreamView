#include <streamview/core/bit_reader.h>
#include <streamview/rules/h264_ebsp_rbsp_mapper.h>

#include <QTest>

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <span>
#include <utility>
#include <vector>

using streamview::core::BitReader;
using streamview::core::CancellationSource;
using streamview::core::LogicalViewId;
using streamview::core::RandomAccessSource;
using streamview::core::SourceBitAddress;
using streamview::core::SourceReadResult;
using streamview::core::SourceReadStatus;
using streamview::core::SourceSpan;
using streamview::rules::H264EbspRbspIssueKind;
using streamview::rules::H264EbspRbspMapLimits;
using streamview::rules::H264EbspRbspMapper;
using streamview::rules::H264EbspRbspMapStatus;

namespace {

[[nodiscard]] std::vector<std::byte> bytes(std::initializer_list<unsigned int> values) {
    std::vector<std::byte> result;
    result.reserve(values.size());
    for (const unsigned int value : values) {
        result.push_back(static_cast<std::byte>(value));
    }
    return result;
}

[[nodiscard]] SourceSpan span(quint64 bitOffset, quint64 bitLength) {
    return *SourceSpan::create(SourceBitAddress(bitOffset), bitLength);
}

void compareSpan(const SourceSpan& actual, quint64 bitOffset, quint64 bitLength) {
    QCOMPARE(actual.start().absoluteBitOffset(), bitOffset);
    QCOMPARE(actual.bitLength(), bitLength);
}

class ScriptedSource final : public RandomAccessSource {
public:
    enum class Behavior : quint8 {
        Normal,
        FailSecondWindow,
        IncompleteSuccess,
        PrematureEnd,
        CancelOnRead,
    };

    explicit ScriptedSource(std::vector<std::byte> data,
                            Behavior behavior = Behavior::Normal,
                            CancellationSource* cancellation = nullptr)
        : data_(std::move(data)), behavior_(behavior), cancellation_(cancellation) {}

    [[nodiscard]] quint64 sizeBytes() const noexcept override {
        return static_cast<quint64>(data_.size());
    }
    [[nodiscard]] QString identity() const override { return QStringLiteral("scripted"); }

    [[nodiscard]] SourceReadResult
    readAt(quint64 byteOffset, std::span<std::byte> destination) const override {
        ++readCount_;
        if (behavior_ == Behavior::CancelOnRead && cancellation_ != nullptr) {
            (void)cancellation_->requestCancellation();
        }
        if (behavior_ == Behavior::FailSecondWindow && byteOffset >= 64U * 1024U) {
            return {SourceReadStatus::Error, 0, QStringLiteral("injected read failure")};
        }
        if (behavior_ == Behavior::PrematureEnd) {
            return {SourceReadStatus::EndOfSource, 0, {}};
        }
        if (destination.empty()) {
            return {SourceReadStatus::Complete, 0, {}};
        }
        if (byteOffset >= static_cast<quint64>(data_.size())) {
            return {SourceReadStatus::EndOfSource, 0, {}};
        }

        const auto offset = static_cast<std::size_t>(byteOffset);
        const std::size_t count = std::min(destination.size(), data_.size() - offset);
        std::copy_n(data_.begin() + static_cast<std::ptrdiff_t>(offset),
                    static_cast<std::ptrdiff_t>(count),
                    destination.begin());
        if (behavior_ == Behavior::IncompleteSuccess) {
            return {SourceReadStatus::Complete, count - 1U, {}};
        }
        return {count == destination.size() ? SourceReadStatus::Complete
                                            : SourceReadStatus::EndOfSource,
                count,
                {}};
    }

    [[nodiscard]] std::size_t readCount() const noexcept { return readCount_; }

private:
    std::vector<std::byte> data_;
    Behavior behavior_ = Behavior::Normal;
    CancellationSource* cancellation_ = nullptr;
    mutable std::size_t readCount_ = 0;
};

} // namespace

class H264EbspRbspMapperTest final : public QObject {
    Q_OBJECT

private slots:
    void mapsEmptyAndContiguousPayloads() {
        ScriptedSource source(bytes({0xFF, 0x12, 0x34}));

        H264EbspRbspMapper empty(source, LogicalViewId(1), span(8, 0));
        const auto emptyResult = empty.mapBatch();
        QCOMPARE(emptyResult.status, H264EbspRbspMapStatus::Complete);
        QCOMPARE(empty.sourceCursor(), quint64(1));
        QCOMPARE(empty.mapping().logicalBitLength(), quint64(0));
        QVERIFY(empty.mapping().sourceSpans().empty());
        QVERIFY(empty.excludedSpans().empty());
        QVERIFY(empty.issues().empty());

        H264EbspRbspMapper mapper(source, LogicalViewId(2), span(8, 16));
        const auto result = mapper.mapBatch();
        QCOMPARE(result.status, H264EbspRbspMapStatus::Complete);
        QVERIFY(mapper.finished());
        QCOMPARE(mapper.sourceCursor(), quint64(3));
        QCOMPARE(mapper.rbspLogicalBitLength(), quint64(16));
        QCOMPARE(mapper.mapping().viewId(), LogicalViewId(2));
        QCOMPARE(mapper.mapping().sourceSpans().size(), std::size_t(1));
        compareSpan(mapper.mapping().sourceSpans().front(), 8, 16);

        const auto replay = mapper.mapBatch(0);
        QCOMPARE(replay.status, H264EbspRbspMapStatus::Complete);
        QCOMPARE(source.readCount(), std::size_t(1));
    }

    void mapsEscapeAndReadsAcrossTheSourceGap() {
        ScriptedSource source(bytes({0x12, 0x00, 0x00, 0x03, 0x02}));
        H264EbspRbspMapper mapper(source, LogicalViewId(7), span(0, 40));

        const auto result = mapper.mapBatch();

        QCOMPARE(result.status, H264EbspRbspMapStatus::Complete);
        QCOMPARE(mapper.rbspLogicalBitLength(), quint64(32));
        QCOMPARE(mapper.mapping().sourceSpans().size(), std::size_t(2));
        compareSpan(mapper.mapping().sourceSpans().at(0), 0, 24);
        compareSpan(mapper.mapping().sourceSpans().at(1), 32, 8);
        QCOMPARE(mapper.excludedSpans().size(), std::size_t(1));
        compareSpan(mapper.excludedSpans().front().sourceSpan, 24, 8);
        QCOMPARE(mapper.excludedSpans().front().rbspBitOffset, quint64(24));
        QVERIFY(mapper.issues().empty());

        BitReader reader(source, mapper.mapping());
        const auto read = reader.readBits(32);
        QVERIFY(read.complete());
        QCOMPARE(read.value, quint64(0x12000002));
    }

    void mapsMultipleAndTerminalEscapes() {
        ScriptedSource source(bytes({0x00, 0x00, 0x03, 0x03,
                                     0x00, 0x00, 0x03}));
        H264EbspRbspMapper mapper(source, LogicalViewId(1), span(0, 56));

        QCOMPARE(mapper.mapBatch().status, H264EbspRbspMapStatus::Complete);
        QCOMPARE(mapper.rbspLogicalBitLength(), quint64(40));
        QCOMPARE(mapper.mapping().sourceSpans().size(), std::size_t(2));
        compareSpan(mapper.mapping().sourceSpans().at(0), 0, 16);
        compareSpan(mapper.mapping().sourceSpans().at(1), 24, 24);
        QCOMPARE(mapper.excludedSpans().size(), std::size_t(2));
        compareSpan(mapper.excludedSpans().at(0).sourceSpan, 16, 8);
        QCOMPARE(mapper.excludedSpans().at(0).rbspBitOffset, quint64(16));
        compareSpan(mapper.excludedSpans().at(1).sourceSpan, 48, 8);
        QCOMPARE(mapper.excludedSpans().at(1).rbspBitOffset, quint64(40));
        QVERIFY(mapper.issues().empty());
    }

    void resetsTheZeroRunAfterAnEscape() {
        ScriptedSource source(bytes({0x00, 0x00, 0x03, 0x00, 0x03}));
        H264EbspRbspMapper mapper(source, LogicalViewId(1), span(0, 40));

        QCOMPARE(mapper.mapBatch().status, H264EbspRbspMapStatus::Complete);
        QCOMPARE(mapper.mapping().sourceSpans().size(), std::size_t(2));
        compareSpan(mapper.mapping().sourceSpans().at(0), 0, 16);
        compareSpan(mapper.mapping().sourceSpans().at(1), 24, 16);
        QCOMPARE(mapper.excludedSpans().size(), std::size_t(1));
        QVERIFY(mapper.issues().empty());
    }

    void doesNotMatchBytesBeforeTheInputSpan() {
        ScriptedSource source(bytes({0x00, 0x00, 0x03, 0x00, 0x01}));
        H264EbspRbspMapper mapper(source, LogicalViewId(1), span(16, 24));

        QCOMPARE(mapper.mapBatch().status, H264EbspRbspMapStatus::Complete);
        QCOMPARE(mapper.mapping().sourceSpans().size(), std::size_t(1));
        compareSpan(mapper.mapping().sourceSpans().front(), 16, 24);
        QVERIFY(mapper.excludedSpans().empty());
        QVERIFY(mapper.issues().empty());
    }

    void reportsViolationsWithoutChangingTheTransformation() {
        ScriptedSource fourByteSource(bytes({0x00, 0x00, 0x03, 0x04}));
        H264EbspRbspMapper fourByteMapper(
            fourByteSource, LogicalViewId(1), span(0, 32));

        QCOMPARE(fourByteMapper.mapBatch().status, H264EbspRbspMapStatus::Complete);
        QCOMPARE(fourByteMapper.mapping().sourceSpans().size(), std::size_t(2));
        compareSpan(fourByteMapper.mapping().sourceSpans().at(0), 0, 16);
        compareSpan(fourByteMapper.mapping().sourceSpans().at(1), 24, 8);
        QCOMPARE(fourByteMapper.excludedSpans().size(), std::size_t(1));
        QCOMPARE(fourByteMapper.issues().size(), std::size_t(1));
        QCOMPARE(fourByteMapper.issues().front().kind,
                 H264EbspRbspIssueKind::Prohibited000003xx);
        compareSpan(fourByteMapper.issues().front().sourceSpan, 0, 32);

        ScriptedSource zeroSource(bytes({0x00, 0x00, 0x00, 0x00}));
        H264EbspRbspMapper zeroMapper(zeroSource, LogicalViewId(2), span(0, 32));
        QCOMPARE(zeroMapper.mapBatch().status, H264EbspRbspMapStatus::Complete);
        QCOMPARE(zeroMapper.mapping().sourceSpans().size(), std::size_t(1));
        compareSpan(zeroMapper.mapping().sourceSpans().front(), 0, 32);
        QCOMPARE(zeroMapper.issues().size(), std::size_t(3));
        QCOMPARE(zeroMapper.issues().at(0).kind,
                 H264EbspRbspIssueKind::Prohibited000000);
        compareSpan(zeroMapper.issues().at(0).sourceSpan, 0, 24);
        QCOMPARE(zeroMapper.issues().at(1).kind,
                 H264EbspRbspIssueKind::Prohibited000000);
        compareSpan(zeroMapper.issues().at(1).sourceSpan, 8, 24);
        QCOMPARE(zeroMapper.issues().at(2).kind, H264EbspRbspIssueKind::FinalZeroByte);
        compareSpan(zeroMapper.issues().at(2).sourceSpan, 24, 8);

        ScriptedSource otherSource(
            bytes({0x00, 0x00, 0x01, 0x7F, 0x00, 0x00, 0x02}));
        H264EbspRbspMapper otherMapper(otherSource, LogicalViewId(3), span(0, 56));
        QCOMPARE(otherMapper.mapBatch().status, H264EbspRbspMapStatus::Complete);
        QCOMPARE(otherMapper.mapping().sourceSpans().size(), std::size_t(1));
        compareSpan(otherMapper.mapping().sourceSpans().front(), 0, 56);
        QCOMPARE(otherMapper.issues().size(), std::size_t(2));
        QCOMPARE(otherMapper.issues().at(0).kind,
                 H264EbspRbspIssueKind::Prohibited000001);
        compareSpan(otherMapper.issues().at(0).sourceSpan, 0, 24);
        QCOMPARE(otherMapper.issues().at(1).kind,
                 H264EbspRbspIssueKind::Prohibited000002);
        compareSpan(otherMapper.issues().at(1).sourceSpan, 32, 24);
    }

    void reportsAZeroFinalByte() {
        ScriptedSource source(bytes({0xAB, 0x00}));
        H264EbspRbspMapper mapper(source, LogicalViewId(1), span(0, 16));

        QCOMPARE(mapper.mapBatch().status, H264EbspRbspMapStatus::Complete);
        QCOMPARE(mapper.issues().size(), std::size_t(1));
        QCOMPARE(mapper.issues().front().kind, H264EbspRbspIssueKind::FinalZeroByte);
        compareSpan(mapper.issues().front().sourceSpan, 8, 8);
    }

    void preservesStateAcrossWorkBudgetBoundaries() {
        ScriptedSource source(bytes({0x12, 0x00, 0x00, 0x03, 0x45}));
        H264EbspRbspMapper mapper(source, LogicalViewId(1), span(0, 40));

        QCOMPARE(mapper.mapBatch(2).status, H264EbspRbspMapStatus::InProgress);
        QCOMPARE(mapper.sourceCursor(), quint64(2));
        compareSpan(mapper.mapping().sourceSpans().front(), 0, 16);

        QCOMPARE(mapper.mapBatch(1).status, H264EbspRbspMapStatus::InProgress);
        QCOMPARE(mapper.sourceCursor(), quint64(3));
        compareSpan(mapper.mapping().sourceSpans().front(), 0, 24);

        QCOMPARE(mapper.mapBatch(1).status, H264EbspRbspMapStatus::InProgress);
        QCOMPARE(mapper.sourceCursor(), quint64(4));
        compareSpan(mapper.mapping().sourceSpans().front(), 0, 24);
        QCOMPARE(mapper.excludedSpans().size(), std::size_t(1));

        QCOMPARE(mapper.mapBatch(1).status, H264EbspRbspMapStatus::Complete);
        QCOMPARE(mapper.sourceCursor(), quint64(5));
        QCOMPARE(mapper.mapping().sourceSpans().size(), std::size_t(2));
        compareSpan(mapper.mapping().sourceSpans().at(1), 32, 8);
        QCOMPARE(mapper.issues().size(), std::size_t(1));
        QCOMPARE(mapper.issues().front().kind,
                 H264EbspRbspIssueKind::Prohibited000003xx);
        compareSpan(mapper.issues().front().sourceSpan, 8, 32);
    }

    void mapsAnEscapeAcrossTheInternalReadWindow() {
        std::vector<std::byte> data(64U * 1024U + 2U, std::byte{0xFF});
        data.at(64U * 1024U - 2U) = std::byte{0x00};
        data.at(64U * 1024U - 1U) = std::byte{0x00};
        data.at(64U * 1024U) = std::byte{0x03};
        data.at(64U * 1024U + 1U) = std::byte{0x02};
        ScriptedSource source(std::move(data));
        H264EbspRbspMapper mapper(
            source, LogicalViewId(1), span(0, (64U * 1024U + 2U) * 8U));

        QCOMPARE(mapper.mapBatch().status, H264EbspRbspMapStatus::InProgress);
        QCOMPARE(mapper.sourceCursor(), quint64(64U * 1024U));
        QCOMPARE(mapper.mapBatch().status, H264EbspRbspMapStatus::Complete);
        QCOMPARE(mapper.mapping().sourceSpans().size(), std::size_t(2));
        compareSpan(mapper.mapping().sourceSpans().at(0), 0, 64U * 1024U * 8U);
        compareSpan(mapper.mapping().sourceSpans().at(1),
                    (64U * 1024U + 1U) * 8U,
                    8);
        QCOMPARE(mapper.excludedSpans().size(), std::size_t(1));
        compareSpan(mapper.excludedSpans().front().sourceSpan,
                    64U * 1024U * 8U,
                    8);
        QCOMPARE(mapper.excludedSpans().front().rbspBitOffset,
                 quint64(64U * 1024U * 8U));
        QVERIFY(mapper.issues().empty());
    }

    void rejectsInvalidCallsAndInputsWithoutReading() {
        ScriptedSource source(bytes({0x00, 0x00, 0x03}));
        H264EbspRbspMapper mapper(source, LogicalViewId(1), span(0, 24));

        const auto invalidBatch = mapper.mapBatch(0);
        QCOMPARE(invalidBatch.status, H264EbspRbspMapStatus::InvalidBatchSize);
        QCOMPARE(mapper.sourceCursor(), quint64(0));
        QCOMPARE(source.readCount(), std::size_t(0));
        QCOMPARE(mapper.mapBatch().status, H264EbspRbspMapStatus::Complete);

        ScriptedSource unalignedSource(bytes({0x00, 0x00, 0x03}));
        H264EbspRbspMapper unalignedStart(
            unalignedSource, LogicalViewId(2), span(1, 16));
        QCOMPARE(unalignedStart.mapBatch().status, H264EbspRbspMapStatus::InvalidInput);
        QCOMPARE(unalignedSource.readCount(), std::size_t(0));
        QCOMPARE(unalignedStart.mapBatch().status, H264EbspRbspMapStatus::InvalidInput);
        QCOMPARE(unalignedSource.readCount(), std::size_t(0));

        H264EbspRbspMapper unalignedLength(
            unalignedSource, LogicalViewId(3), span(0, 7));
        QCOMPARE(unalignedLength.mapBatch().status, H264EbspRbspMapStatus::InvalidInput);

        ScriptedSource shortSource(bytes({0x00}));
        H264EbspRbspMapper pastEnd(shortSource, LogicalViewId(4), span(0, 16));
        QCOMPARE(pastEnd.mapBatch().status, H264EbspRbspMapStatus::InvalidInput);
        QCOMPARE(shortSource.readCount(), std::size_t(0));

        H264EbspRbspMapLimits zeroLimits;
        zeroLimits.maximumIssues = 0;
        H264EbspRbspMapper invalidLimits(
            source, LogicalViewId(5), span(0, 24), zeroLimits);
        QCOMPARE(invalidLimits.mapBatch().status, H264EbspRbspMapStatus::InvalidInput);
        QCOMPARE(invalidLimits.mapBatch().status, H264EbspRbspMapStatus::InvalidInput);
    }

    void observesCancellationAndReplaysIt() {
        ScriptedSource preCancelledSource(bytes({0x00, 0x00, 0x03}));
        CancellationSource preCancellation;
        QVERIFY(preCancellation.requestCancellation());
        H264EbspRbspMapper preCancelled(preCancelledSource,
                                        LogicalViewId(1),
                                        span(0, 24),
                                        {},
                                        preCancellation.token());

        QCOMPARE(preCancelled.mapBatch().status, H264EbspRbspMapStatus::Cancelled);
        QCOMPARE(preCancelled.sourceCursor(), quint64(0));
        QCOMPARE(preCancelledSource.readCount(), std::size_t(0));
        QCOMPARE(preCancelled.mapBatch().status, H264EbspRbspMapStatus::Cancelled);

        CancellationSource cancellation;
        ScriptedSource source(std::vector<std::byte>(2048, std::byte{0xFF}),
                              ScriptedSource::Behavior::CancelOnRead,
                              &cancellation);
        H264EbspRbspMapper mapper(
            source, LogicalViewId(2), span(0, 2048U * 8U), {}, cancellation.token());

        QCOMPARE(mapper.mapBatch(2048).status, H264EbspRbspMapStatus::Cancelled);
        QCOMPARE(mapper.sourceCursor(), quint64(1024));
        QCOMPARE(mapper.rbspLogicalBitLength(), quint64(8192));
        QCOMPARE(mapper.mapping().sourceSpans().size(), std::size_t(1));
        compareSpan(mapper.mapping().sourceSpans().front(), 0, 8192);
        QCOMPARE(source.readCount(), std::size_t(1));
        QCOMPARE(mapper.mapBatch().status, H264EbspRbspMapStatus::Cancelled);
        QCOMPARE(source.readCount(), std::size_t(1));
    }

    void retainsTheCommittedPrefixOnSourceFailures() {
        std::vector<std::byte> data(64U * 1024U + 1U, std::byte{0xFF});
        ScriptedSource source(std::move(data), ScriptedSource::Behavior::FailSecondWindow);
        H264EbspRbspMapper mapper(
            source, LogicalViewId(1), span(0, (64U * 1024U + 1U) * 8U));

        QCOMPARE(mapper.mapBatch().status, H264EbspRbspMapStatus::InProgress);
        QCOMPARE(mapper.sourceCursor(), quint64(64U * 1024U));
        const auto failed = mapper.mapBatch();
        QCOMPARE(failed.status, H264EbspRbspMapStatus::SourceError);
        QCOMPARE(failed.errorMessage, QStringLiteral("injected read failure"));
        QCOMPARE(mapper.sourceCursor(), quint64(64U * 1024U));
        QCOMPARE(mapper.mapping().sourceSpans().size(), std::size_t(1));
        compareSpan(mapper.mapping().sourceSpans().front(), 0, 64U * 1024U * 8U);
        QCOMPARE(mapper.mapBatch().status, H264EbspRbspMapStatus::SourceError);
        QCOMPARE(source.readCount(), std::size_t(2));

        ScriptedSource incomplete(bytes({0x11}),
                                  ScriptedSource::Behavior::IncompleteSuccess);
        H264EbspRbspMapper incompleteMapper(incomplete, LogicalViewId(2), span(0, 8));
        const auto incompleteResult = incompleteMapper.mapBatch();
        QCOMPARE(incompleteResult.status, H264EbspRbspMapStatus::SourceError);
        QVERIFY(incompleteResult.errorMessage.contains(QStringLiteral("incomplete")));
        QCOMPARE(incompleteMapper.sourceCursor(), quint64(0));

        ScriptedSource ended(bytes({0x11}), ScriptedSource::Behavior::PrematureEnd);
        H264EbspRbspMapper endedMapper(ended, LogicalViewId(3), span(0, 8));
        QCOMPARE(endedMapper.mapBatch().status, H264EbspRbspMapStatus::SourceError);
        QCOMPARE(endedMapper.sourceCursor(), quint64(0));
    }

    void stopsBeforeExceedingOutputLimits() {
        H264EbspRbspMapLimits exactExcludedLimits;
        exactExcludedLimits.maximumExcludedSpans = 1;
        ScriptedSource exactExcludedSource(bytes({0x00, 0x00, 0x03}));
        H264EbspRbspMapper exactExcludedMapper(
            exactExcludedSource, LogicalViewId(10), span(0, 24), exactExcludedLimits);
        QCOMPARE(exactExcludedMapper.mapBatch().status,
                 H264EbspRbspMapStatus::Complete);
        QCOMPARE(exactExcludedMapper.excludedSpans().size(), std::size_t(1));

        H264EbspRbspMapLimits exactIssueLimits;
        exactIssueLimits.maximumIssues = 1;
        ScriptedSource exactIssueSource(bytes({0x00, 0x00, 0x01}));
        H264EbspRbspMapper exactIssueMapper(
            exactIssueSource, LogicalViewId(11), span(0, 24), exactIssueLimits);
        QCOMPARE(exactIssueMapper.mapBatch().status, H264EbspRbspMapStatus::Complete);
        QCOMPARE(exactIssueMapper.issues().size(), std::size_t(1));

        H264EbspRbspMapLimits segmentLimits;
        segmentLimits.maximumMappingSegments = 1;
        ScriptedSource segmentSource(bytes({0x11, 0x00, 0x00, 0x03, 0x22}));
        H264EbspRbspMapper segmentMapper(
            segmentSource, LogicalViewId(1), span(0, 40), segmentLimits);

        QCOMPARE(segmentMapper.mapBatch().status, H264EbspRbspMapStatus::ResourceLimit);
        QCOMPARE(segmentMapper.sourceCursor(), quint64(4));
        QCOMPARE(segmentMapper.rbspLogicalBitLength(), quint64(24));
        QCOMPARE(segmentMapper.mapping().sourceSpans().size(), std::size_t(1));
        compareSpan(segmentMapper.mapping().sourceSpans().front(), 0, 24);
        QCOMPARE(segmentMapper.excludedSpans().size(), std::size_t(1));

        H264EbspRbspMapLimits excludedLimits;
        excludedLimits.maximumExcludedSpans = 1;
        ScriptedSource excludedSource(
            bytes({0x00, 0x00, 0x03, 0x00, 0x00, 0x03}));
        H264EbspRbspMapper excludedMapper(
            excludedSource, LogicalViewId(2), span(0, 48), excludedLimits);

        QCOMPARE(excludedMapper.mapBatch().status, H264EbspRbspMapStatus::ResourceLimit);
        QCOMPARE(excludedMapper.sourceCursor(), quint64(5));
        QCOMPARE(excludedMapper.rbspLogicalBitLength(), quint64(32));
        QCOMPARE(excludedMapper.mapping().sourceSpans().size(), std::size_t(2));
        compareSpan(excludedMapper.mapping().sourceSpans().at(0), 0, 16);
        compareSpan(excludedMapper.mapping().sourceSpans().at(1), 24, 16);
        QCOMPARE(excludedMapper.excludedSpans().size(), std::size_t(1));

        H264EbspRbspMapLimits issueLimits;
        issueLimits.maximumIssues = 1;
        ScriptedSource issueSource(bytes({0x00, 0x00, 0x00, 0x00}));
        H264EbspRbspMapper issueMapper(
            issueSource, LogicalViewId(3), span(0, 32), issueLimits);

        QCOMPARE(issueMapper.mapBatch().status, H264EbspRbspMapStatus::ResourceLimit);
        QCOMPARE(issueMapper.sourceCursor(), quint64(3));
        QCOMPARE(issueMapper.rbspLogicalBitLength(), quint64(24));
        QCOMPARE(issueMapper.issues().size(), std::size_t(1));
        compareSpan(issueMapper.mapping().sourceSpans().front(), 0, 24);

        const std::size_t reads = issueSource.readCount();
        QCOMPARE(issueMapper.mapBatch().status, H264EbspRbspMapStatus::ResourceLimit);
        QCOMPARE(issueSource.readCount(), reads);
    }
};

QTEST_GUILESS_MAIN(H264EbspRbspMapperTest)

#include "h264_ebsp_rbsp_mapper_test.moc"
