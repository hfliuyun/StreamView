#include <streamview/core/cancellation.h>
#include <streamview/core/coordinates.h>
#include <streamview/core/source.h>
#include <streamview/rules/h264_rbsp_payload_transform_provider.h>
#include <streamview/rules/payload_transform.h>

#include <QObject>
#include <QTest>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

namespace streamview::rules {

namespace {

using namespace streamview::core;

[[nodiscard]] std::vector<std::byte> toBytes(std::initializer_list<quint8> values) {
    std::vector<std::byte> result;
    result.reserve(values.size());
    for (const quint8 value : values) {
        result.push_back(static_cast<std::byte>(value));
    }
    return result;
}

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
        const auto count = std::min(destination.size(), data_.size() - offset);
        std::copy_n(data_.data() + offset, count, destination.data());
        return {count == destination.size() ? SourceReadStatus::Complete
                                            : SourceReadStatus::EndOfSource,
                count,
                {}};
    }

private:
    std::vector<std::byte> data_;
};

class FaultySource final : public RandomAccessSource {
public:
    FaultySource(std::vector<std::byte> data, quint64 failAtOffset)
        : data_(std::move(data)), failAtOffset_(failAtOffset) {}

    [[nodiscard]] quint64 sizeBytes() const noexcept override { return data_.size(); }
    [[nodiscard]] QString identity() const override { return QStringLiteral("faulty-source"); }

    [[nodiscard]] SourceReadResult
    readAt(quint64 byteOffset, std::span<std::byte> destination) const override {
        if (destination.empty()) {
            return {SourceReadStatus::Complete, 0, {}};
        }
        if (byteOffset >= sizeBytes()) {
            return {SourceReadStatus::EndOfSource, 0, {}};
        }
        const quint64 available = sizeBytes() - byteOffset;
        const std::size_t count = static_cast<std::size_t>(
            std::min(static_cast<quint64>(destination.size()), available));
        if (byteOffset + static_cast<quint64>(count) > failAtOffset_) {
            return {SourceReadStatus::Error, 0, QStringLiteral("Injected I/O failure")};
        }
        for (std::size_t i = 0; i < count; ++i) {
            destination[i] = data_[static_cast<std::size_t>(byteOffset + i)];
        }
        return {count == destination.size() ? SourceReadStatus::Complete
                                            : SourceReadStatus::EndOfSource,
                count,
                {}};
    }

private:
    std::vector<std::byte> data_;
    quint64 failAtOffset_ = 0;
};

class CancellingSource final : public RandomAccessSource {
public:
    CancellingSource(std::vector<std::byte> data,
                     quint64 cancelAtOffset,
                     CancellationSource& cancellation)
        : data_(std::move(data)), cancelAtOffset_(cancelAtOffset),
          cancellation_(&cancellation) {}

    [[nodiscard]] quint64 sizeBytes() const noexcept override { return data_.size(); }
    [[nodiscard]] QString identity() const override { return QStringLiteral("cancelling-source"); }

    [[nodiscard]] SourceReadResult
    readAt(quint64 byteOffset, std::span<std::byte> destination) const override {
        if (destination.empty()) {
            return {SourceReadStatus::Complete, 0, {}};
        }
        if (byteOffset >= sizeBytes()) {
            return {SourceReadStatus::EndOfSource, 0, {}};
        }
        const quint64 available = sizeBytes() - byteOffset;
        const std::size_t count = static_cast<std::size_t>(
            std::min(static_cast<quint64>(destination.size()), available));
        std::copy_n(data_.data() + static_cast<std::size_t>(byteOffset),
                    count,
                    destination.data());
        const quint64 readEnd = byteOffset + static_cast<quint64>(count);
        if (!cancelled_ && cancelAtOffset_ >= byteOffset && cancelAtOffset_ < readEnd) {
            cancelled_ = true;
            (void)cancellation_->requestCancellation();
        }
        return {count == destination.size() ? SourceReadStatus::Complete
                                            : SourceReadStatus::EndOfSource,
                count,
                {}};
    }

private:
    std::vector<std::byte> data_;
    quint64 cancelAtOffset_ = 0;
    CancellationSource* cancellation_ = nullptr;
    mutable bool cancelled_ = false;
};

[[nodiscard]] SourceMapping makeSingleSpanMapping(quint64 byteOffset, quint64 byteLength) {
    const auto start = SourceBitAddress::fromByteAndBit(byteOffset, 0);
    const auto span = SourceSpan::create(*start, byteLength * 8U);
    auto mapping = SourceMapping::create(LogicalViewId(1), {*span});
    return *mapping;
}

[[nodiscard]] SourceMapping makeDisjointMapping(
    const std::vector<std::pair<quint64, quint64>>& byteRanges) {
    std::vector<SourceSpan> spans;
    spans.reserve(byteRanges.size());
    for (const auto& [offset, length] : byteRanges) {
        const auto start = SourceBitAddress::fromByteAndBit(offset, 0);
        const auto span = SourceSpan::create(*start, length * 8U);
        spans.push_back(*span);
    }
    auto mapping = SourceMapping::create(LogicalViewId(1), std::move(spans));
    return *mapping;
}

/// A custom mock provider that excludes 0xEE escape bytes and forwards all other bytes
class MockEscapeFilterProvider final : public PayloadTransformProvider {
public:
    [[nodiscard]] QString identifier() const override {
        return QStringLiteral("mock_escape_filter");
    }

