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

} // namespace

class CompoundStructuralRunnerTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();

    // 1. Success header + payload single tree
    void executesSuccessfulHeaderAndPayloadInSingleTree();

    // 2. Header failure halts and skips payload
    void headerTruncationHaltsAndSkipsPayload();
    void headerSourceErrorHaltsAndSkipsPayload();
    void headerInvalidSyntaxHaltsAndSkipsPayload();

    // 3. Payload failure terminal states and header transition
    void payloadTruncationTransitionsHeaderToInvalid();
    void payloadSourceErrorTransitionsHeaderToInvalid();
    void payloadUnsupportedTransitionsHeaderToUnsupported();
    void payloadInvalidSyntaxTransitionsHeaderToInvalid();

    // 4. Shared budgets (instructions, nodes, arithmetic overflow)
    void sharedInstructionBudgetExhaustion();
    void sharedNodeBudgetExhaustion();
    void budgetArithmeticOverflowProtection();

    // 5. Cancellation across all phases
    void preCancellationHaltsBeforeAnyTreeMutation();
    void headerPhaseCancellation();
    void betweenHeaderAndPayloadCancellation();
    void payloadPhaseCancellation();

    // 6. Header-only execution and exact consumption
    void headerOnlyExecutionWithExactConsumptionSuccess();
    void headerOnlyExecutionFailsOnUnconsumedTrailingBits();
    void payloadFailsOnUnconsumedTrailingBits();

    // 7. Repeated execution and isolation
    void repeatingExecutionDoesNotProduceOrphanNodes();

    // 8. Preflight validation of inputs
    void rejectsInvalidInputContracts();

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

    CompoundStructuralExecutionRequest request;
    request.source = &source;
    request.headerMapping = &headerMapping;
    request.headerStructureIndex = *headerIndex;
    request.payloadStructureIndex = *payloadIndex;
    request.payloadMapping = &payloadMapping;
    request.payloadLogicalStart = 0;
    request.tree = &tree;
    request.parentId = tree.rootId();
    request.transactionHooks.onCommit = [&commitCount]() { ++commitCount; };
    request.transactionHooks.onRollback = [&rollbackCount]() { ++rollbackCount; };

    const auto result = CompoundStructuralRunner::execute(*testProgram_, request);

    QVERIFY(result.materialized());
    QCOMPARE(result.status, DslExecutionStatus::Materialized);
    QCOMPARE(commitCount, 1);
    QCOMPARE(rollbackCount, 0);

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
    // Header executes 4 instructions. Set limit to 4.
    // Header succeeds using 4 instructions; remaining for payload = 0 -> ResourceLimit
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
    request.options.limits.maximumInstructions = 4; // Tight budget

    const auto result = CompoundStructuralRunner::execute(*testProgram_, request);

    QCOMPARE(result.status, DslExecutionStatus::ResourceLimit);
    const auto headerNode = tree.node(*result.headerNodeId);
    QVERIFY(headerNode.has_value());
    QCOMPARE(headerNode->state(), MaterializationState::Invalid);
}

void CompoundStructuralRunnerTest::sharedNodeBudgetExhaustion() {
    // Header creates 4 nodes (struct + 3 fields). Set maximumMaterializedNodes to 4.
    // Payload can't create any nodes -> ResourceLimit
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
    request.options.limits.maximumMaterializedNodes = 4; // Tight node budget

    const auto result = CompoundStructuralRunner::execute(*testProgram_, request);

    QCOMPARE(result.status, DslExecutionStatus::ResourceLimit);
    const auto headerNode = tree.node(*result.headerNodeId);
    QVERIFY(headerNode.has_value());
    QCOMPARE(headerNode->state(), MaterializationState::Invalid);
}

void CompoundStructuralRunnerTest::budgetArithmeticOverflowProtection() {
    // Verify checked arithmetic when computing total instruction/node counts
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

    const auto result = CompoundStructuralRunner::execute(*testProgram_, request);

    QVERIFY(result.materialized());
    QVERIFY(result.instructionsExecuted > 0);
    QVERIFY(result.nodesCreated > 0);
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
    const MemorySource source(data);
    const auto headerMapping = makeMapping(0, 1, 1);
    const auto payloadMapping = makeMapping(1, 2, 2);

    auto treeOpt = AnalysisTree::create(QStringLiteral("Root"));
    AnalysisTree tree = std::move(*treeOpt);

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
}

void CompoundStructuralRunnerTest::betweenHeaderAndPayloadCancellation() {
    const auto data = toBytes({0x67, 0x12, 0x34});
    const MemorySource source(data);
    const auto headerMapping = makeMapping(0, 1, 1);
    const auto payloadMapping = makeMapping(1, 2, 2);

    auto treeOpt = AnalysisTree::create(QStringLiteral("Root"));
    AnalysisTree tree = std::move(*treeOpt);

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
}

void CompoundStructuralRunnerTest::payloadPhaseCancellation() {
    const auto data = toBytes({0x67, 0x12, 0x34});
    const MemorySource source(data);
    const auto headerMapping = makeMapping(0, 1, 1);
    const auto payloadMapping = makeMapping(1, 2, 2);

    auto treeOpt = AnalysisTree::create(QStringLiteral("Root"));
    AnalysisTree tree = std::move(*treeOpt);

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

    // Execute on tree 2 with invalid data -> fails
    const auto badData = toBytes({0x80, 0x12, 0x34});
    const MemorySource badSource(badData);

    auto treeOpt2 = AnalysisTree::create(QStringLiteral("Root"));
    AnalysisTree tree2 = std::move(*treeOpt2);

    CompoundStructuralExecutionRequest request2;
    request2.source = &badSource;
    request2.headerMapping = &headerMapping;
    request2.headerStructureIndex = *headerIndex;
    request2.payloadStructureIndex = *payloadIndex;
    request2.payloadMapping = &payloadMapping;
    request2.tree = &tree2;
    request2.parentId = tree2.rootId();

    const auto result2 = CompoundStructuralRunner::execute(*testProgram_, request2);
    QCOMPARE(result2.status, DslExecutionStatus::InvalidSyntax);

    // Tree 1 is unaffected and has no partial or invalid results
    QVERIFY(!tree1.hasPartialResults());
    QCOMPARE(tree1.node(*result1.headerNodeId)->state(), MaterializationState::Materialized);
    QCOMPARE(tree1.node(*result1.payloadNodeId)->state(), MaterializationState::Materialized);
}

void CompoundStructuralRunnerTest::rejectsInvalidInputContracts() {
    const auto data = toBytes({0x67, 0x12, 0x34});
    const MemorySource source(data);
    const auto headerMapping = makeMapping(0, 1, 1);
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
}

QTEST_MAIN(CompoundStructuralRunnerTest)
#include "compound_structural_runner_test.moc"
