#include <streamview/core/bit_reader.h>
#include <streamview/core/cancellation.h>
#include <streamview/core/coordinates.h>
#include <streamview/core/source.h>
#include <streamview/rules/dsl.h>
#include <streamview/rules/dsl_executor.h>
#include <streamview/rules/dsl_ir.h>

#include <QTest>

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <span>
#include <vector>

using streamview::core::AnalysisNodeKind;
using streamview::core::AnalysisTree;
using streamview::core::BitReader;
using streamview::core::CancellationSource;
using streamview::core::DiagnosticCode;
using streamview::core::MaterializationState;
using streamview::core::RandomAccessSource;
using streamview::core::SourceReadResult;
using streamview::core::SourceReadStatus;
using streamview::core::SourceSpan;
using streamview::rules::DslExecutionStatus;
using streamview::rules::DslExecutionLimits;
using streamview::rules::DslExecutionOptions;
using streamview::rules::DslExecutor;
using streamview::rules::DslCompiler;
using streamview::rules::DslParser;

namespace {

[[nodiscard]] std::vector<std::byte> bytes(std::initializer_list<unsigned int> values) {
    std::vector<std::byte> result;
    result.reserve(values.size());
    for (const unsigned int value : values) {
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

[[nodiscard]] std::optional<streamview::core::SourceMapping>
mappingForBytes(quint64 byteCount) {
    const auto span = SourceSpan::create(streamview::core::SourceBitAddress(0), byteCount * 8U);
    if (!span) {
        return std::nullopt;
    }
    return streamview::core::SourceMapping::create(
        streamview::core::LogicalViewId(1), {*span});
}

} // namespace

class DslExecutorTest final : public QObject {
    Q_OBJECT

private slots:
    void materializesFieldsWithValuesAndLocations() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Header { bits<3> first; bits<5> second; } entry Header;"));
        QVERIFY(parsed.succeeded());

        MemorySource source(bytes({0b10110010}));
        const auto mapping = mappingForBytes(1);
        QVERIFY(mapping.has_value());
        const auto range = SourceSpan::create(streamview::core::SourceBitAddress(0), 8);
        QVERIFY(range.has_value());
        BitReader reader(source, *range);
        auto tree = AnalysisTree::create(QStringLiteral("test"));
        QVERIFY(tree.has_value());

        const auto result = DslExecutor::decodeStruct(
            parsed.program, QStringLiteral("Header"), reader, *mapping, 0, *tree, tree->rootId());
        QCOMPARE(result.status, DslExecutionStatus::Materialized);
        QCOMPARE(result.bitsConsumed, quint64(8));
        QVERIFY(result.structureNode.has_value());
        const auto structure = tree->node(*result.structureNode);
        QVERIFY(structure.has_value());
        QCOMPARE(structure->kind(), AnalysisNodeKind::Structure);
        QCOMPARE(structure->children().size(), std::size_t(2));
        const auto first = tree->node(structure->children().at(0));
        const auto second = tree->node(structure->children().at(1));
        QVERIFY(first.has_value());
        QVERIFY(second.has_value());
        QCOMPARE(first->value().toULongLong(), quint64(5));
        QCOMPARE(second->value().toULongLong(), quint64(18));
        QVERIFY(first->location().has_value());
        QCOMPARE(first->location()->sourceSpans().size(), std::size_t(1));
        QCOMPARE(first->location()->sourceSpans().front().start().absoluteBitOffset(), quint64(0));
        QCOMPARE(first->location()->sourceSpans().front().bitLength(), quint64(3));
        QCOMPARE(structure->state(), streamview::core::MaterializationState::Materialized);
    }

    void carriesPresentationMetadataIntoAnalysisNodes() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "@spec(\"Example Standard\", \"4.2\") "
            "@description(\"A compact header.\") "
            "struct Header { "
            "bits<3> first @description(\"First field.\"); "
            "bits<5> second; "
            "} entry Header;"));
        QVERIFY(parsed.succeeded());

        MemorySource source(bytes({0b10110010}));
        const auto mapping = mappingForBytes(1);
        QVERIFY(mapping.has_value());
        const auto range = SourceSpan::create(streamview::core::SourceBitAddress(0), 8);
        QVERIFY(range.has_value());
        BitReader reader(source, *range);
        auto tree = AnalysisTree::create(QStringLiteral("test"));
        QVERIFY(tree.has_value());

        const auto result = DslExecutor::decodeStruct(
            parsed.program, QStringLiteral("Header"), reader, *mapping, 0, *tree, tree->rootId());
        QVERIFY(result.structureNode.has_value());

        const auto structure = tree->node(*result.structureNode);
        QVERIFY(structure.has_value());
        QCOMPARE(structure->metadata().typeName, QStringLiteral("struct"));
        QCOMPARE(structure->metadata().description, QStringLiteral("A compact header."));
        QVERIFY(structure->metadata().specification.has_value());
        QCOMPARE(structure->metadata().specification->standard,
                 QStringLiteral("Example Standard"));
        QCOMPARE(structure->metadata().specification->clause, QStringLiteral("4.2"));

        const auto first = tree->node(structure->children().at(0));
        const auto second = tree->node(structure->children().at(1));
        QVERIFY(first.has_value());
        QVERIFY(second.has_value());
        QCOMPARE(first->metadata().typeName, QStringLiteral("bits"));
        QCOMPARE(first->metadata().description, QStringLiteral("First field."));
        QVERIFY(first->metadata().specification.has_value());
        QCOMPARE(first->metadata().specification->standard,
                 QStringLiteral("Example Standard"));
        QCOMPARE(first->location()->logicalRange().bitLength(), quint64{3});
        QCOMPARE(second->metadata().typeName, QStringLiteral("bits"));
        QVERIFY(second->metadata().description.isEmpty());
        QVERIFY(second->metadata().specification.has_value());
    }

    void retainsCompleteFieldsWhenTheNextFieldIsTruncated() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Header { bits<3> first; bits<8> second; } entry Header;"));
        QVERIFY(parsed.succeeded());

        MemorySource source(bytes({0b10110010}));
        const auto mapping = mappingForBytes(1);
        QVERIFY(mapping.has_value());
        const auto range = SourceSpan::create(streamview::core::SourceBitAddress(0), 8);
        QVERIFY(range.has_value());
        BitReader reader(source, *range);
        auto tree = AnalysisTree::create(QStringLiteral("test"));
        QVERIFY(tree.has_value());

        const auto result = DslExecutor::decodeStruct(
            parsed.program, QStringLiteral("Header"), reader, *mapping, 0, *tree, tree->rootId());
        QCOMPARE(result.status, DslExecutionStatus::TruncatedSource);
        QVERIFY(result.structureNode.has_value());
        const auto structure = tree->node(*result.structureNode);
        QVERIFY(structure.has_value());
        QCOMPARE(structure->children().size(), std::size_t(1));
        QCOMPARE(structure->diagnostics().size(), std::size_t(1));
        QCOMPARE(structure->diagnostics().front().code,
                 streamview::core::DiagnosticCode::TruncatedSource);
        QVERIFY(structure->diagnostics().front().location.has_value());
        QCOMPARE(structure->diagnostics().front().location->sourceSpans().size(),
                 std::size_t(1));
        QCOMPARE(structure->diagnostics()
                     .front()
                     .location->sourceSpans()
                     .front()
                     .start()
                     .absoluteBitOffset(),
                 quint64(3));
        QCOMPARE(structure->diagnostics().front().location->sourceSpans().front().bitLength(),
                 quint64(5));
        QVERIFY(tree->hasPartialResults());
    }

    void enforcesEqualsConstraintAndKeepsFieldLocation() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Header { bits<1> zero @equals(0); } entry Header;"));
        QVERIFY(parsed.succeeded());

        MemorySource source(bytes({0x80}));
        const auto mapping = mappingForBytes(1);
        QVERIFY(mapping.has_value());
        const auto range = SourceSpan::create(streamview::core::SourceBitAddress(0), 8);
        QVERIFY(range.has_value());
        BitReader reader(source, *range);
        auto tree = AnalysisTree::create(QStringLiteral("test"));
        QVERIFY(tree.has_value());

        const auto result = DslExecutor::decodeStruct(
            parsed.program, QStringLiteral("Header"), reader, *mapping, 0, *tree, tree->rootId());
        QCOMPARE(result.status, DslExecutionStatus::InvalidSyntax);
        const auto structure = tree->node(*result.structureNode);
        QVERIFY(structure.has_value());
        QCOMPARE(structure->children().size(), std::size_t(1));
        QVERIFY(structure->children().front().value() != 0);
        QCOMPARE(structure->diagnostics().front().code,
                 streamview::core::DiagnosticCode::InvalidSyntax);
        QCOMPARE(structure->diagnostics().front().fieldPath, QStringLiteral("Header.zero"));
    }

    void executesTypedIrByResolvedStructureIndex() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Header { bits<3> first; bits<5> second; } entry Header;"));
        QVERIFY(parsed.succeeded());
        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY(compiled.succeeded());

        MemorySource source(bytes({0b10110010}));
        const auto mapping = mappingForBytes(1);
        const auto range = SourceSpan::create(streamview::core::SourceBitAddress(0), 8);
        auto tree = AnalysisTree::create(QStringLiteral("test"));
        QVERIFY(mapping.has_value());
        QVERIFY(range.has_value());
        QVERIFY(tree.has_value());
        BitReader reader(source, *range);

        const auto result = DslExecutor::decodeStruct(
            *compiled.program, quint32(0), reader, *mapping, 0, *tree, tree->rootId());

        QCOMPARE(result.status, DslExecutionStatus::Materialized);
        QCOMPARE(result.instructionsExecuted, quint64(4));
        QCOMPARE(result.nodesCreated, quint64(3));
    }

    void enforcesInstructionBudgetAndRetainsCompletedFields() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Header { bits<3> first; bits<5> second; } entry Header;"));
        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY(compiled.succeeded());

        MemorySource source(bytes({0b10110010}));
        const auto mapping = mappingForBytes(1);
        const auto range = SourceSpan::create(streamview::core::SourceBitAddress(0), 8);
        auto tree = AnalysisTree::create(QStringLiteral("test"));
        QVERIFY(mapping.has_value());
        QVERIFY(range.has_value());
        QVERIFY(tree.has_value());
        BitReader reader(source, *range);
        DslExecutionOptions options;
        options.limits.maximumInstructions = 2;

        const auto result = DslExecutor::decodeStruct(
            *compiled.program, quint32(0), reader, *mapping, 0, *tree, tree->rootId(), options);

        QCOMPARE(result.status, DslExecutionStatus::ResourceLimit);
        QCOMPARE(result.instructionsExecuted, quint64(2));
        QCOMPARE(result.nodesCreated, quint64(2));
        QCOMPARE(result.bitsConsumed, quint64(3));
        const auto structure = tree->node(*result.structureNode);
        QVERIFY(structure.has_value());
        QCOMPARE(structure->state(), MaterializationState::Invalid);
        QCOMPARE(structure->children().size(), std::size_t(1));
        QCOMPARE(structure->diagnostics().front().code, DiagnosticCode::ResourceLimit);
    }

    void enforcesNodeBudgetAndRetainsCompletedFields() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Header { bits<3> first; bits<5> second; } entry Header;"));
        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY(compiled.succeeded());

        MemorySource source(bytes({0b10110010}));
        const auto mapping = mappingForBytes(1);
        const auto range = SourceSpan::create(streamview::core::SourceBitAddress(0), 8);
        auto tree = AnalysisTree::create(QStringLiteral("test"));
        QVERIFY(mapping.has_value());
        QVERIFY(range.has_value());
        QVERIFY(tree.has_value());
        BitReader reader(source, *range);
        DslExecutionOptions options;
        options.limits.maximumMaterializedNodes = 2;

        const auto result = DslExecutor::decodeStruct(
            *compiled.program, quint32(0), reader, *mapping, 0, *tree, tree->rootId(), options);

        QCOMPARE(result.status, DslExecutionStatus::ResourceLimit);
        QCOMPARE(result.nodesCreated, quint64(2));
        QCOMPARE(result.bitsConsumed, quint64(3));
        const auto structure = tree->node(*result.structureNode);
        QVERIFY(structure.has_value());
        QCOMPARE(structure->children().size(), std::size_t(1));
        QCOMPARE(structure->diagnostics().front().code, DiagnosticCode::ResourceLimit);
    }

    void enforcesInclusiveNodeDepthBudget() {
        const auto parsed = DslParser::parse(
            QStringLiteral("struct Header { bits<1> flag; } entry Header;"));
        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY(compiled.succeeded());
        const auto mapping = mappingForBytes(1);
        const auto range = SourceSpan::create(streamview::core::SourceBitAddress(0), 8);
        QVERIFY(mapping.has_value());
        QVERIFY(range.has_value());

        MemorySource successSource(bytes({0x00}));
        BitReader successReader(successSource, *range);
        auto successTree = AnalysisTree::create(QStringLiteral("success"));
        QVERIFY(successTree.has_value());
        DslExecutionOptions successOptions;
        successOptions.limits.maximumNodeDepth = 3;
        const auto success = DslExecutor::decodeStruct(*compiled.program,
                                                       quint32(0),
                                                       successReader,
                                                       *mapping,
                                                       0,
                                                       *successTree,
                                                       successTree->rootId(),
                                                       successOptions);
        QCOMPARE(success.status, DslExecutionStatus::Materialized);

        MemorySource limitedSource(bytes({0x00}));
        BitReader limitedReader(limitedSource, *range);
        auto limitedTree = AnalysisTree::create(QStringLiteral("limited"));
        QVERIFY(limitedTree.has_value());
        DslExecutionOptions limitedOptions;
        limitedOptions.limits.maximumNodeDepth = 2;
        const auto limited = DslExecutor::decodeStruct(*compiled.program,
                                                       quint32(0),
                                                       limitedReader,
                                                       *mapping,
                                                       0,
                                                       *limitedTree,
                                                       limitedTree->rootId(),
                                                       limitedOptions);
        QCOMPARE(limited.status, DslExecutionStatus::ResourceLimit);
        QCOMPARE(limited.nodesCreated, quint64(1));
        QVERIFY(limited.structureNode.has_value());
        const auto structure = limitedTree->node(*limited.structureNode);
        QVERIFY(structure.has_value());
        QCOMPARE(structure->state(), MaterializationState::Invalid);
        QVERIFY(structure->children().empty());
        QCOMPARE(structure->diagnostics().front().code, DiagnosticCode::ResourceLimit);
    }

    void observesPreCancelledTokenBeforeTheFirstInstruction() {
        const auto parsed = DslParser::parse(
            QStringLiteral("struct Header { bits<1> flag; } entry Header;"));
        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY(compiled.succeeded());

        MemorySource source(bytes({0x00}));
        const auto mapping = mappingForBytes(1);
        const auto range = SourceSpan::create(streamview::core::SourceBitAddress(0), 8);
        auto tree = AnalysisTree::create(QStringLiteral("test"));
        QVERIFY(mapping.has_value());
        QVERIFY(range.has_value());
        QVERIFY(tree.has_value());
        BitReader reader(source, *range);
        CancellationSource cancellation;
        QVERIFY(cancellation.requestCancellation());
        DslExecutionOptions options;
        options.cancellation = cancellation.token();

        const auto result = DslExecutor::decodeStruct(
            *compiled.program, quint32(0), reader, *mapping, 0, *tree, tree->rootId(), options);

        QCOMPARE(result.status, DslExecutionStatus::Cancelled);
        QCOMPARE(result.instructionsExecuted, quint64(0));
        QCOMPARE(result.nodesCreated, quint64(0));
        QVERIFY(!result.structureNode.has_value());
        const auto root = tree->node(tree->rootId());
        QVERIFY(root.has_value());
        QCOMPARE(root->state(), MaterializationState::Cancelled);
        QCOMPARE(root->diagnostics().front().code, DiagnosticCode::Cancelled);
    }

    void rejectsMalformedEqualityBytecodeAfterRetainingTheField() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Header { bits<1> flag @equals(0); } entry Header;"));
        auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY(compiled.succeeded());
        QCOMPARE(compiled.program->bytecode.size(), std::size_t(4));
        compiled.program->bytecode.at(2).immediate = 1;

        MemorySource source(bytes({0x00}));
        const auto mapping = mappingForBytes(1);
        const auto range = SourceSpan::create(streamview::core::SourceBitAddress(0), 8);
        auto tree = AnalysisTree::create(QStringLiteral("test"));
        QVERIFY(mapping.has_value());
        QVERIFY(range.has_value());
        QVERIFY(tree.has_value());
        BitReader reader(source, *range);

        const auto result = DslExecutor::decodeStruct(
            *compiled.program, quint32(0), reader, *mapping, 0, *tree, tree->rootId());

        QCOMPARE(result.status, DslExecutionStatus::InvalidDefinition);
        QCOMPARE(result.nodesCreated, quint64(2));
        const auto structure = tree->node(*result.structureNode);
        QVERIFY(structure.has_value());
        QCOMPARE(structure->state(), MaterializationState::Invalid);
        QCOMPARE(structure->children().size(), std::size_t(1));
    }

    void exposesTheDocumentedDefaultLimits() {
        const DslExecutionLimits limits;
        QCOMPARE(limits.maximumInstructions, quint64(1'000'000));
        QCOMPARE(limits.maximumCallDepth, quint32(64));
        QCOMPARE(limits.maximumViewDepth, quint32(64));
        QCOMPARE(limits.maximumNodeDepth, quint32(256));
        QCOMPARE(limits.maximumMaterializedNodes, quint64(100'000));
        QCOMPARE(limits.cancellationCheckInterval, quint64(1'024));
    }

    void rejectsLimitsAboveTheSandboxContract() {
        const auto parsed = DslParser::parse(
            QStringLiteral("struct Header { bits<1> flag; } entry Header;"));
        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY(compiled.succeeded());

        MemorySource source(bytes({0x00}));
        const auto mapping = mappingForBytes(1);
        const auto range = SourceSpan::create(streamview::core::SourceBitAddress(0), 8);
        auto tree = AnalysisTree::create(QStringLiteral("test"));
        QVERIFY(mapping.has_value());
        QVERIFY(range.has_value());
        QVERIFY(tree.has_value());
        BitReader reader(source, *range);
        DslExecutionOptions options;
        options.limits.maximumInstructions =
            DslExecutionLimits::defaultMaximumInstructions() + 1U;

        const auto result = DslExecutor::decodeStruct(
            *compiled.program, quint32(0), reader, *mapping, 0, *tree, tree->rootId(), options);

        QCOMPARE(result.status, DslExecutionStatus::ResourceLimit);
        const auto root = tree->node(tree->rootId());
        QVERIFY(root.has_value());
        QCOMPARE(root->diagnostics().front().code, DiagnosticCode::ResourceLimit);
    }
};

QTEST_GUILESS_MAIN(DslExecutorTest)

#include "dsl_executor_test.moc"