    [[nodiscard]] PayloadTransformResult transform(
        const PayloadTransformRequest& request) const override {
        PayloadTransformResult result;
        if (!request.source || !request.inputMapping) {
            result.status = DslExecutionStatus::InvalidDefinition;
            result.errorMessage = QStringLiteral("Null inputs");
            return result;
        }

        if (request.cancellation && request.cancellation->isCancellationRequested()) {
            result.status = DslExecutionStatus::Cancelled;
            result.errorMessage = QStringLiteral("Cancelled before transform");
            return result;
        }

        const quint64 totalBytes = request.logicalBitLength / 8U;
        if (request.maximumInspectedBytes > 0 && totalBytes > request.maximumInspectedBytes) {
            result.status = DslExecutionStatus::ResourceLimit;
            result.inspectedByteCount = request.maximumInspectedBytes;
            result.errorMessage = QStringLiteral("Inspection budget exceeded");
            return result;
        }

        std::vector<SourceSpan> forwardedSpans;
        std::vector<PayloadExcludedSpan> excludedSpans;
        quint64 outputBitOffset = 0;
        quint64 inspectedBytes = 0;

        for (quint64 byteIndex = 0; byteIndex < totalBytes; ++byteIndex) {
            if (request.cancellation && request.cancellation->isCancellationRequested()) {
                result.status = DslExecutionStatus::Cancelled;
                result.errorMessage = QStringLiteral("Cancelled during transform");
                result.inspectedByteCount = inspectedBytes;
                return result;
            }

            const quint64 logicalBitOffset = request.logicalBitStart + (byteIndex * 8U);
            const auto logicalAddr = LogicalBitAddress(
                request.inputMapping->viewId(), logicalBitOffset);
            const auto rangeOpt = LogicalRange::create(logicalAddr, 8U);
            if (!rangeOpt) {
                result.status = DslExecutionStatus::InvalidDefinition;
                result.errorMessage = QStringLiteral("Invalid logical range coordinates");
                return result;
            }
            const auto location = request.inputMapping->locate(*rangeOpt);
            if (!location || location->sourceSpans().empty()) {
                result.status = DslExecutionStatus::InvalidDefinition;
                result.errorMessage = QStringLiteral("Failed to locate byte in input mapping");
                return result;
            }

            const SourceSpan byteSpan = location->sourceSpans().front();
            const quint64 physByteOffset = byteSpan.start().byteOffset();

            std::byte readByte{};
            const auto readRes = request.source->readAt(physByteOffset, std::span<std::byte>(&readByte, 1));
            if (readRes.status == SourceReadStatus::Error) {
                result.status = DslExecutionStatus::SourceError;
                result.errorMessage = readRes.errorMessage;
                result.inspectedByteCount = inspectedBytes;
                return result;
            }
            if (readRes.status == SourceReadStatus::EndOfSource || readRes.bytesRead == 0) {
                result.status = DslExecutionStatus::TruncatedSource;
                result.errorMessage = QStringLiteral("Unexpected EOF");
                result.inspectedByteCount = inspectedBytes;
                return result;
            }

            const auto val = static_cast<quint8>(readByte);
            ++inspectedBytes;

            if (val == 0xEE) {
                // Exclude this byte
                PayloadExcludedSpan excluded{byteSpan, outputBitOffset};
                excludedSpans.push_back(excluded);
            } else {
                // Forward this byte
                forwardedSpans.push_back(byteSpan);
                outputBitOffset += 8U;
            }
        }

        auto mappingOpt = SourceMapping::create(
            request.inputMapping->viewId(), std::move(forwardedSpans));
        if (!mappingOpt) {
            result.status = DslExecutionStatus::InvalidDefinition;
            result.errorMessage = QStringLiteral("Failed to build forwarded mapping");
            return result;
        }

        result.status = DslExecutionStatus::Materialized;
        result.forwardedMapping = std::move(mappingOpt);
        result.excludedSpans = std::move(excludedSpans);
        result.inspectedByteCount = inspectedBytes;
        return result;
    }
};

} // namespace

class PayloadTransformTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    // 1. Identity Provider Tests
    void identityTransformSingleSpan();
    void identityTransformDisjointSpans();
    void identityTransformSubrangeAcrossSpanBoundaries();
    void identityTransformRejectsUnalignedStart();
    void identityTransformRejectsUnalignedLength();
    void identityTransformRejectsZeroLength();
    void identityTransformRejectsNullInputs();
    void identityTransformRejectsOutOfBounds();
    void identityTransformPreCancellation();
    void identityTransformInspectionBudgetExceeded();
    void identityTransformInspectionBudgetExactBoundary();

    // 2. Registry Tests
    void registryRegisterNewProvider();
    void registryRejectDuplicateRegistration();
    void registryRejectNullOrEmptyProvider();
    void registryFindKnownAndUnknownProvider();
    void registryUnregisterCustomProvider();
    void registryResetRestoresBuiltinsOnly();

    // 3. Custom Provider Tests
    void customProviderWithExcludedRecordsAndLengthInvariant();
    void customProviderMidTransformCancellation();
    void customProviderSourceErrorPropagation();
    void customProviderEOFPropagation();

    // 4. Concrete H.264 RBSP Provider Tests
    void rbspTransformNormalSingleSpan00000301();
    void rbspTransformMultipleExcludedSpansAndLengthInvariant();
    void rbspTransformWithNonZeroLogicalStart();
    void rbspTransformDisjointMappingRejected();
    void rbspTransformDisjointMappingSubrangeWithinSingleSpanAccepted();
    void rbspTransformMalformedSequencesEmitDiagnostics();
    void rbspTransformSourceErrorPropagation();
    void rbspTransformEndOfSourcePropagation();
    void rbspTransformMidInspectionCancellation();
    void rbspTransformInspectionBudgetExceededAndExactBoundary();
    void rbspTransformUnalignedAndOutOfBoundsRejection();
    void registryRegisterAndLifecycleRbspProvider();
};

void PayloadTransformTest::initTestCase() {
    PayloadTransformRegistry::instance().reset();
}

void PayloadTransformTest::cleanupTestCase() {
    PayloadTransformRegistry::instance().reset();
}

