#include <streamview/rules/compound_structural_runner.h>

#include <streamview/core/analysis_model.h>
#include <streamview/core/cancellation.h>
#include <streamview/core/coordinates.h>
#include <streamview/core/source.h>
#include <streamview/rules/dsl.h>
#include <streamview/rules/dsl_ir.h>

#include <QString>
#include <QtTest>

#include <memory>
#include <stdexcept>
#include <vector>

using namespace streamview::core;
using namespace streamview::rules;

namespace {

[[nodiscard]] std::vector<std::byte> toBytes(std::initializer_list<quint8> list) {
    std::vector<std::byte> result;
    result.reserve(list.size());
    for (const quint8 b : list) {
        result.push_back(static_cast<std::byte>(b));
    }
    return result;
}

[[nodiscard]] SourceMapping makeMapping(quint64 startByte, quint64 numBytes, quint32 viewId = 1) {
    const auto span = SourceSpan::create(SourceBitAddress(startByte * 8), numBytes * 8);
    return *SourceMapping::create(LogicalViewId(viewId), {*span});
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

[[nodiscard]] std::shared_ptr<DslTypedProgram> compileDslProgram(const QString& dslText) {
    const auto parseResult = DslParser::parse(dslText);
    if (!parseResult.succeeded()) {
        return nullptr;
    }
    const auto compileResult = DslCompiler::compile(parseResult.program);
    if (!compileResult.succeeded() || !compileResult.program) {
        return nullptr;
    }
    return std::make_shared<DslTypedProgram>(*compileResult.program);
}

class MockFilteringProvider final : public PayloadTransformProvider {
public:
    [[nodiscard]] QString identifier() const override {
        return QStringLiteral("mock_filter");
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
                result.errorMessage = QStringLiteral("Invalid range");
                return result;
            }
            const auto location = request.inputMapping->locate(*rangeOpt);
            if (!location || location->sourceSpans().empty()) {
                result.status = DslExecutionStatus::InvalidDefinition;
                result.errorMessage = QStringLiteral("Failed to locate byte");
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
                // Exclude
                excludedSpans.push_back({byteSpan, outputBitOffset});
            } else {
                // Forward
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

class MalformedTransformProvider final : public PayloadTransformProvider {
public:
    [[nodiscard]] QString identifier() const override {
        return QStringLiteral("malformed_transform");
    }

    [[nodiscard]] PayloadTransformResult transform(
        const PayloadTransformRequest& request) const override {
        PayloadTransformResult result;
        result.status = DslExecutionStatus::Materialized;
        if (request.inputMapping == nullptr || request.inputMapping->sourceSpans().empty()) {
            result.status = DslExecutionStatus::InvalidDefinition;
            result.errorMessage = QStringLiteral("Missing input mapping");
            return result;
        }
        // Omit one input byte without an excluded record. The runner must reject
        // the result before handing a shorter range to the payload VM.
        const auto firstByteRange = LogicalRange::create(
            LogicalBitAddress(request.inputMapping->viewId(), request.logicalBitStart), 8U);
        const auto firstByte = firstByteRange
            ? request.inputMapping->locate(*firstByteRange)
            : std::nullopt;
        if (!firstByte) {
            result.status = DslExecutionStatus::InvalidDefinition;
            result.errorMessage = QStringLiteral("Unable to build malformed mapping");
            return result;
        }
        result.forwardedMapping = SourceMapping::create(
            request.inputMapping->viewId(), firstByte->sourceSpans());
        return result;
    }
};

class DiagnosticTransformProvider final : public PayloadTransformProvider {
public:
    [[nodiscard]] QString identifier() const override {
        return QStringLiteral("diagnostic_transform");
    }

    [[nodiscard]] PayloadTransformResult transform(
        const PayloadTransformRequest& request) const override {
        IdentityPayloadTransformProvider identity;
        auto result = identity.transform(request);
        if (result.succeeded()) {
            ParseDiagnostic diagnostic;
            diagnostic.code = DiagnosticCode::UnsupportedSyntax;
            diagnostic.severity = DiagnosticSeverity::Warning;
            diagnostic.message = QStringLiteral("Transform provider warning");
            result.diagnostics.push_back(std::move(diagnostic));
        }
        return result;
    }
};

} // namespace

class CompoundStructuralRunnerTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    // 1. Success cases
    void executesSuccessfulHeaderAndPayloadInSingleTree();

    // 2. Header failure halts compound execution
    void headerTruncationHaltsAndSkipsPayload();
    void headerSourceErrorHaltsAndSkipsPayload();
    void headerInvalidSyntaxHaltsAndSkipsPayload();

    // 3. Payload failure transitions header to corresponding terminal state
    void payloadTruncationTransitionsHeaderToInvalid();
    void payloadSourceErrorTransitionsHeaderToInvalid();
    void payloadUnsupportedTransitionsHeaderToUnsupported();
    void payloadInvalidSyntaxTransitionsHeaderToInvalid();

    // 4. Shared compound budget exhaustion and arithmetic overflow protection
    void sharedInstructionBudgetExhaustion();
    void sharedNodeBudgetExhaustion();
    void reportsCombinedExecutionCountsAtExactBudget();

    // 5. Cancellation tokens
    void preCancellationHaltsBeforeAnyTreeMutation();
    void headerPhaseCancellation();
    void betweenHeaderAndPayloadCancellation();
    void payloadPhaseCancellation();

    // 6. Header-only execution and exact consumption
    void headerOnlyExecutionWithExactConsumptionSuccess();
    void headerOnlyExecutionFailsOnUnconsumedTrailingBits();
    void headerWithPayloadFailsOnUnconsumedHeaderBits();
    void payloadFailsOnUnconsumedTrailingBits();
    void executesPayloadFromNonZeroLogicalStart();

    // 7. Transaction hook failures
    void commitHookExceptionFailsClosedAndRollsBack();
    void rollbackHookExceptionFailsClosed();

    // 8. Repeated execution and isolation
    void repeatingExecutionDoesNotProduceOrphanNodes();

    // 9. Preflight validation of inputs
    void rejectsInvalidInputContracts();

    // 10. Transform provider integration tests
    void executesCompoundWithCustomTransformProviderAndExcludedSpans();
    void compoundFailsClosedOnUnknownTransformProvider();
    void compoundPropagatesTransformSourceError();
    void compoundPropagatesTransformTruncation();
    void compoundPropagatesTransformCancellation();
    void compoundPropagatesTransformInspectionBudgetExceeded();
    void compoundRejectsZeroLengthPayloadInput();
    void compoundRejectsMalformedTransformResult();
    void compoundPublishesTransformDiagnostics();
    void rejectsInspectionBudgetAboveSandboxBound();

private:
    std::shared_ptr<DslTypedProgram> testProgram_;
};

void CompoundStructuralRunnerTest::initTestCase() {
    const QString dsl = QStringLiteral(R"(
struct Header {
    bits<1> forbidden_zero_bit @equals(0);
    bits<2> nal_ref_idc;
    bits<5> nal_unit_type;
}

struct PayloadA {
    bits<8> data_byte;
    bits<8> more_byte;
}

struct PayloadUnsupported {
    bits<8> unsupported_field;
    if (unsupported_field == 255) {
        unsupported("Feature not supported in this profile") at unsupported_field;
    }
}

struct HeaderLarge {
    bits<16> field1;
    bits<16> field2;
}

entry Header;
)");

    testProgram_ = compileDslProgram(dsl);
    QVERIFY(testProgram_ != nullptr);
}

void CompoundStructuralRunnerTest::cleanupTestCase() {
    testProgram_.reset();
}

void CompoundStructuralRunnerTest::executesSuccessfulHeaderAndPayloadInSingleTree() {
    // Header (8 bits: 0x67 -> 0, 3, 7) + PayloadA (16 bits: 0x12, 0x34) = 3 bytes
    const auto data = toBytes({0x67, 0x12, 0x34});
    const MemorySource source(data);

    const auto headerMapping = makeMapping(0, 1, 1);
    const auto payloadMapping = makeMapping(1, 2, 2);

    auto treeOpt = AnalysisTree::create(QStringLiteral("Root"));
    QVERIFY(treeOpt.has_value());
    AnalysisTree tree = std::move(*treeOpt);

    const auto headerIndex = testProgram_->structureIndex(QStringLiteral("Header"));
    const auto payloadIndex = testProgram_->structureIndex(QStringLiteral("PayloadA"));
    QVERIFY(headerIndex.has_value());
    QVERIFY(payloadIndex.has_value());

    int commitCount = 0;
    int rollbackCount = 0;
    std::optional<MaterializationState> headerStateAtCommit;
    std::optional<MaterializationState> payloadStateAtCommit;

    CompoundStructuralExecutionRequest request;
    request.source = &source;
    request.headerMapping = &headerMapping;
    request.headerStructureIndex = *headerIndex;
    request.payloadStructureIndex = *payloadIndex;
    request.payloadMapping = &payloadMapping;
    request.payloadLogicalStart = 0;
    request.tree = &tree;
    request.parentId = tree.rootId();
    request.transactionHooks.onCommit = [&]() {
        ++commitCount;
        const auto root = tree.node(tree.rootId());
        if (!root || root->children().empty()) {
            return;
        }
        const auto header = tree.node(root->children().back());
        if (!header || header->children().empty()) {
            return;
        }
        const auto payload = tree.node(header->children().back());
        headerStateAtCommit = header->state();
        if (payload) {
            payloadStateAtCommit = payload->state();
        }
    };
    request.transactionHooks.onRollback = [&rollbackCount]() { ++rollbackCount; };

    const auto result = CompoundStructuralRunner::execute(*testProgram_, request);

    QVERIFY(result.materialized());
    QCOMPARE(result.status, DslExecutionStatus::Materialized);
    QCOMPARE(commitCount, 1);
    QCOMPARE(rollbackCount, 0);
    QCOMPARE(headerStateAtCommit, std::optional(MaterializationState::Indexing));
    QCOMPARE(payloadStateAtCommit, std::optional(MaterializationState::Indexing));

    QVERIFY(result.headerNodeId.has_value());
    QVERIFY(result.payloadNodeId.has_value());
    QCOMPARE(result.headerBitsConsumed, 8ULL);
    QCOMPARE(result.payloadBitsConsumed, 16ULL);

    // Verify tree hierarchy: root -> Header -> PayloadA
    const auto headerNode = tree.node(*result.headerNodeId);
    QVERIFY(headerNode.has_value());
    QCOMPARE(headerNode->name(), QStringLiteral("Header"));
    QCOMPARE(headerNode->parentId(), tree.rootId());
    QCOMPARE(headerNode->state(), MaterializationState::Materialized);

    const auto payloadNode = tree.node(*result.payloadNodeId);
    QVERIFY(payloadNode.has_value());
    QCOMPARE(payloadNode->name(), QStringLiteral("PayloadA"));
    QCOMPARE(payloadNode->parentId(), *result.headerNodeId);
    QCOMPARE(payloadNode->state(), MaterializationState::Materialized);

    // Header field values captured
    QVERIFY(result.headerFieldValues.size() >= 3);
    QCOMPARE(result.headerFieldValues.at(0), std::optional<quint64>(0));
    QCOMPARE(result.headerFieldValues.at(1), std::optional<quint64>(3));
    QCOMPARE(result.headerFieldValues.at(2), std::optional<quint64>(7));
}

void CompoundStructuralRunnerTest::headerTruncationHaltsAndSkipsPayload() {
    // Only 1 byte provided, but HeaderLarge requires 4 bytes (32 bits)
    const auto data = toBytes({0x12});
    const MemorySource source(data);
    const auto headerMapping = makeMapping(0, 1, 1);
    const auto payloadMapping = makeMapping(0, 1, 2);

    auto treeOpt = AnalysisTree::create(QStringLiteral("Root"));
    AnalysisTree tree = std::move(*treeOpt);

    const auto headerIndex = testProgram_->structureIndex(QStringLiteral("HeaderLarge"));
    const auto payloadIndex = testProgram_->structureIndex(QStringLiteral("PayloadA"));

    int commitCount = 0;
    int rollbackCount = 0;

    CompoundStructuralExecutionRequest request;
    request.source = &source;
    request.headerMapping = &headerMapping;
    request.headerStructureIndex = *headerIndex;
    request.payloadStructureIndex = *payloadIndex;
    request.payloadMapping = &payloadMapping;
    request.tree = &tree;
    request.parentId = tree.rootId();
    request.transactionHooks.onCommit = [&commitCount]() { ++commitCount; };
    request.transactionHooks.onRollback = [&rollbackCount]() { ++rollbackCount; };

    const auto result = CompoundStructuralRunner::execute(*testProgram_, request);

    QCOMPARE(result.status, DslExecutionStatus::TruncatedSource);
    QCOMPARE(result.payloadNodeId, std::nullopt);
    QCOMPARE(commitCount, 0);
    QCOMPARE(rollbackCount, 1);

    // Header node is partial with TruncatedSource diagnostic
    QVERIFY(result.headerNodeId.has_value());
    const auto headerNode = tree.node(*result.headerNodeId);
    QVERIFY(headerNode.has_value());
    QCOMPARE(headerNode->state(), MaterializationState::Invalid);
}

void CompoundStructuralRunnerTest::headerSourceErrorHaltsAndSkipsPayload() {
    // FaultySource fails on reading byte 0
    const auto data = toBytes({0x67, 0x12});
    const FaultySource source(data, 0);
    const auto headerMapping = makeMapping(0, 1, 1);
    const auto payloadMapping = makeMapping(1, 1, 2);

    auto treeOpt = AnalysisTree::create(QStringLiteral("Root"));
    AnalysisTree tree = std::move(*treeOpt);

    const auto headerIndex = testProgram_->structureIndex(QStringLiteral("Header"));
    const auto payloadIndex = testProgram_->structureIndex(QStringLiteral("PayloadA"));

    int rollbackCount = 0;

    CompoundStructuralExecutionRequest request;
    request.source = &source;
    request.headerMapping = &headerMapping;
    request.headerStructureIndex = *headerIndex;
    request.payloadStructureIndex = *payloadIndex;
    request.payloadMapping = &payloadMapping;
    request.tree = &tree;
    request.parentId = tree.rootId();
    request.transactionHooks.onRollback = [&rollbackCount]() { ++rollbackCount; };

    const auto result = CompoundStructuralRunner::execute(*testProgram_, request);

    QCOMPARE(result.status, DslExecutionStatus::SourceError);
    QCOMPARE(result.payloadNodeId, std::nullopt);
    QCOMPARE(rollbackCount, 1);
}

void CompoundStructuralRunnerTest::headerInvalidSyntaxHaltsAndSkipsPayload() {
    // Header requires forbidden_zero_bit == 0. Provide 0x80 (forbidden_zero_bit = 1)
    const auto data = toBytes({0x80, 0x12, 0x34});
    const MemorySource source(data);
    const auto headerMapping = makeMapping(0, 1, 1);
    const auto payloadMapping = makeMapping(1, 2, 2);

    auto treeOpt = AnalysisTree::create(QStringLiteral("Root"));
    AnalysisTree tree = std::move(*treeOpt);

    const auto headerIndex = testProgram_->structureIndex(QStringLiteral("Header"));
    const auto payloadIndex = testProgram_->structureIndex(QStringLiteral("PayloadA"));

    int rollbackCount = 0;

    CompoundStructuralExecutionRequest request;
    request.source = &source;
    request.headerMapping = &headerMapping;
    request.headerStructureIndex = *headerIndex;
    request.payloadStructureIndex = *payloadIndex;
    request.payloadMapping = &payloadMapping;
    request.tree = &tree;
    request.parentId = tree.rootId();
    request.transactionHooks.onRollback = [&rollbackCount]() { ++rollbackCount; };

    const auto result = CompoundStructuralRunner::execute(*testProgram_, request);

    QCOMPARE(result.status, DslExecutionStatus::InvalidSyntax);
    QCOMPARE(result.payloadNodeId, std::nullopt);
    QCOMPARE(rollbackCount, 1);
}

void CompoundStructuralRunnerTest::payloadTruncationTransitionsHeaderToInvalid() {
    // Header succeeds (8 bits: 0x67). PayloadA expects 16 bits, but payloadMapping has only 8 bits.
    const auto data = toBytes({0x67, 0x12});
    const MemorySource source(data);
    const auto headerMapping = makeMapping(0, 1, 1);
    const auto payloadMapping = makeMapping(1, 1, 2);

    auto treeOpt = AnalysisTree::create(QStringLiteral("Root"));
    AnalysisTree tree = std::move(*treeOpt);

    const auto headerIndex = testProgram_->structureIndex(QStringLiteral("Header"));
    const auto payloadIndex = testProgram_->structureIndex(QStringLiteral("PayloadA"));

    int rollbackCount = 0;

    CompoundStructuralExecutionRequest request;
    request.source = &source;
    request.headerMapping = &headerMapping;
    request.headerStructureIndex = *headerIndex;
    request.payloadStructureIndex = *payloadIndex;
    request.payloadMapping = &payloadMapping;
    request.tree = &tree;
    request.parentId = tree.rootId();
    request.transactionHooks.onRollback = [&rollbackCount]() { ++rollbackCount; };

    const auto result = CompoundStructuralRunner::execute(*testProgram_, request);

    QCOMPARE(result.status, DslExecutionStatus::TruncatedSource);
    QCOMPARE(rollbackCount, 1);

    // Parent header transitioned to Invalid
    QVERIFY(result.headerNodeId.has_value());
    const auto headerNode = tree.node(*result.headerNodeId);
    QVERIFY(headerNode.has_value());
    QCOMPARE(headerNode->state(), MaterializationState::Invalid);
}

void CompoundStructuralRunnerTest::payloadSourceErrorTransitionsHeaderToInvalid() {
    // Header reads byte 0 ok. FaultySource fails at byte 1 (during payload read).
    const auto data = toBytes({0x67, 0x12, 0x34});
    const FaultySource source(data, 1);
    const auto headerMapping = makeMapping(0, 1, 1);
    const auto payloadMapping = makeMapping(1, 2, 2);

    auto treeOpt = AnalysisTree::create(QStringLiteral("Root"));
    AnalysisTree tree = std::move(*treeOpt);

    const auto headerIndex = testProgram_->structureIndex(QStringLiteral("Header"));
    const auto payloadIndex = testProgram_->structureIndex(QStringLiteral("PayloadA"));

    int rollbackCount = 0;

    CompoundStructuralExecutionRequest request;
    request.source = &source;
    request.headerMapping = &headerMapping;
    request.headerStructureIndex = *headerIndex;
    request.payloadStructureIndex = *payloadIndex;
    request.payloadMapping = &payloadMapping;
    request.tree = &tree;
    request.parentId = tree.rootId();
    request.transactionHooks.onRollback = [&rollbackCount]() { ++rollbackCount; };

    const auto result = CompoundStructuralRunner::execute(*testProgram_, request);

    QCOMPARE(result.status, DslExecutionStatus::SourceError);
    QCOMPARE(rollbackCount, 1);

    const auto headerNode = tree.node(*result.headerNodeId);
    QVERIFY(headerNode.has_value());
    QCOMPARE(headerNode->state(), MaterializationState::Invalid);
}

void CompoundStructuralRunnerTest::payloadUnsupportedTransitionsHeaderToUnsupported() {
    // PayloadUnsupported with unsupported_field = 0xFF triggers @unsupported assertion
    const auto data = toBytes({0x67, 0xFF});
    const MemorySource source(data);
    const auto headerMapping = makeMapping(0, 1, 1);
    const auto payloadMapping = makeMapping(1, 1, 2);

    auto treeOpt = AnalysisTree::create(QStringLiteral("Root"));
    AnalysisTree tree = std::move(*treeOpt);

    const auto headerIndex = testProgram_->structureIndex(QStringLiteral("Header"));
    const auto payloadIndex = testProgram_->structureIndex(QStringLiteral("PayloadUnsupported"));

    int rollbackCount = 0;

    CompoundStructuralExecutionRequest request;
    request.source = &source;
    request.headerMapping = &headerMapping;
    request.headerStructureIndex = *headerIndex;
    request.payloadStructureIndex = *payloadIndex;
    request.payloadMapping = &payloadMapping;
    request.tree = &tree;
    request.parentId = tree.rootId();
    request.transactionHooks.onRollback = [&rollbackCount]() { ++rollbackCount; };

    const auto result = CompoundStructuralRunner::execute(*testProgram_, request);

    QCOMPARE(result.status, DslExecutionStatus::Unsupported);
    QCOMPARE(rollbackCount, 1);

    // Parent header transitioned to Unsupported
    const auto headerNode = tree.node(*result.headerNodeId);
    QVERIFY(headerNode.has_value());
    QCOMPARE(headerNode->state(), MaterializationState::Unsupported);
}

void CompoundStructuralRunnerTest::payloadInvalidSyntaxTransitionsHeaderToInvalid() {
    // Compile DSL with constraint
    const QString dsl = QStringLiteral(R"(
struct Hdr {
    bits<8> type;
}
struct PayloadConstrained {
    bits<8> magic @equals(170);
}

entry Hdr;
)");
    const auto prog = compileDslProgram(dsl);
    QVERIFY(prog != nullptr);

    // Magic provided is 0x55 (mismatch)
    const auto data = toBytes({0x01, 0x55});
    const MemorySource source(data);
    const auto headerMapping = makeMapping(0, 1, 1);
    const auto payloadMapping = makeMapping(1, 1, 2);

    auto treeOpt = AnalysisTree::create(QStringLiteral("Root"));
    AnalysisTree tree = std::move(*treeOpt);

    CompoundStructuralExecutionRequest request;
    request.source = &source;
    request.headerMapping = &headerMapping;
    request.headerStructureIndex = *prog->structureIndex(QStringLiteral("Hdr"));
    request.payloadStructureIndex = *prog->structureIndex(QStringLiteral("PayloadConstrained"));
    request.payloadMapping = &payloadMapping;
    request.tree = &tree;
    request.parentId = tree.rootId();

    const auto result = CompoundStructuralRunner::execute(*prog, request);

    QCOMPARE(result.status, DslExecutionStatus::InvalidSyntax);
    const auto headerNode = tree.node(*result.headerNodeId);
    QVERIFY(headerNode.has_value());
    QCOMPARE(headerNode->state(), MaterializationState::Invalid);
}

void CompoundStructuralRunnerTest::sharedInstructionBudgetExhaustion() {
    // Leave exactly one instruction for the payload structure begin.
    const auto data = toBytes({0x67, 0x12, 0x34});
    const MemorySource source(data);
    const auto headerMapping = makeMapping(0, 1, 1);
    const auto payloadMapping = makeMapping(1, 2, 2);

    auto treeOpt = AnalysisTree::create(QStringLiteral("Root"));
    AnalysisTree tree = std::move(*treeOpt);

    const auto headerIndex = testProgram_->structureIndex(QStringLiteral("Header"));
    const auto payloadIndex = testProgram_->structureIndex(QStringLiteral("PayloadA"));
    const quint64 headerInstructions = testProgram_->structs.at(*headerIndex).bytecodeLength;

    CompoundStructuralExecutionRequest request;
    request.source = &source;
    request.headerMapping = &headerMapping;
    request.headerStructureIndex = *headerIndex;
    request.payloadStructureIndex = *payloadIndex;
    request.payloadMapping = &payloadMapping;
    request.tree = &tree;
    request.parentId = tree.rootId();
    request.options.limits.maximumInstructions = headerInstructions + 1;

    const auto result = CompoundStructuralRunner::execute(*testProgram_, request);

    QCOMPARE(result.status, DslExecutionStatus::ResourceLimit);
    QCOMPARE(result.instructionsExecuted, headerInstructions + 1);
    QVERIFY(result.payloadNodeId.has_value());
    const auto headerNode = tree.node(*result.headerNodeId);
    QVERIFY(headerNode.has_value());
    QCOMPARE(headerNode->state(), MaterializationState::Invalid);
}

void CompoundStructuralRunnerTest::sharedNodeBudgetExhaustion() {
    // Header creates four nodes, leaving one for the payload structure itself.
    const auto data = toBytes({0x67, 0x12, 0x34});
    const MemorySource source(data);
    const auto headerMapping = makeMapping(0, 1, 1);
    const auto payloadMapping = makeMapping(1, 2, 2);

    auto treeOpt = AnalysisTree::create(QStringLiteral("Root"));
    AnalysisTree tree = std::move(*treeOpt);

    const auto headerIndex = testProgram_->structureIndex(QStringLiteral("Header"));
    const auto payloadIndex = testProgram_->structureIndex(QStringLiteral("PayloadA"));

    CompoundStructuralExecutionRequest request;
    request.source = &source;
    request.headerMapping = &headerMapping;
    request.headerStructureIndex = *headerIndex;
    request.payloadStructureIndex = *payloadIndex;
    request.payloadMapping = &payloadMapping;
    request.tree = &tree;
    request.parentId = tree.rootId();
    request.options.limits.maximumMaterializedNodes = 5;

    const auto result = CompoundStructuralRunner::execute(*testProgram_, request);

    QCOMPARE(result.status, DslExecutionStatus::ResourceLimit);
    QCOMPARE(result.nodesCreated, 5ULL);
    QVERIFY(result.payloadNodeId.has_value());
    const auto headerNode = tree.node(*result.headerNodeId);
    QVERIFY(headerNode.has_value());
    QCOMPARE(headerNode->state(), MaterializationState::Invalid);
}

void CompoundStructuralRunnerTest::reportsCombinedExecutionCountsAtExactBudget() {
    const auto data = toBytes({0x67, 0x12, 0x34});
    const MemorySource source(data);
    const auto headerMapping = makeMapping(0, 1, 1);
    const auto payloadMapping = makeMapping(1, 2, 2);

    auto treeOpt = AnalysisTree::create(QStringLiteral("Root"));
    AnalysisTree tree = std::move(*treeOpt);

    const auto headerIndex = testProgram_->structureIndex(QStringLiteral("Header"));
    const auto payloadIndex = testProgram_->structureIndex(QStringLiteral("PayloadA"));
    const quint64 totalInstructions =
        testProgram_->structs.at(*headerIndex).bytecodeLength +
        testProgram_->structs.at(*payloadIndex).bytecodeLength;

    CompoundStructuralExecutionRequest request;
    request.source = &source;
    request.headerMapping = &headerMapping;
    request.headerStructureIndex = *headerIndex;
    request.payloadStructureIndex = *payloadIndex;
    request.payloadMapping = &payloadMapping;
    request.tree = &tree;
    request.parentId = tree.rootId();
    request.options.limits.maximumInstructions = totalInstructions;
    request.options.limits.maximumMaterializedNodes = 7;

    const auto result = CompoundStructuralRunner::execute(*testProgram_, request);

    QVERIFY(result.materialized());
    QCOMPARE(result.instructionsExecuted, totalInstructions);
    QCOMPARE(result.nodesCreated, 7ULL);
}

void CompoundStructuralRunnerTest::preCancellationHaltsBeforeAnyTreeMutation() {
    const auto data = toBytes({0x67, 0x12, 0x34});
    const MemorySource source(data);
    const auto headerMapping = makeMapping(0, 1, 1);
    const auto payloadMapping = makeMapping(1, 2, 2);

    auto treeOpt = AnalysisTree::create(QStringLiteral("Root"));
    AnalysisTree tree = std::move(*treeOpt);
    const std::size_t initialNodes = tree.nodeCount();

    const auto headerIndex = testProgram_->structureIndex(QStringLiteral("Header"));
    const auto payloadIndex = testProgram_->structureIndex(QStringLiteral("PayloadA"));

    CancellationSource cancelSource;
    (void)cancelSource.requestCancellation();

    CompoundStructuralExecutionRequest request;
    request.source = &source;
    request.headerMapping = &headerMapping;
    request.headerStructureIndex = *headerIndex;
    request.payloadStructureIndex = *payloadIndex;
    request.payloadMapping = &payloadMapping;
    request.tree = &tree;
    request.parentId = tree.rootId();
    request.options.cancellation = cancelSource.token();

    const auto result = CompoundStructuralRunner::execute(*testProgram_, request);

    QCOMPARE(result.status, DslExecutionStatus::Cancelled);
    QCOMPARE(result.headerNodeId, std::nullopt);
    QCOMPARE(tree.nodeCount(), initialNodes);
}

void CompoundStructuralRunnerTest::headerPhaseCancellation() {
    const auto data = toBytes({0x67, 0x12, 0x34});
    CancellationSource cancelSource;
    const CancellingSource source(data, 0, cancelSource);
    const auto headerMapping = makeMapping(0, 1, 1);
    const auto payloadMapping = makeMapping(1, 2, 2);

    auto treeOpt = AnalysisTree::create(QStringLiteral("Root"));
    AnalysisTree tree = std::move(*treeOpt);

    const auto headerIndex = testProgram_->structureIndex(QStringLiteral("Header"));
    const auto payloadIndex = testProgram_->structureIndex(QStringLiteral("PayloadA"));

    CompoundStructuralExecutionRequest request;
    request.source = &source;
    request.headerMapping = &headerMapping;
    request.headerStructureIndex = *headerIndex;
    request.payloadStructureIndex = *payloadIndex;
    request.payloadMapping = &payloadMapping;
    request.tree = &tree;
    request.parentId = tree.rootId();
    request.options.cancellation = cancelSource.token();
    request.options.limits.cancellationCheckInterval = 1;
    int rollbackCount = 0;
    request.transactionHooks.onRollback = [&rollbackCount]() { ++rollbackCount; };

    const auto result = CompoundStructuralRunner::execute(*testProgram_, request);

    QCOMPARE(result.status, DslExecutionStatus::Cancelled);
    QVERIFY(result.headerNodeId.has_value());
    QCOMPARE(result.payloadNodeId, std::nullopt);
    QCOMPARE(rollbackCount, 1);
    QCOMPARE(tree.node(*result.headerNodeId)->state(), MaterializationState::Cancelled);
}

void CompoundStructuralRunnerTest::betweenHeaderAndPayloadCancellation() {
    const auto data = toBytes({0x67, 0x12, 0x34});
    CancellationSource cancelSource;
    const CancellingSource source(data, 0, cancelSource);
    const auto headerMapping = makeMapping(0, 1, 1);
    const auto payloadMapping = makeMapping(1, 2, 2);

    auto treeOpt = AnalysisTree::create(QStringLiteral("Root"));
    AnalysisTree tree = std::move(*treeOpt);

    const auto headerIndex = testProgram_->structureIndex(QStringLiteral("Header"));
    const auto payloadIndex = testProgram_->structureIndex(QStringLiteral("PayloadA"));

    CompoundStructuralExecutionRequest request;
    request.source = &source;
    request.headerMapping = &headerMapping;
    request.headerStructureIndex = *headerIndex;
    request.payloadStructureIndex = *payloadIndex;
    request.payloadMapping = &payloadMapping;
    request.tree = &tree;
    request.parentId = tree.rootId();
    request.options.cancellation = cancelSource.token();
    int rollbackCount = 0;
    request.transactionHooks.onRollback = [&rollbackCount]() { ++rollbackCount; };

    const auto result = CompoundStructuralRunner::execute(*testProgram_, request);

    QCOMPARE(result.status, DslExecutionStatus::Cancelled);
    QVERIFY(result.headerNodeId.has_value());
    QCOMPARE(result.payloadNodeId, std::nullopt);
    QCOMPARE(rollbackCount, 1);
    QCOMPARE(tree.node(*result.headerNodeId)->state(), MaterializationState::Cancelled);
}

void CompoundStructuralRunnerTest::payloadPhaseCancellation() {
    const auto data = toBytes({0x67, 0x12, 0x34});
    CancellationSource cancelSource;
    const CancellingSource source(data, 1, cancelSource);
    const auto headerMapping = makeMapping(0, 1, 1);
    const auto payloadMapping = makeMapping(1, 2, 2);

    auto treeOpt = AnalysisTree::create(QStringLiteral("Root"));
    AnalysisTree tree = std::move(*treeOpt);

    const auto headerIndex = testProgram_->structureIndex(QStringLiteral("Header"));
    const auto payloadIndex = testProgram_->structureIndex(QStringLiteral("PayloadA"));

    CompoundStructuralExecutionRequest request;
    request.source = &source;
    request.headerMapping = &headerMapping;
    request.headerStructureIndex = *headerIndex;
    request.payloadStructureIndex = *payloadIndex;
    request.payloadMapping = &payloadMapping;
    request.tree = &tree;
    request.parentId = tree.rootId();
    request.options.cancellation = cancelSource.token();
    request.options.limits.cancellationCheckInterval = 1;
    int rollbackCount = 0;
    request.transactionHooks.onRollback = [&rollbackCount]() { ++rollbackCount; };

    const auto result = CompoundStructuralRunner::execute(*testProgram_, request);

    QCOMPARE(result.status, DslExecutionStatus::Cancelled);
    QVERIFY(result.headerNodeId.has_value());
    QVERIFY(result.payloadNodeId.has_value());
    QCOMPARE(rollbackCount, 1);
    QCOMPARE(tree.node(*result.headerNodeId)->state(), MaterializationState::Cancelled);
    QCOMPARE(tree.node(*result.payloadNodeId)->state(), MaterializationState::Cancelled);
}

void CompoundStructuralRunnerTest::headerOnlyExecutionWithExactConsumptionSuccess() {
    const auto data = toBytes({0x67});
    const MemorySource source(data);
    const auto headerMapping = makeMapping(0, 1, 1);

    auto treeOpt = AnalysisTree::create(QStringLiteral("Root"));
    AnalysisTree tree = std::move(*treeOpt);

    const auto headerIndex = testProgram_->structureIndex(QStringLiteral("Header"));

    int commitCount = 0;
    CompoundStructuralExecutionRequest request;
    request.source = &source;
    request.headerMapping = &headerMapping;
    request.headerStructureIndex = *headerIndex;
    request.payloadStructureIndex = std::nullopt; // No payload
    request.tree = &tree;
    request.parentId = tree.rootId();
    request.requireExactConsumption = true;
    request.transactionHooks.onCommit = [&commitCount]() { ++commitCount; };

    const auto result = CompoundStructuralRunner::execute(*testProgram_, request);

    QVERIFY(result.materialized());
    QCOMPARE(commitCount, 1);
    QCOMPARE(result.headerBitsConsumed, 8ULL);
    QCOMPARE(result.payloadBitsConsumed, 0ULL);

    const auto headerNode = tree.node(*result.headerNodeId);
    QVERIFY(headerNode.has_value());
    QCOMPARE(headerNode->state(), MaterializationState::Materialized);
}

void CompoundStructuralRunnerTest::headerOnlyExecutionFailsOnUnconsumedTrailingBits() {
    // Provide 16 bits in mapping, but Header only consumes 8 bits
    const auto data = toBytes({0x67, 0x00});
    const MemorySource source(data);
    const auto headerMapping = makeMapping(0, 2, 1);

    auto treeOpt = AnalysisTree::create(QStringLiteral("Root"));
    AnalysisTree tree = std::move(*treeOpt);

    const auto headerIndex = testProgram_->structureIndex(QStringLiteral("Header"));

    int rollbackCount = 0;
    CompoundStructuralExecutionRequest request;
    request.source = &source;
    request.headerMapping = &headerMapping;
    request.headerStructureIndex = *headerIndex;
    request.payloadStructureIndex = std::nullopt;
    request.tree = &tree;
    request.parentId = tree.rootId();
    request.requireExactConsumption = true;
    request.transactionHooks.onRollback = [&rollbackCount]() { ++rollbackCount; };

    const auto result = CompoundStructuralRunner::execute(*testProgram_, request);

    QCOMPARE(result.status, DslExecutionStatus::InvalidSyntax);
    QCOMPARE(rollbackCount, 1);

    const auto headerNode = tree.node(*result.headerNodeId);
    QVERIFY(headerNode.has_value());
    QCOMPARE(headerNode->state(), MaterializationState::Invalid);
}

void CompoundStructuralRunnerTest::headerWithPayloadFailsOnUnconsumedHeaderBits() {
    const auto data = toBytes({0x67, 0x00, 0x12, 0x34});
    const MemorySource source(data);
    const auto headerMapping = makeMapping(0, 2, 1);
    const auto payloadMapping = makeMapping(2, 2, 2);

    auto treeOpt = AnalysisTree::create(QStringLiteral("Root"));
    AnalysisTree tree = std::move(*treeOpt);

    int rollbackCount = 0;
    CompoundStructuralExecutionRequest request;
    request.source = &source;
    request.headerMapping = &headerMapping;
    request.headerStructureIndex = *testProgram_->structureIndex(QStringLiteral("Header"));
    request.payloadStructureIndex =
        *testProgram_->structureIndex(QStringLiteral("PayloadA"));
    request.payloadMapping = &payloadMapping;
    request.tree = &tree;
    request.parentId = tree.rootId();
    request.transactionHooks.onRollback = [&rollbackCount]() { ++rollbackCount; };

    const auto result = CompoundStructuralRunner::execute(*testProgram_, request);

    QCOMPARE(result.status, DslExecutionStatus::InvalidSyntax);
    QCOMPARE(result.payloadNodeId, std::nullopt);
    QCOMPARE(rollbackCount, 1);
    QVERIFY(result.headerNodeId.has_value());
    QCOMPARE(tree.node(*result.headerNodeId)->state(), MaterializationState::Invalid);
}

void CompoundStructuralRunnerTest::payloadFailsOnUnconsumedTrailingBits() {
    // Header consumes 8 bits. PayloadA consumes 16 bits. But payload mapping provides 24 bits.
    const auto data = toBytes({0x67, 0x12, 0x34, 0x56});
    const MemorySource source(data);
    const auto headerMapping = makeMapping(0, 1, 1);
    const auto payloadMapping = makeMapping(1, 3, 2);

    auto treeOpt = AnalysisTree::create(QStringLiteral("Root"));
    AnalysisTree tree = std::move(*treeOpt);

    const auto headerIndex = testProgram_->structureIndex(QStringLiteral("Header"));
    const auto payloadIndex = testProgram_->structureIndex(QStringLiteral("PayloadA"));

    int rollbackCount = 0;
    CompoundStructuralExecutionRequest request;
    request.source = &source;
    request.headerMapping = &headerMapping;
    request.headerStructureIndex = *headerIndex;
    request.payloadStructureIndex = *payloadIndex;
    request.payloadMapping = &payloadMapping;
    request.payloadLogicalStart = 0;
    request.tree = &tree;
    request.parentId = tree.rootId();
    request.requireExactConsumption = true;
    request.transactionHooks.onRollback = [&rollbackCount]() { ++rollbackCount; };

    const auto result = CompoundStructuralRunner::execute(*testProgram_, request);

    QCOMPARE(result.status, DslExecutionStatus::InvalidSyntax);
    QCOMPARE(rollbackCount, 1);

    const auto headerNode = tree.node(*result.headerNodeId);
    QVERIFY(headerNode.has_value());
    QCOMPARE(headerNode->state(), MaterializationState::Invalid);
    const auto payloadNode = tree.node(*result.payloadNodeId);
    QVERIFY(payloadNode.has_value());
    QCOMPARE(payloadNode->state(), MaterializationState::Invalid);
    QCOMPARE(payloadNode->diagnostics().size(), std::size_t(1));
    QCOMPARE(payloadNode->diagnostics().front().message,
             QStringLiteral("Payload did not consume all available logical bits"));
}

void CompoundStructuralRunnerTest::executesPayloadFromNonZeroLogicalStart() {
    const auto data = toBytes({0x67, 0xFF, 0x12, 0x34});
    const MemorySource source(data);
    const auto headerMapping = makeMapping(0, 1, 1);
    const auto payloadMapping = makeMapping(1, 3, 2);

    auto treeOpt = AnalysisTree::create(QStringLiteral("Root"));
    AnalysisTree tree = std::move(*treeOpt);

    CompoundStructuralExecutionRequest request;
    request.source = &source;
    request.headerMapping = &headerMapping;
    request.headerStructureIndex = *testProgram_->structureIndex(QStringLiteral("Header"));
    request.payloadStructureIndex =
        *testProgram_->structureIndex(QStringLiteral("PayloadA"));
    request.payloadMapping = &payloadMapping;
    request.payloadLogicalStart = 8;
    request.tree = &tree;
    request.parentId = tree.rootId();

    const auto result = CompoundStructuralRunner::execute(*testProgram_, request);

    QVERIFY(result.materialized());
    QCOMPARE(result.payloadBitsConsumed, 16ULL);
    const auto payloadNode = tree.node(*result.payloadNodeId);
    QVERIFY(payloadNode.has_value());
    QCOMPARE(payloadNode->children().size(), std::size_t(2));
    const auto firstField = tree.node(payloadNode->children().at(0));
    const auto secondField = tree.node(payloadNode->children().at(1));
    QVERIFY(firstField && firstField->location());
    QVERIFY(secondField && secondField->location());
    QCOMPARE(firstField->value().toULongLong(), 0x12ULL);
    QCOMPARE(secondField->value().toULongLong(), 0x34ULL);
    QCOMPARE(firstField->location()->sourceSpans().front().start().absoluteBitOffset(), 16ULL);
    QCOMPARE(secondField->location()->sourceSpans().front().start().absoluteBitOffset(), 24ULL);
}

void CompoundStructuralRunnerTest::commitHookExceptionFailsClosedAndRollsBack() {
    const auto data = toBytes({0x67, 0x12, 0x34});
    const MemorySource source(data);
    const auto headerMapping = makeMapping(0, 1, 1);
    const auto payloadMapping = makeMapping(1, 2, 2);

    auto treeOpt = AnalysisTree::create(QStringLiteral("Root"));
    AnalysisTree tree = std::move(*treeOpt);

    int rollbackCount = 0;
    CompoundStructuralExecutionRequest request;
    request.source = &source;
    request.headerMapping = &headerMapping;
    request.headerStructureIndex = *testProgram_->structureIndex(QStringLiteral("Header"));
    request.payloadStructureIndex =
        *testProgram_->structureIndex(QStringLiteral("PayloadA"));
    request.payloadMapping = &payloadMapping;
    request.tree = &tree;
    request.parentId = tree.rootId();
    request.transactionHooks.onCommit = []() { throw std::runtime_error("commit failed"); };
    request.transactionHooks.onRollback = [&rollbackCount]() { ++rollbackCount; };

    const auto result = CompoundStructuralRunner::execute(*testProgram_, request);

    QCOMPARE(result.status, DslExecutionStatus::InvalidDefinition);
    QVERIFY(result.errorMessage.contains(QStringLiteral("Commit hook failed: commit failed")));
    QCOMPARE(rollbackCount, 1);
    QCOMPARE(tree.node(*result.headerNodeId)->state(), MaterializationState::Invalid);
    QCOMPARE(tree.node(*result.payloadNodeId)->state(), MaterializationState::Invalid);
}

void CompoundStructuralRunnerTest::rollbackHookExceptionFailsClosed() {
    const auto data = toBytes({0x80, 0x12, 0x34});
    const MemorySource source(data);
    const auto headerMapping = makeMapping(0, 1, 1);

    auto treeOpt = AnalysisTree::create(QStringLiteral("Root"));
    AnalysisTree tree = std::move(*treeOpt);

    CompoundStructuralExecutionRequest request;
    request.source = &source;
    request.headerMapping = &headerMapping;
    request.headerStructureIndex = *testProgram_->structureIndex(QStringLiteral("Header"));
    request.tree = &tree;
    request.parentId = tree.rootId();
    request.transactionHooks.onRollback = []() { throw std::runtime_error("rollback failed"); };

    const auto result = CompoundStructuralRunner::execute(*testProgram_, request);

    QCOMPARE(result.status, DslExecutionStatus::InvalidDefinition);
    QVERIFY(result.errorMessage.contains(QStringLiteral("Rollback hook failed: rollback failed")));
    QVERIFY(result.headerNodeId.has_value());
    QCOMPARE(tree.node(*result.headerNodeId)->state(), MaterializationState::Invalid);
}

void CompoundStructuralRunnerTest::repeatingExecutionDoesNotProduceOrphanNodes() {
    // Execute on tree 1 -> success
    const auto data = toBytes({0x67, 0x12, 0x34});
    const MemorySource source(data);
    const auto headerMapping = makeMapping(0, 1, 1);
    const auto payloadMapping = makeMapping(1, 2, 2);

    auto treeOpt1 = AnalysisTree::create(QStringLiteral("Root"));
    AnalysisTree tree1 = std::move(*treeOpt1);

    const auto headerIndex = testProgram_->structureIndex(QStringLiteral("Header"));
    const auto payloadIndex = testProgram_->structureIndex(QStringLiteral("PayloadA"));

    CompoundStructuralExecutionRequest request1;
    request1.source = &source;
    request1.headerMapping = &headerMapping;
    request1.headerStructureIndex = *headerIndex;
    request1.payloadStructureIndex = *payloadIndex;
    request1.payloadMapping = &payloadMapping;
    request1.tree = &tree1;
    request1.parentId = tree1.rootId();

    const auto result1 = CompoundStructuralRunner::execute(*testProgram_, request1);
    QVERIFY(result1.materialized());

    const std::size_t nodesAfterFirst = tree1.nodeCount();

    // Execute again under the same indexing root with invalid data -> fails independently.
    const auto badData = toBytes({0x80, 0x12, 0x34});
    const MemorySource badSource(badData);

    CompoundStructuralExecutionRequest request2;
    request2.source = &badSource;
    request2.headerMapping = &headerMapping;
    request2.headerStructureIndex = *headerIndex;
    request2.payloadStructureIndex = *payloadIndex;
    request2.payloadMapping = &payloadMapping;
    request2.tree = &tree1;
    request2.parentId = tree1.rootId();

    const auto result2 = CompoundStructuralRunner::execute(*testProgram_, request2);
    QCOMPARE(result2.status, DslExecutionStatus::InvalidSyntax);

    QVERIFY(result2.headerNodeId.has_value());
    QCOMPARE(result2.payloadNodeId, std::nullopt);
    QCOMPARE(tree1.nodeCount(), nodesAfterFirst + 2);
    QCOMPARE(tree1.node(*result2.headerNodeId)->parentId(), tree1.rootId());
    QCOMPARE(tree1.node(*result2.headerNodeId)->state(), MaterializationState::Invalid);

    // The previously committed subtree remains unchanged.
    QCOMPARE(tree1.node(*result1.headerNodeId)->state(), MaterializationState::Materialized);
    QCOMPARE(tree1.node(*result1.payloadNodeId)->state(), MaterializationState::Materialized);
}

void CompoundStructuralRunnerTest::rejectsInvalidInputContracts() {
    const auto data = toBytes({0x67, 0x12, 0x34});
    const MemorySource source(data);
    const auto headerMapping = makeMapping(0, 1, 1);
    const auto payloadMapping = makeMapping(1, 2, 2);
    auto treeOpt = AnalysisTree::create(QStringLiteral("Root"));
    AnalysisTree tree = std::move(*treeOpt);

    // Null source
    {
        CompoundStructuralExecutionRequest req;
        req.source = nullptr;
        req.headerMapping = &headerMapping;
        req.tree = &tree;
        req.parentId = tree.rootId();
        const auto res = CompoundStructuralRunner::execute(*testProgram_, req);
        QCOMPARE(res.status, DslExecutionStatus::InvalidDefinition);
    }

    // Null headerMapping
    {
        CompoundStructuralExecutionRequest req;
        req.source = &source;
        req.headerMapping = nullptr;
        req.tree = &tree;
        req.parentId = tree.rootId();
        const auto res = CompoundStructuralRunner::execute(*testProgram_, req);
        QCOMPARE(res.status, DslExecutionStatus::InvalidDefinition);
    }

    // Null tree
    {
        CompoundStructuralExecutionRequest req;
        req.source = &source;
        req.headerMapping = &headerMapping;
        req.tree = nullptr;
        const auto res = CompoundStructuralRunner::execute(*testProgram_, req);
        QCOMPARE(res.status, DslExecutionStatus::InvalidDefinition);
    }

    // Out of range header index
    {
        CompoundStructuralExecutionRequest req;
        req.source = &source;
        req.headerMapping = &headerMapping;
        req.headerStructureIndex = 99999;
        req.tree = &tree;
        req.parentId = tree.rootId();
        const auto res = CompoundStructuralRunner::execute(*testProgram_, req);
        QCOMPARE(res.status, DslExecutionStatus::InvalidDefinition);
    }

    // Unaligned mapping
    {
        const auto span = SourceSpan::create(SourceBitAddress(1), 7);
        const auto unalignedMapping = SourceMapping::create(LogicalViewId(1), {*span});
        CompoundStructuralExecutionRequest req;
        req.source = &source;
        req.headerMapping = &*unalignedMapping;
        req.headerStructureIndex = 0;
        req.tree = &tree;
        req.parentId = tree.rootId();
        const auto res = CompoundStructuralRunner::execute(*testProgram_, req);
        QCOMPARE(res.status, DslExecutionStatus::InvalidDefinition);
    }

    // Empty mapping
    {
        const auto emptyMapping = SourceMapping::create(LogicalViewId(9), {});
        QVERIFY(emptyMapping.has_value());
        CompoundStructuralExecutionRequest req;
        req.source = &source;
        req.headerMapping = &*emptyMapping;
        req.tree = &tree;
        req.parentId = tree.rootId();
        const auto res = CompoundStructuralRunner::execute(*testProgram_, req);
        QCOMPARE(res.status, DslExecutionStatus::InvalidDefinition);
    }

    // Invalid parent ID
    {
        CompoundStructuralExecutionRequest req;
        req.source = &source;
        req.headerMapping = &headerMapping;
        req.tree = &tree;
        req.parentId = AnalysisNodeId(99999);
        const auto res = CompoundStructuralRunner::execute(*testProgram_, req);
        QCOMPARE(res.status, DslExecutionStatus::InvalidDefinition);
    }

    // Parent must remain indexing
    {
        auto terminalTreeOpt = AnalysisTree::create(QStringLiteral("TerminalRoot"));
        AnalysisTree terminalTree = std::move(*terminalTreeOpt);
        QVERIFY(terminalTree.transition(terminalTree.rootId(),
                                        MaterializationState::Materialized));
        CompoundStructuralExecutionRequest req;
        req.source = &source;
        req.headerMapping = &headerMapping;
        req.tree = &terminalTree;
        req.parentId = terminalTree.rootId();
        const auto res = CompoundStructuralRunner::execute(*testProgram_, req);
        QCOMPARE(res.status, DslExecutionStatus::InvalidDefinition);
    }

    // Payload index out of range
    {
        CompoundStructuralExecutionRequest req;
        req.source = &source;
        req.headerMapping = &headerMapping;
        req.payloadStructureIndex = 99999;
        req.payloadMapping = &payloadMapping;
        req.tree = &tree;
        req.parentId = tree.rootId();
        const auto res = CompoundStructuralRunner::execute(*testProgram_, req);
        QCOMPARE(res.status, DslExecutionStatus::InvalidDefinition);
    }

    // Payload structure without mapping
    {
        CompoundStructuralExecutionRequest req;
        req.source = &source;
        req.headerMapping = &headerMapping;
        req.payloadStructureIndex = 0;
        req.tree = &tree;
        req.parentId = tree.rootId();
        const auto res = CompoundStructuralRunner::execute(*testProgram_, req);
        QCOMPARE(res.status, DslExecutionStatus::InvalidDefinition);
    }

    // Mapping without payload structure
    {
        CompoundStructuralExecutionRequest req;
        req.source = &source;
        req.headerMapping = &headerMapping;
        req.payloadMapping = &payloadMapping;
        req.tree = &tree;
        req.parentId = tree.rootId();
        const auto res = CompoundStructuralRunner::execute(*testProgram_, req);
        QCOMPARE(res.status, DslExecutionStatus::InvalidDefinition);
    }

    // Payload start must be in range and byte-aligned
    for (const quint64 invalidStart : {quint64(1), quint64(25)}) {
        CompoundStructuralExecutionRequest req;
        req.source = &source;
        req.headerMapping = &headerMapping;
        req.payloadStructureIndex = 0;
        req.payloadMapping = &payloadMapping;
        req.payloadLogicalStart = invalidStart;
        req.tree = &tree;
        req.parentId = tree.rootId();
        const auto res = CompoundStructuralRunner::execute(*testProgram_, req);
        QCOMPARE(res.status, DslExecutionStatus::InvalidDefinition);
    }
}

void CompoundStructuralRunnerTest::executesCompoundWithCustomTransformProviderAndExcludedSpans() {
    // Header (0x67) + Payload with escape (0x12, 0xEE, 0x34) = 4 bytes
    const auto data = toBytes({0x67, 0x12, 0xEE, 0x34});
    const MemorySource source(data);
    const auto headerMapping = makeMapping(0, 1, 1);
    const auto payloadMapping = makeMapping(1, 3, 2);

    auto treeOpt = AnalysisTree::create(QStringLiteral("Root"));
    AnalysisTree tree = std::move(*treeOpt);

    const auto headerIndex = testProgram_->structureIndex(QStringLiteral("Header"));
    const auto payloadIndex = testProgram_->structureIndex(QStringLiteral("PayloadA"));

    PayloadTransformRegistry registry;
    auto mockProvider = std::make_shared<MockFilteringProvider>();
    QVERIFY(registry.registerProvider(mockProvider));

    bool committed = false;
    CompoundStructuralExecutionRequest request;
    request.source = &source;
    request.headerMapping = &headerMapping;
    request.headerStructureIndex = *headerIndex;
    request.payloadStructureIndex = *payloadIndex;
    request.payloadMapping = &payloadMapping;
    request.transformProviderId = QStringLiteral("mock_filter");
    request.transformRegistry = &registry;
    request.tree = &tree;
    request.parentId = tree.rootId();
    request.transactionHooks.onCommit = [&]() { committed = true; };

    const auto result = CompoundStructuralRunner::execute(*testProgram_, request);

    QVERIFY(result.materialized());
    QVERIFY(committed);
    QVERIFY(result.headerNodeId.has_value());
    QVERIFY(result.payloadNodeId.has_value());
    QCOMPARE(result.inspectedByteCount, 3ULL);
    QCOMPARE(result.excludedSpans.size(), 1ULL);
    QCOMPARE(result.excludedSpans[0].sourceSpan.start().byteOffset(), 2ULL);
    QCOMPARE(result.excludedSpans[0].outputBitOffset, 8ULL);
    QVERIFY(result.forwardedPayloadMapping.has_value());
    QCOMPARE(result.forwardedPayloadMapping->logicalBitLength(), 16ULL);

    const auto headerNode = tree.node(*result.headerNodeId);
    const auto payloadNode = tree.node(*result.payloadNodeId);
    QVERIFY(headerNode.has_value());
    QVERIFY(payloadNode.has_value());
    QCOMPARE(headerNode->state(), MaterializationState::Materialized);
    QCOMPARE(payloadNode->state(), MaterializationState::Materialized);
    QCOMPARE(payloadNode->parentId(), *result.headerNodeId);
}

void CompoundStructuralRunnerTest::compoundFailsClosedOnUnknownTransformProvider() {
    const auto data = toBytes({0x67, 0x12, 0x34});
    const MemorySource source(data);
    const auto headerMapping = makeMapping(0, 1, 1);
    const auto payloadMapping = makeMapping(1, 2, 2);

    auto treeOpt = AnalysisTree::create(QStringLiteral("Root"));
    AnalysisTree tree = std::move(*treeOpt);

    const auto headerIndex = testProgram_->structureIndex(QStringLiteral("Header"));
    const auto payloadIndex = testProgram_->structureIndex(QStringLiteral("PayloadA"));

    bool rolledBack = false;
    CompoundStructuralExecutionRequest request;
    request.source = &source;
    request.headerMapping = &headerMapping;
    request.headerStructureIndex = *headerIndex;
    request.payloadStructureIndex = *payloadIndex;
    request.payloadMapping = &payloadMapping;
    request.transformProviderId = QStringLiteral("non_existent_provider");
    request.tree = &tree;
    request.parentId = tree.rootId();
    request.transactionHooks.onRollback = [&]() { rolledBack = true; };

    const auto result = CompoundStructuralRunner::execute(*testProgram_, request);

    QCOMPARE(result.status, DslExecutionStatus::InvalidDefinition);
    QVERIFY(rolledBack);
    QVERIFY(result.headerNodeId.has_value());
    QCOMPARE(result.payloadNodeId, std::nullopt);
    const auto headerNode = tree.node(*result.headerNodeId);
    QVERIFY(headerNode.has_value());
    QCOMPARE(headerNode->state(), MaterializationState::Invalid);
}

void CompoundStructuralRunnerTest::compoundPropagatesTransformSourceError() {
    const auto data = toBytes({0x67, 0x12, 0x34});
    const FaultySource source(data, 2); // Fails when reading byte 2 (payload)
    const auto headerMapping = makeMapping(0, 1, 1);
    const auto payloadMapping = makeMapping(1, 2, 2);

    auto treeOpt = AnalysisTree::create(QStringLiteral("Root"));
    AnalysisTree tree = std::move(*treeOpt);

    const auto headerIndex = testProgram_->structureIndex(QStringLiteral("Header"));
    const auto payloadIndex = testProgram_->structureIndex(QStringLiteral("PayloadA"));

    PayloadTransformRegistry registry;
    auto mockProvider = std::make_shared<MockFilteringProvider>();
    QVERIFY(registry.registerProvider(mockProvider));

    bool rolledBack = false;
    CompoundStructuralExecutionRequest request;
    request.source = &source;
    request.headerMapping = &headerMapping;
    request.headerStructureIndex = *headerIndex;
    request.payloadStructureIndex = *payloadIndex;
    request.payloadMapping = &payloadMapping;
    request.transformProviderId = QStringLiteral("mock_filter");
    request.transformRegistry = &registry;
    request.tree = &tree;
    request.parentId = tree.rootId();
    request.transactionHooks.onRollback = [&]() { rolledBack = true; };

    const auto result = CompoundStructuralRunner::execute(*testProgram_, request);

    QCOMPARE(result.status, DslExecutionStatus::SourceError);
    QVERIFY(rolledBack);
    QVERIFY(result.headerNodeId.has_value());
    QCOMPARE(result.payloadNodeId, std::nullopt);
    const auto headerNode = tree.node(*result.headerNodeId);
    QVERIFY(headerNode.has_value());
    QCOMPARE(headerNode->state(), MaterializationState::Invalid);
}

void CompoundStructuralRunnerTest::compoundPropagatesTransformTruncation() {
    const auto data = toBytes({0x67, 0x12}); // Only 2 bytes total, but payload claims 3 bytes
    const MemorySource source(data);
    const auto headerMapping = makeMapping(0, 1, 1);
    const auto payloadMapping = makeMapping(1, 3, 2);

    auto treeOpt = AnalysisTree::create(QStringLiteral("Root"));
    AnalysisTree tree = std::move(*treeOpt);

    const auto headerIndex = testProgram_->structureIndex(QStringLiteral("Header"));
    const auto payloadIndex = testProgram_->structureIndex(QStringLiteral("PayloadA"));

    PayloadTransformRegistry registry;
    auto mockProvider = std::make_shared<MockFilteringProvider>();
    QVERIFY(registry.registerProvider(mockProvider));

    bool rolledBack = false;
    CompoundStructuralExecutionRequest request;
    request.source = &source;
    request.headerMapping = &headerMapping;
    request.headerStructureIndex = *headerIndex;
    request.payloadStructureIndex = *payloadIndex;
    request.payloadMapping = &payloadMapping;
    request.transformProviderId = QStringLiteral("mock_filter");
    request.transformRegistry = &registry;
    request.tree = &tree;
    request.parentId = tree.rootId();
    request.transactionHooks.onRollback = [&]() { rolledBack = true; };

    const auto result = CompoundStructuralRunner::execute(*testProgram_, request);

    QCOMPARE(result.status, DslExecutionStatus::TruncatedSource);
    QVERIFY(rolledBack);
    QVERIFY(result.headerNodeId.has_value());
    QCOMPARE(result.payloadNodeId, std::nullopt);
    const auto headerNode = tree.node(*result.headerNodeId);
    QVERIFY(headerNode.has_value());
    QCOMPARE(headerNode->state(), MaterializationState::Invalid);
}

void CompoundStructuralRunnerTest::compoundPropagatesTransformCancellation() {
    const auto data = toBytes({0x67, 0x12, 0x34});
    CancellationSource cancelSource;
    CancellingSource source(data, 1, cancelSource); // Cancels when reading byte 1 during transform
    const auto headerMapping = makeMapping(0, 1, 1);
    const auto payloadMapping = makeMapping(1, 2, 2);

    auto treeOpt = AnalysisTree::create(QStringLiteral("Root"));
    AnalysisTree tree = std::move(*treeOpt);

    const auto headerIndex = testProgram_->structureIndex(QStringLiteral("Header"));
    const auto payloadIndex = testProgram_->structureIndex(QStringLiteral("PayloadA"));

    PayloadTransformRegistry registry;
    auto mockProvider = std::make_shared<MockFilteringProvider>();
    QVERIFY(registry.registerProvider(mockProvider));

    bool rolledBack = false;
    CompoundStructuralExecutionRequest request;
    request.source = &source;
    request.headerMapping = &headerMapping;
    request.headerStructureIndex = *headerIndex;
    request.payloadStructureIndex = *payloadIndex;
    request.payloadMapping = &payloadMapping;
    request.transformProviderId = QStringLiteral("mock_filter");
    request.transformRegistry = &registry;
    request.options.cancellation = cancelSource.token();
    request.tree = &tree;
    request.parentId = tree.rootId();
    request.transactionHooks.onRollback = [&]() { rolledBack = true; };

    const auto result = CompoundStructuralRunner::execute(*testProgram_, request);

    QCOMPARE(result.status, DslExecutionStatus::Cancelled);
    QVERIFY(rolledBack);
    QVERIFY(result.headerNodeId.has_value());
    QCOMPARE(result.payloadNodeId, std::nullopt);
    const auto headerNode = tree.node(*result.headerNodeId);
    QVERIFY(headerNode.has_value());
    QCOMPARE(headerNode->state(), MaterializationState::Cancelled);
}

void CompoundStructuralRunnerTest::compoundPropagatesTransformInspectionBudgetExceeded() {
    const auto data = toBytes({0x67, 0x12, 0x34, 0x56});
    const MemorySource source(data);
    const auto headerMapping = makeMapping(0, 1, 1);
    const auto payloadMapping = makeMapping(1, 3, 2); // 3 bytes payload

    auto treeOpt = AnalysisTree::create(QStringLiteral("Root"));
    AnalysisTree tree = std::move(*treeOpt);

    const auto headerIndex = testProgram_->structureIndex(QStringLiteral("Header"));
    const auto payloadIndex = testProgram_->structureIndex(QStringLiteral("PayloadA"));

    bool rolledBack = false;
    CompoundStructuralExecutionRequest request;
    request.source = &source;
    request.headerMapping = &headerMapping;
    request.headerStructureIndex = *headerIndex;
    request.payloadStructureIndex = *payloadIndex;
    request.payloadMapping = &payloadMapping;
    request.options.limits.maximumInspectedBytes = 2; // Limit 2 < 3
    request.tree = &tree;
    request.parentId = tree.rootId();
    request.transactionHooks.onRollback = [&]() { rolledBack = true; };

    const auto result = CompoundStructuralRunner::execute(*testProgram_, request);

    QCOMPARE(result.status, DslExecutionStatus::ResourceLimit);
    QVERIFY(rolledBack);
    QVERIFY(result.headerNodeId.has_value());
    const auto headerNode = tree.node(*result.headerNodeId);
    QVERIFY(headerNode.has_value());
    QCOMPARE(headerNode->state(), MaterializationState::Invalid);
}

void CompoundStructuralRunnerTest::compoundRejectsZeroLengthPayloadInput() {
    const auto data = toBytes({0x67, 0x12, 0x34});
    const MemorySource source(data);
    const auto headerMapping = makeMapping(0, 1, 1);
    const auto payloadMapping = makeMapping(1, 2, 2);

    auto treeOpt = AnalysisTree::create(QStringLiteral("Root"));
    AnalysisTree tree = std::move(*treeOpt);

    const auto headerIndex = testProgram_->structureIndex(QStringLiteral("Header"));
    const auto payloadIndex = testProgram_->structureIndex(QStringLiteral("PayloadA"));

    bool rolledBack = false;
    CompoundStructuralExecutionRequest request;
    request.source = &source;
    request.headerMapping = &headerMapping;
    request.headerStructureIndex = *headerIndex;
    request.payloadStructureIndex = *payloadIndex;
    request.payloadMapping = &payloadMapping;
    request.payloadLogicalStart = 16; // Equal to payloadMapping->logicalBitLength() -> zero length payload
    request.tree = &tree;
    request.parentId = tree.rootId();
    request.transactionHooks.onRollback = [&]() { rolledBack = true; };

    const auto result = CompoundStructuralRunner::execute(*testProgram_, request);

    QCOMPARE(result.status, DslExecutionStatus::InvalidDefinition);
    QVERIFY(rolledBack);
}

void CompoundStructuralRunnerTest::compoundRejectsMalformedTransformResult() {
    const auto data = toBytes({0x67, 0x12, 0x34});
    const MemorySource source(data);
    const auto headerMapping = makeMapping(0, 1, 1);
    const auto payloadMapping = makeMapping(1, 2, 2);

    auto treeOpt = AnalysisTree::create(QStringLiteral("Root"));
    QVERIFY(treeOpt.has_value());
    AnalysisTree tree = std::move(*treeOpt);

    PayloadTransformRegistry registry;
    QVERIFY(registry.registerProvider(std::make_shared<MalformedTransformProvider>()));

    CompoundStructuralExecutionRequest request;
    request.source = &source;
    request.headerMapping = &headerMapping;
    request.headerStructureIndex = *testProgram_->structureIndex(QStringLiteral("Header"));
    request.payloadStructureIndex = *testProgram_->structureIndex(QStringLiteral("PayloadA"));
    request.payloadMapping = &payloadMapping;
    request.transformProviderId = QStringLiteral("malformed_transform");
    request.transformRegistry = &registry;
    request.tree = &tree;
    request.parentId = tree.rootId();

    const auto result = CompoundStructuralRunner::execute(*testProgram_, request);

    QCOMPARE(result.status, DslExecutionStatus::InvalidDefinition);
    QVERIFY(result.headerNodeId.has_value());
    QCOMPARE(result.payloadNodeId, std::nullopt);
    const auto headerNode = tree.node(*result.headerNodeId);
    QVERIFY(headerNode.has_value());
    QCOMPARE(headerNode->state(), MaterializationState::Invalid);
    QVERIFY(result.errorMessage.contains(QStringLiteral("cover the input logical range")));
}

void CompoundStructuralRunnerTest::compoundPublishesTransformDiagnostics() {
    const auto data = toBytes({0x67, 0x12, 0x34});
    const MemorySource source(data);
    const auto headerMapping = makeMapping(0, 1, 1);
    const auto payloadMapping = makeMapping(1, 2, 2);

    auto treeOpt = AnalysisTree::create(QStringLiteral("Root"));
    QVERIFY(treeOpt.has_value());
    AnalysisTree tree = std::move(*treeOpt);

    PayloadTransformRegistry registry;
    QVERIFY(registry.registerProvider(std::make_shared<DiagnosticTransformProvider>()));

    CompoundStructuralExecutionRequest request;
    request.source = &source;
    request.headerMapping = &headerMapping;
    request.headerStructureIndex = *testProgram_->structureIndex(QStringLiteral("Header"));
    request.payloadStructureIndex = *testProgram_->structureIndex(QStringLiteral("PayloadA"));
    request.payloadMapping = &payloadMapping;
    request.transformProviderId = QStringLiteral("diagnostic_transform");
    request.transformRegistry = &registry;
    request.tree = &tree;
    request.parentId = tree.rootId();

    const auto result = CompoundStructuralRunner::execute(*testProgram_, request);

    QVERIFY(result.materialized());
    QCOMPARE(result.transformDiagnostics.size(), std::size_t(1));
    QCOMPARE(result.transformDiagnostics.front().code, DiagnosticCode::UnsupportedSyntax);
    QVERIFY(result.headerNodeId.has_value());
    const auto headerNode = tree.node(*result.headerNodeId);
    QVERIFY(headerNode.has_value());
    QCOMPARE(headerNode->diagnostics().size(), std::size_t(1));
    QCOMPARE(headerNode->diagnostics().front().message,
             QStringLiteral("Transform provider warning"));
}

void CompoundStructuralRunnerTest::rejectsInspectionBudgetAboveSandboxBound() {
    const auto data = toBytes({0x67});
    const MemorySource source(data);
    const auto headerMapping = makeMapping(0, 1, 1);

    auto treeOpt = AnalysisTree::create(QStringLiteral("Root"));
    QVERIFY(treeOpt.has_value());
    AnalysisTree tree = std::move(*treeOpt);

    CompoundStructuralExecutionRequest request;
    request.source = &source;
    request.headerMapping = &headerMapping;
    request.headerStructureIndex = *testProgram_->structureIndex(QStringLiteral("Header"));
    request.tree = &tree;
    request.parentId = tree.rootId();
    request.options.limits.maximumInspectedBytes =
        DslExecutionLimits::defaultMaximumInspectedBytes() + 1U;

    const auto result = CompoundStructuralRunner::execute(*testProgram_, request);

    QCOMPARE(result.status, DslExecutionStatus::ResourceLimit);
    QCOMPARE(result.headerNodeId, std::nullopt);
    QCOMPARE(tree.nodeCount(), std::size_t(1));
}

QTEST_MAIN(CompoundStructuralRunnerTest)
#include "compound_structural_runner_test.moc"