void PayloadTransformTest::identityTransformSingleSpan() {
    const auto data = toBytes({0x01, 0x02, 0x03, 0x04});
    const MemorySource source(data);
    const auto mapping = makeSingleSpanMapping(0, 4);

    IdentityPayloadTransformProvider provider;
    QCOMPARE(provider.identifier(), QStringLiteral("none"));

    PayloadTransformRequest request;
    request.source = &source;
    request.inputMapping = &mapping;
    request.logicalBitStart = 0;
    request.logicalBitLength = 32;

    const auto result = provider.transform(request);
    QVERIFY(result.succeeded());
    QCOMPARE(result.status, DslExecutionStatus::Materialized);
    QVERIFY(result.forwardedMapping.has_value());
    QCOMPARE(result.forwardedMapping->logicalBitLength(), 32ULL);
    QCOMPARE(result.forwardedMapping->sourceSpans().size(), 1ULL);
    QCOMPARE(result.forwardedMapping->sourceSpans().front().start().absoluteBitOffset(), 0ULL);
    QCOMPARE(result.forwardedMapping->sourceSpans().front().bitLength(), 32ULL);
    QCOMPARE(result.inspectedByteCount, 4ULL);
    QVERIFY(result.excludedSpans.empty());
}

void PayloadTransformTest::identityTransformDisjointSpans() {
    // 2 disjoint spans: [0..16) and [32..48) in physical space -> 32 logical bits
    const auto data = toBytes({0x01, 0x02, 0xFF, 0xFF, 0x03, 0x04});
    const MemorySource source(data);
    const auto mapping = makeDisjointMapping({{0, 2}, {4, 2}});

    IdentityPayloadTransformProvider provider;
    PayloadTransformRequest request;
    request.source = &source;
    request.inputMapping = &mapping;
    request.logicalBitStart = 0;
    request.logicalBitLength = 32;

    const auto result = provider.transform(request);
    QVERIFY(result.succeeded());
    QCOMPARE(result.forwardedMapping->logicalBitLength(), 32ULL);
    QCOMPARE(result.forwardedMapping->sourceSpans().size(), 2ULL);
    QCOMPARE(result.forwardedMapping->sourceSpans()[0].start().byteOffset(), 0ULL);
    QCOMPARE(result.forwardedMapping->sourceSpans()[0].bitLength(), 16ULL);
    QCOMPARE(result.forwardedMapping->sourceSpans()[1].start().byteOffset(), 4ULL);
    QCOMPARE(result.forwardedMapping->sourceSpans()[1].bitLength(), 16ULL);
    QCOMPARE(result.inspectedByteCount, 4ULL);
}

void PayloadTransformTest::identityTransformSubrangeAcrossSpanBoundaries() {
    // 3 disjoint spans: [0..2), [4..6), [8..10) -> total 6 bytes = 48 bits
    // Slice logical [8..40) = 4 bytes (middle slice spanning part of span 0, all of span 1, part of span 2)
    const auto data = toBytes({0x00, 0x01, 0xFF, 0xFF, 0x02, 0x03, 0xEE, 0xEE, 0x04, 0x05});
    const MemorySource source(data);
    const auto mapping = makeDisjointMapping({{0, 2}, {4, 2}, {8, 2}});

    IdentityPayloadTransformProvider provider;
    PayloadTransformRequest request;
    request.source = &source;
    request.inputMapping = &mapping;
    request.logicalBitStart = 8; // skip 1st byte of span 0
    request.logicalBitLength = 32; // span 0 (8 bits) + span 1 (16 bits) + span 2 (8 bits)

    const auto result = provider.transform(request);
    QVERIFY(result.succeeded());
    QCOMPARE(result.forwardedMapping->logicalBitLength(), 32ULL);
    QCOMPARE(result.forwardedMapping->sourceSpans().size(), 3ULL);
    // Span 0 slice: byte 1 (offset 1, len 8)
    QCOMPARE(result.forwardedMapping->sourceSpans()[0].start().byteOffset(), 1ULL);
    QCOMPARE(result.forwardedMapping->sourceSpans()[0].bitLength(), 8ULL);
    // Span 1 slice: byte 4..5 (offset 4, len 16)
    QCOMPARE(result.forwardedMapping->sourceSpans()[1].start().byteOffset(), 4ULL);
    QCOMPARE(result.forwardedMapping->sourceSpans()[1].bitLength(), 16ULL);
    // Span 2 slice: byte 8 (offset 8, len 8)
    QCOMPARE(result.forwardedMapping->sourceSpans()[2].start().byteOffset(), 8ULL);
    QCOMPARE(result.forwardedMapping->sourceSpans()[2].bitLength(), 8ULL);
    QCOMPARE(result.inspectedByteCount, 4ULL);
}

void PayloadTransformTest::identityTransformRejectsUnalignedStart() {
    const auto data = toBytes({0x01, 0x02});
    const MemorySource source(data);
    const auto mapping = makeSingleSpanMapping(0, 2);

    IdentityPayloadTransformProvider provider;
    PayloadTransformRequest request;
    request.source = &source;
    request.inputMapping = &mapping;
    request.logicalBitStart = 3; // unaligned
    request.logicalBitLength = 8;

    const auto result = provider.transform(request);
    QCOMPARE(result.status, DslExecutionStatus::InvalidDefinition);
    QVERIFY(!result.succeeded());
}

void PayloadTransformTest::identityTransformRejectsUnalignedLength() {
    const auto data = toBytes({0x01, 0x02});
    const MemorySource source(data);
    const auto mapping = makeSingleSpanMapping(0, 2);

    IdentityPayloadTransformProvider provider;
    PayloadTransformRequest request;
    request.source = &source;
    request.inputMapping = &mapping;
    request.logicalBitStart = 0;
    request.logicalBitLength = 7; // unaligned

    const auto result = provider.transform(request);
    QCOMPARE(result.status, DslExecutionStatus::InvalidDefinition);
}

void PayloadTransformTest::identityTransformRejectsZeroLength() {
    const auto data = toBytes({0x01, 0x02});
    const MemorySource source(data);
    const auto mapping = makeSingleSpanMapping(0, 2);

    IdentityPayloadTransformProvider provider;
    PayloadTransformRequest request;
    request.source = &source;
    request.inputMapping = &mapping;
    request.logicalBitStart = 0;
    request.logicalBitLength = 0; // zero length

    const auto result = provider.transform(request);
    QCOMPARE(result.status, DslExecutionStatus::InvalidDefinition);
}

void PayloadTransformTest::identityTransformRejectsNullInputs() {
    IdentityPayloadTransformProvider provider;

    PayloadTransformRequest req1;
    req1.source = nullptr;
    const auto res1 = provider.transform(req1);
    QCOMPARE(res1.status, DslExecutionStatus::InvalidDefinition);

    const auto data = toBytes({0x01});
    const MemorySource source(data);
    PayloadTransformRequest req2;
    req2.source = &source;
    req2.inputMapping = nullptr;
    const auto res2 = provider.transform(req2);
    QCOMPARE(res2.status, DslExecutionStatus::InvalidDefinition);
}

void PayloadTransformTest::identityTransformRejectsOutOfBounds() {
    const auto data = toBytes({0x01, 0x02});
    const MemorySource source(data);
    const auto mapping = makeSingleSpanMapping(0, 2);

    IdentityPayloadTransformProvider provider;

    // Start > length
    PayloadTransformRequest req1;
    req1.source = &source;
    req1.inputMapping = &mapping;
    req1.logicalBitStart = 24;
    req1.logicalBitLength = 8;
    QCOMPARE(provider.transform(req1).status, DslExecutionStatus::InvalidDefinition);

    // Start + length > length
    PayloadTransformRequest req2;
    req2.source = &source;
    req2.inputMapping = &mapping;
    req2.logicalBitStart = 8;
    req2.logicalBitLength = 16;
    QCOMPARE(provider.transform(req2).status, DslExecutionStatus::InvalidDefinition);
}

void PayloadTransformTest::identityTransformPreCancellation() {
    const auto data = toBytes({0x01, 0x02});
    const MemorySource source(data);
    const auto mapping = makeSingleSpanMapping(0, 2);

    CancellationSource cancelSource;
    (void)cancelSource.requestCancellation();

    IdentityPayloadTransformProvider provider;
    PayloadTransformRequest request;
    request.source = &source;
    request.inputMapping = &mapping;
    request.logicalBitStart = 0;
    request.logicalBitLength = 16;
    request.cancellation = cancelSource.token();

    const auto result = provider.transform(request);
    QCOMPARE(result.status, DslExecutionStatus::Cancelled);
    QVERIFY(!result.succeeded());
}

void PayloadTransformTest::identityTransformInspectionBudgetExceeded() {
    const auto data = toBytes({0x01, 0x02, 0x03, 0x04});
    const MemorySource source(data);
    const auto mapping = makeSingleSpanMapping(0, 4);

    IdentityPayloadTransformProvider provider;
    PayloadTransformRequest request;
    request.source = &source;
    request.inputMapping = &mapping;
    request.logicalBitStart = 0;
    request.logicalBitLength = 32; // 4 bytes
    request.maximumInspectedBytes = 3; // Limit 3 bytes < 4

    const auto result = provider.transform(request);
    QCOMPARE(result.status, DslExecutionStatus::ResourceLimit);
    QCOMPARE(result.inspectedByteCount, 3ULL);
    QVERIFY(!result.succeeded());
}

void PayloadTransformTest::identityTransformInspectionBudgetExactBoundary() {
    const auto data = toBytes({0x01, 0x02, 0x03, 0x04});
    const MemorySource source(data);
    const auto mapping = makeSingleSpanMapping(0, 4);

    IdentityPayloadTransformProvider provider;
    PayloadTransformRequest request;
    request.source = &source;
    request.inputMapping = &mapping;
    request.logicalBitStart = 0;
    request.logicalBitLength = 32; // 4 bytes
    request.maximumInspectedBytes = 4; // Exactly 4 bytes

    const auto result = provider.transform(request);
    QCOMPARE(result.status, DslExecutionStatus::Materialized);
    QCOMPARE(result.inspectedByteCount, 4ULL);
    QVERIFY(result.succeeded());
}

void PayloadTransformTest::registryRegisterNewProvider() {
    PayloadTransformRegistry registry;
    auto mockProvider = std::make_shared<MockEscapeFilterProvider>();

    QVERIFY(registry.registerProvider(mockProvider));
    const auto found = registry.findProvider(QStringLiteral("mock_escape_filter"));
    QVERIFY(found != nullptr);
    QCOMPARE(found->identifier(), QStringLiteral("mock_escape_filter"));
}

void PayloadTransformTest::registryRejectDuplicateRegistration() {
    PayloadTransformRegistry registry;
    auto mock1 = std::make_shared<MockEscapeFilterProvider>();
    auto mock2 = std::make_shared<MockEscapeFilterProvider>();

    QVERIFY(registry.registerProvider(mock1));
    QVERIFY(!registry.registerProvider(mock2)); // duplicate rejected
}

void PayloadTransformTest::registryRejectNullOrEmptyProvider() {
    PayloadTransformRegistry registry;
    QVERIFY(!registry.registerProvider(nullptr));
}

void PayloadTransformTest::registryFindKnownAndUnknownProvider() {
    PayloadTransformRegistry registry;
    const auto none = registry.findProvider(QStringLiteral("none"));
    QVERIFY(none != nullptr);
    QCOMPARE(none->identifier(), QStringLiteral("none"));

    const auto unknown = registry.findProvider(QStringLiteral("non_existent_provider"));
    QCOMPARE(unknown, nullptr);
}

void PayloadTransformTest::registryUnregisterCustomProvider() {
    PayloadTransformRegistry registry;
    auto mock = std::make_shared<MockEscapeFilterProvider>();
    QVERIFY(registry.registerProvider(mock));

    QVERIFY(registry.unregisterProvider(QStringLiteral("mock_escape_filter")));
    QCOMPARE(registry.findProvider(QStringLiteral("mock_escape_filter")), nullptr);

    // Built-in "none" cannot be unregistered
    QVERIFY(!registry.unregisterProvider(QStringLiteral("none")));
    QVERIFY(registry.findProvider(QStringLiteral("none")) != nullptr);
}

void PayloadTransformTest::registryResetRestoresBuiltinsOnly() {
    PayloadTransformRegistry registry;
    auto mock = std::make_shared<MockEscapeFilterProvider>();
    QVERIFY(registry.registerProvider(mock));

    registry.reset();
    QCOMPARE(registry.findProvider(QStringLiteral("mock_escape_filter")), nullptr);
    QVERIFY(registry.findProvider(QStringLiteral("none")) != nullptr);
}

void PayloadTransformTest::customProviderWithExcludedRecordsAndLengthInvariant() {
    // 5 bytes: 0x11, 0xEE (escape), 0x22, 0xEE (escape), 0x33
    // Forwarded should have 3 bytes (0x11, 0x22, 0x33) = 24 bits
    // Excluded should have 2 records (each 8 bits)
    // Invariant: total input bits (40) == forwarded bits (24) + excluded bits (16)
    const auto data = toBytes({0x11, 0xEE, 0x22, 0xEE, 0x33});
    const MemorySource source(data);
    const auto mapping = makeSingleSpanMapping(0, 5);

    MockEscapeFilterProvider provider;
    PayloadTransformRequest request;
    request.source = &source;
    request.inputMapping = &mapping;
    request.logicalBitStart = 0;
    request.logicalBitLength = 40;

    const auto result = provider.transform(request);
    QVERIFY(result.succeeded());
    QCOMPARE(result.status, DslExecutionStatus::Materialized);
    QVERIFY(result.forwardedMapping.has_value());
    QCOMPARE(result.forwardedMapping->logicalBitLength(), 24ULL);
    QCOMPARE(result.inspectedByteCount, 5ULL);

    // Check excluded records
    QCOMPARE(result.excludedSpans.size(), 2ULL);
    QCOMPARE(result.excludedSpans[0].sourceSpan.start().byteOffset(), 1ULL);
    QCOMPARE(result.excludedSpans[0].sourceSpan.bitLength(), 8ULL);
    QCOMPARE(result.excludedSpans[0].outputBitOffset, 8ULL);

    QCOMPARE(result.excludedSpans[1].sourceSpan.start().byteOffset(), 3ULL);
    QCOMPARE(result.excludedSpans[1].sourceSpan.bitLength(), 8ULL);
    QCOMPARE(result.excludedSpans[1].outputBitOffset, 16ULL);

    // Invariant check
    quint64 totalExcludedBits = 0;
    for (const auto& excluded : result.excludedSpans) {
        totalExcludedBits += excluded.sourceSpan.bitLength();
    }
    QCOMPARE(result.forwardedMapping->logicalBitLength() + totalExcludedBits, 40ULL);
}

void PayloadTransformTest::customProviderMidTransformCancellation() {
    const auto data = toBytes({0x11, 0x22, 0x33, 0x44});
    CancellationSource cancelSource;
    const CancellingSource source(data, 1, cancelSource);
    const auto mapping = makeSingleSpanMapping(0, 4);

    MockEscapeFilterProvider provider;
    PayloadTransformRequest request;
    request.source = &source;
    request.inputMapping = &mapping;
    request.logicalBitStart = 0;
    request.logicalBitLength = 32;
    request.cancellation = cancelSource.token();

    const auto result = provider.transform(request);
    QCOMPARE(result.status, DslExecutionStatus::Cancelled);
    QCOMPARE(result.inspectedByteCount, 2ULL);
    QVERIFY(!result.succeeded());
}

void PayloadTransformTest::customProviderSourceErrorPropagation() {
    const auto data = toBytes({0x11, 0x22, 0x33, 0x44});
    // Injected failure at byte 2
    const FaultySource source(data, 2);
    const auto mapping = makeSingleSpanMapping(0, 4);

    MockEscapeFilterProvider provider;
    PayloadTransformRequest request;
    request.source = &source;
    request.inputMapping = &mapping;
    request.logicalBitStart = 0;
    request.logicalBitLength = 32;

    const auto result = provider.transform(request);
    QCOMPARE(result.status, DslExecutionStatus::SourceError);
    QVERIFY(!result.succeeded());
}

void PayloadTransformTest::customProviderEOFPropagation() {
    // 2 bytes provided in data, but mapping requests 4 bytes
    const auto data = toBytes({0x11, 0x22});
    const MemorySource source(data);
    const auto mapping = makeSingleSpanMapping(0, 4);

    MockEscapeFilterProvider provider;
    PayloadTransformRequest request;
    request.source = &source;
    request.inputMapping = &mapping;
    request.logicalBitStart = 0;
    request.logicalBitLength = 32;

    const auto result = provider.transform(request);
    QCOMPARE(result.status, DslExecutionStatus::TruncatedSource);
    QVERIFY(!result.succeeded());
}

void PayloadTransformTest::rbspTransformNormalSingleSpan00000301() {
    const auto data = toBytes({0x00, 0x00, 0x03, 0x01});
    const MemorySource source(data);
    const auto mapping = makeSingleSpanMapping(0, 4);

    H264RbspPayloadTransformProvider provider;
    QCOMPARE(provider.identifier(), QStringLiteral("rbsp"));

    PayloadTransformRequest request;
    request.source = &source;
    request.inputMapping = &mapping;
    request.logicalBitStart = 0;
    request.logicalBitLength = 32;

    const auto result = provider.transform(request);
    QVERIFY(result.succeeded());
    QCOMPARE(result.status, DslExecutionStatus::Materialized);
    QVERIFY(result.forwardedMapping.has_value());
    QCOMPARE(result.forwardedMapping->logicalBitLength(), 24ULL);
    QCOMPARE(result.inspectedByteCount, 4ULL);
    QCOMPARE(result.excludedSpans.size(), 1ULL);
    QCOMPARE(result.excludedSpans[0].sourceSpan.start().byteOffset(), 2ULL);
    QCOMPARE(result.excludedSpans[0].sourceSpan.bitLength(), 8ULL);
    QCOMPARE(result.excludedSpans[0].outputBitOffset, 16ULL);

    quint64 totalExcludedBits = 0;
    for (const auto& excluded : result.excludedSpans) {
        totalExcludedBits += excluded.sourceSpan.bitLength();
    }
    QCOMPARE(result.forwardedMapping->logicalBitLength() + totalExcludedBits, 32ULL);
    QVERIFY(result.diagnostics.empty());
}

void PayloadTransformTest::rbspTransformMultipleExcludedSpansAndLengthInvariant() {
    // 12 bytes with three 0x03 escape bytes: 00 00 03 01 00 00 03 02 00 00 03 03
    const auto data = toBytes({0x00, 0x00, 0x03, 0x01, 0x00, 0x00, 0x03, 0x02, 0x00, 0x00, 0x03, 0x03});
    const MemorySource source(data);
    const auto mapping = makeSingleSpanMapping(0, 12);

    H264RbspPayloadTransformProvider provider;
    PayloadTransformRequest request;
    request.source = &source;
    request.inputMapping = &mapping;
    request.logicalBitStart = 0;
    request.logicalBitLength = 96;

    const auto result = provider.transform(request);
    QVERIFY(result.succeeded());
    QCOMPARE(result.forwardedMapping->logicalBitLength(), 72ULL);
    QCOMPARE(result.inspectedByteCount, 12ULL);
    QCOMPARE(result.excludedSpans.size(), 3ULL);

    QCOMPARE(result.excludedSpans[0].sourceSpan.start().byteOffset(), 2ULL);
    QCOMPARE(result.excludedSpans[0].outputBitOffset, 16ULL);

    QCOMPARE(result.excludedSpans[1].sourceSpan.start().byteOffset(), 6ULL);
    QCOMPARE(result.excludedSpans[1].outputBitOffset, 40ULL);

    QCOMPARE(result.excludedSpans[2].sourceSpan.start().byteOffset(), 10ULL);
    QCOMPARE(result.excludedSpans[2].outputBitOffset, 64ULL);

    quint64 totalExcludedBits = 0;
    for (const auto& excluded : result.excludedSpans) {
        totalExcludedBits += excluded.sourceSpan.bitLength();
    }
    QCOMPARE(result.forwardedMapping->logicalBitLength() + totalExcludedBits, 96ULL);
}

void PayloadTransformTest::rbspTransformWithNonZeroLogicalStart() {
    // 12 bytes: 4 prefix bytes, then 00 00 03 01 EE FF, then 2 trailing bytes
    const auto data = toBytes({0xAA, 0xBB, 0xCC, 0xDD, 0x00, 0x00, 0x03, 0x01, 0xEE, 0xFF, 0x11, 0x22});
    const MemorySource source(data);
    const auto mapping = makeSingleSpanMapping(0, 12);

    H264RbspPayloadTransformProvider provider;
    PayloadTransformRequest request;
    request.source = &source;
    request.inputMapping = &mapping;
    request.logicalBitStart = 32; // byte 4
    request.logicalBitLength = 48; // 6 bytes (bytes 4..9)

    const auto result = provider.transform(request);
    QVERIFY(result.succeeded());
    QCOMPARE(result.forwardedMapping->logicalBitLength(), 40ULL); // 5 bytes
    QCOMPARE(result.inspectedByteCount, 6ULL);
    QCOMPARE(result.excludedSpans.size(), 1ULL);
    QCOMPARE(result.excludedSpans[0].sourceSpan.start().byteOffset(), 6ULL);
    QCOMPARE(result.excludedSpans[0].outputBitOffset, 16ULL);

    for (const auto& span : result.forwardedMapping->sourceSpans()) {
        QVERIFY(span.start().byteOffset() >= 4ULL);
        QVERIFY(span.endExclusive().byteOffset() <= 10ULL);
    }
}

void PayloadTransformTest::rbspTransformDisjointMappingRejected() {
    const auto data = toBytes({0x00, 0x00, 0xFF, 0xFF, 0x03, 0x01});
    const MemorySource source(data);
    const auto mapping = makeDisjointMapping({{0, 2}, {4, 2}});

    H264RbspPayloadTransformProvider provider;
    PayloadTransformRequest request;
    request.source = &source;
    request.inputMapping = &mapping;
    request.logicalBitStart = 0;
    request.logicalBitLength = 32;

    const auto result = provider.transform(request);
    QCOMPARE(result.status, DslExecutionStatus::InvalidDefinition);
    QVERIFY(!result.succeeded());
    QVERIFY(result.errorMessage.contains(QStringLiteral("Disjoint EBSP source spans")));
}

void PayloadTransformTest::rbspTransformDisjointMappingSubrangeWithinSingleSpanAccepted() {
    // 10 bytes: span 0 [0..4) has 00 00 03 01, gap [4..6), span 1 [6..10)
    const auto data = toBytes({0x00, 0x00, 0x03, 0x01, 0xFF, 0xFF, 0xAA, 0xBB, 0xCC, 0xDD});
    const MemorySource source(data);
    const auto mapping = makeDisjointMapping({{0, 4}, {6, 4}});

    H264RbspPayloadTransformProvider provider;
    PayloadTransformRequest request;
    request.source = &source;
    request.inputMapping = &mapping;
    request.logicalBitStart = 0;
    request.logicalBitLength = 32; // spans only span 0

    const auto result = provider.transform(request);
    QVERIFY(result.succeeded());
    QCOMPARE(result.forwardedMapping->logicalBitLength(), 24ULL);
    QCOMPARE(result.excludedSpans.size(), 1ULL);
    QCOMPARE(result.excludedSpans[0].sourceSpan.start().byteOffset(), 2ULL);
}

void PayloadTransformTest::rbspTransformMalformedSequencesEmitDiagnostics() {
    H264RbspPayloadTransformProvider provider;

    // 1. Prohibited 00 00 00
    {
        const auto data = toBytes({0x00, 0x00, 0x00, 0x05});
        const MemorySource source(data);
        const auto mapping = makeSingleSpanMapping(0, 4);
        PayloadTransformRequest request;
        request.source = &source;
        request.inputMapping = &mapping;
        request.logicalBitStart = 0;
        request.logicalBitLength = 32;

        const auto result = provider.transform(request);
        QVERIFY(result.succeeded());
        QCOMPARE(result.diagnostics.size(), 1ULL);
        QCOMPARE(result.diagnostics[0].code, DiagnosticCode::InvalidSyntax);
        QVERIFY(result.diagnostics[0].message.contains(QStringLiteral("00 00 00")));
    }

    // 2. Prohibited 00 00 01
    {
        const auto data = toBytes({0x00, 0x00, 0x01, 0x05});
        const MemorySource source(data);
        const auto mapping = makeSingleSpanMapping(0, 4);
        PayloadTransformRequest request;
        request.source = &source;
        request.inputMapping = &mapping;
        request.logicalBitStart = 0;
        request.logicalBitLength = 32;

        const auto result = provider.transform(request);
        QVERIFY(result.succeeded());
        QCOMPARE(result.diagnostics.size(), 1ULL);
        QVERIFY(result.diagnostics[0].message.contains(QStringLiteral("00 00 01")));
    }

    // 3. Prohibited 00 00 02
    {
        const auto data = toBytes({0x00, 0x00, 0x02, 0x05});
        const MemorySource source(data);
        const auto mapping = makeSingleSpanMapping(0, 4);
        PayloadTransformRequest request;
        request.source = &source;
        request.inputMapping = &mapping;
        request.logicalBitStart = 0;
        request.logicalBitLength = 32;

        const auto result = provider.transform(request);
        QVERIFY(result.succeeded());
        QCOMPARE(result.diagnostics.size(), 1ULL);
        QVERIFY(result.diagnostics[0].message.contains(QStringLiteral("00 00 02")));
    }

    // 4. Prohibited 00 00 03 xx (xx > 03)
    {
        const auto data = toBytes({0x00, 0x00, 0x03, 0x04});
        const MemorySource source(data);
        const auto mapping = makeSingleSpanMapping(0, 4);
        PayloadTransformRequest request;
        request.source = &source;
        request.inputMapping = &mapping;
        request.logicalBitStart = 0;
        request.logicalBitLength = 32;

        const auto result = provider.transform(request);
        QVERIFY(result.succeeded());
        QCOMPARE(result.excludedSpans.size(), 1ULL); // 0x03 still excluded
        QCOMPARE(result.diagnostics.size(), 1ULL);
        QVERIFY(result.diagnostics[0].message.contains(QStringLiteral("00 00 03 xx")));
    }

    // 5. Terminal 00 00 03
    {
        const auto data = toBytes({0x00, 0x00, 0x03});
        const MemorySource source(data);
        const auto mapping = makeSingleSpanMapping(0, 3);
        PayloadTransformRequest request;
        request.source = &source;
        request.inputMapping = &mapping;
        request.logicalBitStart = 0;
        request.logicalBitLength = 24;

        const auto result = provider.transform(request);
        QVERIFY(result.succeeded());
        QCOMPARE(result.forwardedMapping->logicalBitLength(), 16ULL);
        QCOMPARE(result.excludedSpans.size(), 1ULL);
    }

    // 6. Non-empty input ending in 00
    {
        const auto data = toBytes({0x01, 0x02, 0x00});
        const MemorySource source(data);
        const auto mapping = makeSingleSpanMapping(0, 3);
        PayloadTransformRequest request;
        request.source = &source;
        request.inputMapping = &mapping;
        request.logicalBitStart = 0;
        request.logicalBitLength = 24;

        const auto result = provider.transform(request);
        QVERIFY(result.succeeded());
        QCOMPARE(result.diagnostics.size(), 1ULL);
        QVERIFY(result.diagnostics[0].message.contains(QStringLiteral("final byte must not be 00")));
    }
}

void PayloadTransformTest::rbspTransformSourceErrorPropagation() {
    const auto data = toBytes({0x00, 0x00, 0x03, 0x01});
    const FaultySource source(data, 2); // Fails reading byte 2
    const auto mapping = makeSingleSpanMapping(0, 4);

    H264RbspPayloadTransformProvider provider;
    PayloadTransformRequest request;
    request.source = &source;
    request.inputMapping = &mapping;
    request.logicalBitStart = 0;
    request.logicalBitLength = 32;

    const auto result = provider.transform(request);
    QCOMPARE(result.status, DslExecutionStatus::SourceError);
    QVERIFY(!result.succeeded());
}

void PayloadTransformTest::rbspTransformEndOfSourcePropagation() {
    const auto data = toBytes({0x00, 0x00});
    const MemorySource source(data);
    const auto mapping = makeSingleSpanMapping(0, 4); // claims 4 bytes on a 2-byte source

    H264RbspPayloadTransformProvider provider;
    PayloadTransformRequest request;
    request.source = &source;
    request.inputMapping = &mapping;
    request.logicalBitStart = 0;
    request.logicalBitLength = 32;

    const auto result = provider.transform(request);
    QCOMPARE(result.status, DslExecutionStatus::InvalidDefinition);
    QVERIFY(!result.succeeded());
}

void PayloadTransformTest::rbspTransformMidInspectionCancellation() {
    std::vector<std::byte> data(2048, std::byte{0x01});
    CancellationSource cancelSource;
    CancellingSource source(data, 1024, cancelSource); // Cancels at 1024 bytes (matching kCancellationInterval)
    const auto mapping = makeSingleSpanMapping(0, 2048);

    H264RbspPayloadTransformProvider provider;
    PayloadTransformRequest request;
    request.source = &source;
    request.inputMapping = &mapping;
    request.logicalBitStart = 0;
    request.logicalBitLength = 2048 * 8U;
    request.cancellation = cancelSource.token();

    const auto result = provider.transform(request);
    QCOMPARE(result.status, DslExecutionStatus::Cancelled);
    QVERIFY(!result.succeeded());
    QCOMPARE(result.inspectedByteCount, 1024ULL);
}

void PayloadTransformTest::rbspTransformInspectionBudgetExceededAndExactBoundary() {
    const auto data = toBytes({0x00, 0x00, 0x03, 0x01});
    const MemorySource source(data);
    const auto mapping = makeSingleSpanMapping(0, 4);

    H264RbspPayloadTransformProvider provider;

    // Budget = 3 on 4-byte input -> ResourceLimit
    {
        PayloadTransformRequest request;
        request.source = &source;
        request.inputMapping = &mapping;
        request.logicalBitStart = 0;
        request.logicalBitLength = 32;
        request.maximumInspectedBytes = 3;

        const auto result = provider.transform(request);
        QCOMPARE(result.status, DslExecutionStatus::ResourceLimit);
        QCOMPARE(result.inspectedByteCount, 3ULL);
        QVERIFY(!result.succeeded());
    }

    // Budget = 4 on 4-byte input -> Materialized
    {
        PayloadTransformRequest request;
        request.source = &source;
        request.inputMapping = &mapping;
        request.logicalBitStart = 0;
        request.logicalBitLength = 32;
        request.maximumInspectedBytes = 4;

        const auto result = provider.transform(request);
        QCOMPARE(result.status, DslExecutionStatus::Materialized);
        QCOMPARE(result.inspectedByteCount, 4ULL);
        QVERIFY(result.succeeded());
    }
}

void PayloadTransformTest::rbspTransformUnalignedAndOutOfBoundsRejection() {
    const auto data = toBytes({0x00, 0x00, 0x03, 0x01});
    const MemorySource source(data);
    const auto mapping = makeSingleSpanMapping(0, 4);

    H264RbspPayloadTransformProvider provider;

    // Start unaligned
    {
        PayloadTransformRequest req;
        req.source = &source;
        req.inputMapping = &mapping;
        req.logicalBitStart = 3;
        req.logicalBitLength = 8;
        QCOMPARE(provider.transform(req).status, DslExecutionStatus::InvalidDefinition);
    }

    // Length unaligned
    {
        PayloadTransformRequest req;
        req.source = &source;
        req.inputMapping = &mapping;
        req.logicalBitStart = 0;
        req.logicalBitLength = 7;
        QCOMPARE(provider.transform(req).status, DslExecutionStatus::InvalidDefinition);
    }

    // Zero length
    {
        PayloadTransformRequest req;
        req.source = &source;
        req.inputMapping = &mapping;
        req.logicalBitStart = 0;
        req.logicalBitLength = 0;
        QCOMPARE(provider.transform(req).status, DslExecutionStatus::InvalidDefinition);
    }

    // Null inputs
    {
        PayloadTransformRequest req1;
        req1.source = nullptr;
        req1.inputMapping = &mapping;
        QCOMPARE(provider.transform(req1).status, DslExecutionStatus::InvalidDefinition);

        PayloadTransformRequest req2;
        req2.source = &source;
        req2.inputMapping = nullptr;
        QCOMPARE(provider.transform(req2).status, DslExecutionStatus::InvalidDefinition);
    }

    // Out of bounds
    {
        PayloadTransformRequest req;
        req.source = &source;
        req.inputMapping = &mapping;
        req.logicalBitStart = 24;
        req.logicalBitLength = 16; // exceeds 32 bits
        QCOMPARE(provider.transform(req).status, DslExecutionStatus::InvalidDefinition);
    }
}

void PayloadTransformTest::registryRegisterAndLifecycleRbspProvider() {
    PayloadTransformRegistry registry;
    auto rbspProvider = std::make_shared<H264RbspPayloadTransformProvider>();

    QVERIFY(registry.registerProvider(rbspProvider));
    const auto found = registry.findProvider(QStringLiteral("rbsp"));
    QVERIFY(found != nullptr);
    QCOMPARE(found->identifier(), QStringLiteral("rbsp"));

    // Duplicate registration rejected
    QVERIFY(!registry.registerProvider(std::make_shared<H264RbspPayloadTransformProvider>()));

    // Unregister rbsp
    QVERIFY(registry.unregisterProvider(QStringLiteral("rbsp")));
    QCOMPARE(registry.findProvider(QStringLiteral("rbsp")), nullptr);

    // Re-register then reset
    QVERIFY(registry.registerProvider(rbspProvider));
    registry.reset();
    QCOMPARE(registry.findProvider(QStringLiteral("rbsp")), nullptr);
    QVERIFY(registry.findProvider(QStringLiteral("none")) != nullptr);
}

} // namespace streamview::rules

QTEST_MAIN(streamview::rules::PayloadTransformTest)
#include "payload_transform_test.moc"
