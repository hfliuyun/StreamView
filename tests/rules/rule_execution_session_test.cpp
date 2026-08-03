#include <streamview/core/coordinates.h>
#include <streamview/core/source.h>
#include <streamview/rules/dsl.h>
#include <streamview/rules/dsl_ir.h>
#include <streamview/rules/rule_execution_session.h>

#include <QTest>

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <span>
#include <utility>
#include <vector>

using streamview::core::AnalysisTree;
using streamview::core::ContextDefinitionKind;
using streamview::core::ContextKey;
using streamview::core::ContextLookupStatus;
using streamview::core::DiagnosticCode;
using streamview::core::RandomAccessSource;
using streamview::core::SourceBitAddress;
using streamview::core::SourceMapping;
using streamview::core::SourceReadResult;
using streamview::core::SourceReadStatus;
using streamview::core::SourceSpan;
using streamview::rules::DslCompiler;
using streamview::rules::DslExecutionStatus;
using streamview::rules::DslParser;
using streamview::rules::RuleExecutionRequest;
using streamview::rules::RuleExecutionSession;
using streamview::rules::RuleExecutionStatus;

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
        ++readCount_;
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

    [[nodiscard]] quint64 readCount() const noexcept { return readCount_; }

private:
    std::vector<std::byte> data_;
    mutable quint64 readCount_ = 0;
};

struct View final {
    SourceSpan span;
    SourceMapping mapping;
};

[[nodiscard]] std::optional<View> makeView(quint64 viewId,
                                           quint64 sourceBitStart,
                                           quint64 bitLength) {
    const auto span = SourceSpan::create(SourceBitAddress(sourceBitStart), bitLength);
    if (!span) {
        return std::nullopt;
    }
    auto mapping = SourceMapping::create(streamview::core::LogicalViewId(viewId), {*span});
    if (!mapping) {
        return std::nullopt;
    }
    return View{*span, std::move(*mapping)};
}

[[nodiscard]] auto compileContextProgram() {
    const auto parsed = DslParser::parse(QStringLiteral(
        "@context(\"h264-sps\", sps_id) "
        "struct Sps { bits<8> sps_id; bits<8> width @context_export; } "
        "@context(\"h264-pps\", pps_id) "
        "@context_dependency(\"h264-sps\", sps_id) "
        "struct Pps { bits<8> pps_id; bits<8> sps_id; "
        "bits<8> entropy_mode @context_export; } entry Sps;"));
    return DslCompiler::compile(parsed.program);
}

[[nodiscard]] RuleExecutionRequest makeRequest(const RandomAccessSource& source,
                                               quint32 structureIndex,
                                               const View& view,
                                               AnalysisTree& tree) {
    RuleExecutionRequest request;
    request.source = &source;
    request.structureIndex = structureIndex;
    request.mapping = &view.mapping;
    request.tree = &tree;
    request.parentId = tree.rootId();
    request.enclosingSourceSpan = view.span;
    return request;
}

} // namespace

class RuleExecutionSessionTest final : public QObject {
    Q_OBJECT

private slots:
    void publishesExactSpsAndPpsGenerations() {
        const auto compiled = compileContextProgram();
        QVERIFY(compiled.succeeded());
        const auto spsIndex = compiled.program->structureIndex(QStringLiteral("Sps"));
        const auto ppsIndex = compiled.program->structureIndex(QStringLiteral("Pps"));
        QVERIFY(spsIndex.has_value());
        QVERIFY(ppsIndex.has_value());

        MemorySource source(bytes({3, 12, 5, 3, 1}));
        const auto spsView = makeView(1, 0, 16);
        const auto ppsView = makeView(2, 16, 24);
        QVERIFY(spsView.has_value());
        QVERIFY(ppsView.has_value());
        auto tree = AnalysisTree::create(QStringLiteral("context-publication"));
        QVERIFY(tree.has_value());
        RuleExecutionSession session(*compiled.program);

        const auto sps = session.run(
            makeRequest(source, *spsIndex, *spsView, *tree));
        QCOMPARE(sps.status, RuleExecutionStatus::Materialized);
        QVERIFY(sps.publishedDefinition.has_value());
        QCOMPARE(sps.publishedDefinition->value(), quint64(1));
        QVERIFY(sps.execution.contextValues.has_value());
        QCOMPARE(sps.execution.contextValues->key.value, quint64(3));
        QCOMPARE(sps.execution.contextValues->exports.size(), std::size_t(1));
        QCOMPARE(sps.execution.contextValues->exports.front().value, quint64(12));

        const auto pps = session.run(
            makeRequest(source, *ppsIndex, *ppsView, *tree));
        QCOMPARE(pps.status, RuleExecutionStatus::Materialized);
        QVERIFY(pps.publishedDefinition.has_value());
        QCOMPARE(pps.publishedDefinition->value(), quint64(2));
        QCOMPARE(session.contextDirectory().definitionCount(), std::size_t(2));
        const auto ppsDefinition =
            session.contextDirectory().definition(*pps.publishedDefinition);
        QVERIFY(ppsDefinition.has_value());
        QCOMPARE(ppsDefinition->dependencies.size(), std::size_t(1));
        QCOMPARE(ppsDefinition->dependencies.front(), *sps.publishedDefinition);
        QCOMPARE(pps.execution.contextValues->dependencies.front().value, quint64(3));
        QCOMPARE(pps.execution.contextValues->dependencies.front()
                     .location->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(24));
    }

    void executesAContextDefinitionFromANonzeroLogicalStart() {
        const auto compiled = compileContextProgram();
        QVERIFY(compiled.succeeded());
        const auto spsIndex = *compiled.program->structureIndex(QStringLiteral("Sps"));
        MemorySource source(bytes({0xff, 3, 12}));
        const auto view = makeView(1, 0, 24);
        QVERIFY(view.has_value());
        auto tree = AnalysisTree::create(QStringLiteral("logical-slice"));
        QVERIFY(tree.has_value());
        RuleExecutionSession session(*compiled.program);
        auto request = makeRequest(source, spsIndex, *view, *tree);
        request.logicalStart = 8;

        const auto result = session.run(request);

        QVERIFY(result.materialized());
        QVERIFY(result.publishedDefinition.has_value());
        QCOMPARE(result.execution.bitsConsumed, quint64(16));
        QCOMPARE(result.execution.contextValues->key.value, quint64(3));
        QCOMPARE(result.execution.contextValues->key.location->sourceSpans().front()
                     .start().absoluteBitOffset(),
                 quint64(8));
        QCOMPARE(result.execution.contextValues->exports.front().value, quint64(12));
    }

    void rejectsReuseAcrossAnalysisSourcesAndTrees() {
        const auto compiled = compileContextProgram();
        QVERIFY(compiled.succeeded());
        const auto spsIndex = *compiled.program->structureIndex(QStringLiteral("Sps"));
        const auto ppsIndex = *compiled.program->structureIndex(QStringLiteral("Pps"));
        MemorySource firstSource(bytes({3, 12, 5, 3, 1}));
        MemorySource secondSource(bytes({3, 12, 5, 3, 1}));
        const auto spsView = makeView(1, 0, 16);
        const auto ppsView = makeView(2, 16, 24);
        QVERIFY(spsView.has_value());
        QVERIFY(ppsView.has_value());
        auto firstTree = AnalysisTree::create(QStringLiteral("first-analysis"));
        auto secondTree = AnalysisTree::create(QStringLiteral("second-analysis"));
        QVERIFY(firstTree.has_value());
        QVERIFY(secondTree.has_value());
        RuleExecutionSession session(*compiled.program);
        QVERIFY(session.run(makeRequest(firstSource, spsIndex, *spsView, *firstTree))
                    .materialized());

        const auto wrongTree =
            session.run(makeRequest(firstSource, ppsIndex, *ppsView, *secondTree));
        const auto wrongSource =
            session.run(makeRequest(secondSource, ppsIndex, *ppsView, *firstTree));

        QCOMPARE(wrongTree.status, RuleExecutionStatus::InvalidDefinition);
        QCOMPARE(wrongSource.status, RuleExecutionStatus::InvalidDefinition);
        QCOMPARE(session.contextDirectory().definitionCount(), std::size_t(1));
        QCOMPARE(secondTree->nodeCount(), std::size_t(1));
        QCOMPARE(secondSource.readCount(), quint64(0));
    }

    void revalidatesDependentGenerationAtConsumerPosition() {
        const auto compiled = compileContextProgram();
        QVERIFY(compiled.succeeded());
        const auto spsIndex = *compiled.program->structureIndex(QStringLiteral("Sps"));
        const auto ppsIndex = *compiled.program->structureIndex(QStringLiteral("Pps"));
        MemorySource source(bytes({3, 12, 5, 3, 1, 3, 13}));
        const auto firstSps = makeView(1, 0, 16);
        const auto pps = makeView(2, 16, 24);
        const auto secondSps = makeView(3, 40, 16);
        QVERIFY(firstSps.has_value());
        QVERIFY(pps.has_value());
        QVERIFY(secondSps.has_value());
        auto tree = AnalysisTree::create(QStringLiteral("context-redefinition"));
        QVERIFY(tree.has_value());
        RuleExecutionSession session(*compiled.program);

        const auto first = session.run(
            makeRequest(source, spsIndex, *firstSps, *tree));
        const auto dependent = session.run(
            makeRequest(source, ppsIndex, *pps, *tree));
        const auto second = session.run(
            makeRequest(source, spsIndex, *secondSps, *tree));
        QVERIFY(first.materialized());
        QVERIFY(dependent.materialized());
        QVERIFY(second.materialized());

        const ContextKey ppsKey{ContextDefinitionKind::H264PictureParameterSet, 0, 5};
        QCOMPARE(session.contextDirectory().resolveBefore(ppsKey, SourceBitAddress(40)).status,
                 ContextLookupStatus::Found);
        QCOMPARE(session.contextDirectory().resolveBefore(ppsKey, SourceBitAddress(56)).status,
                 ContextLookupStatus::DependencyUnavailable);
    }

    void rejectsMissingDependencyWithoutPublishingPps() {
        const auto compiled = compileContextProgram();
        QVERIFY(compiled.succeeded());
        const auto ppsIndex = *compiled.program->structureIndex(QStringLiteral("Pps"));
        MemorySource source(bytes({5, 3, 1}));
        const auto view = makeView(1, 0, 24);
        QVERIFY(view.has_value());
        auto tree = AnalysisTree::create(QStringLiteral("missing-context"));
        QVERIFY(tree.has_value());
        RuleExecutionSession session(*compiled.program);

        const auto result = session.run(
            makeRequest(source, ppsIndex, *view, *tree));

        QCOMPARE(result.status, RuleExecutionStatus::DependencyUnavailable);
        QCOMPARE(session.contextDirectory().definitionCount(), std::size_t(0));
        QVERIFY(result.execution.structureNode.has_value());
        const auto structure = tree->node(*result.execution.structureNode);
        QVERIFY(structure.has_value());
        QCOMPARE(structure->diagnostics().size(), std::size_t(1));
        QCOMPARE(structure->diagnostics().front().code,
                 DiagnosticCode::DependencyUnavailable);
        QCOMPARE(structure->diagnostics().front().fieldPath,
                 QStringLiteral("Pps.sps_id"));
        QCOMPARE(structure->diagnostics().front()
                     .location->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(8));
    }

    void exactConsumptionFailurePublishesNothing() {
        const auto compiled = compileContextProgram();
        QVERIFY(compiled.succeeded());
        const auto spsIndex = *compiled.program->structureIndex(QStringLiteral("Sps"));
        MemorySource source(bytes({3, 12, 0xff}));
        const auto view = makeView(1, 0, 24);
        QVERIFY(view.has_value());
        auto tree = AnalysisTree::create(QStringLiteral("residual-context"));
        QVERIFY(tree.has_value());
        RuleExecutionSession session(*compiled.program);

        const auto result = session.run(
            makeRequest(source, spsIndex, *view, *tree));

        QCOMPARE(result.status, RuleExecutionStatus::InvalidSyntax);
        QVERIFY(result.execution.materialized());
        QVERIFY(!result.publishedDefinition.has_value());
        QCOMPARE(session.contextDirectory().definitionCount(), std::size_t(0));
    }

    void failedRedefinitionDoesNotHideThePreviousGeneration() {
        const auto compiled = compileContextProgram();
        QVERIFY(compiled.succeeded());
        const auto spsIndex = *compiled.program->structureIndex(QStringLiteral("Sps"));
        const auto ppsIndex = *compiled.program->structureIndex(QStringLiteral("Pps"));
        MemorySource source(bytes({3, 12, 3, 13, 0xff, 5, 3, 1}));
        const auto firstSps = makeView(1, 0, 16);
        const auto malformedSps = makeView(2, 16, 24);
        const auto pps = makeView(3, 40, 24);
        QVERIFY(firstSps.has_value());
        QVERIFY(malformedSps.has_value());
        QVERIFY(pps.has_value());
        auto tree = AnalysisTree::create(QStringLiteral("failed-redefinition"));
        QVERIFY(tree.has_value());
        RuleExecutionSession session(*compiled.program);

        const auto first = session.run(makeRequest(source, spsIndex, *firstSps, *tree));
        const auto failed =
            session.run(makeRequest(source, spsIndex, *malformedSps, *tree));
        const auto dependent = session.run(makeRequest(source, ppsIndex, *pps, *tree));

        QVERIFY(first.materialized());
        QCOMPARE(failed.status, RuleExecutionStatus::InvalidSyntax);
        QVERIFY(!failed.publishedDefinition.has_value());
        QVERIFY(dependent.materialized());
        QCOMPARE(session.contextDirectory().definitionCount(), std::size_t(2));
        const auto definition =
            session.contextDirectory().definition(*dependent.publishedDefinition);
        QVERIFY(definition.has_value());
        QCOMPARE(definition->dependencies,
                 std::vector<streamview::core::ContextDefinitionId>(
                     {*first.publishedDefinition}));
    }

    void preservesPublishedGenerationsAcrossSessionMoves() {
        const auto compiled = compileContextProgram();
        QVERIFY(compiled.succeeded());
        const auto spsIndex = *compiled.program->structureIndex(QStringLiteral("Sps"));
        const auto ppsIndex = *compiled.program->structureIndex(QStringLiteral("Pps"));
        MemorySource source(bytes({3, 12, 5, 3, 1}));
        const auto spsView = makeView(1, 0, 16);
        const auto ppsView = makeView(2, 16, 24);
        QVERIFY(spsView.has_value());
        QVERIFY(ppsView.has_value());
        auto tree = AnalysisTree::create(QStringLiteral("moved-session"));
        QVERIFY(tree.has_value());
        RuleExecutionSession original(*compiled.program);
        const auto sps =
            original.run(makeRequest(source, spsIndex, *spsView, *tree));
        QVERIFY(sps.materialized());

        RuleExecutionSession moved(std::move(original));
        RuleExecutionSession assigned(*compiled.program);
        assigned = std::move(moved);
        const auto pps = assigned.run(makeRequest(source, ppsIndex, *ppsView, *tree));

        QVERIFY(pps.materialized());
        QCOMPARE(assigned.contextDirectory().definitionCount(), std::size_t(2));
        const auto definition =
            assigned.contextDirectory().definition(*pps.publishedDefinition);
        QVERIFY(definition.has_value());
        QCOMPARE(definition->dependencies.front(), *sps.publishedDefinition);
    }

    void rejectsMalformedContextIrBeforeReadingSource() {
        auto compiled = compileContextProgram();
        QVERIFY(compiled.succeeded());
        const auto spsIndex = *compiled.program->structureIndex(QStringLiteral("Sps"));
        compiled.program->structs.at(spsIndex).contextDefinition->keyFieldIndex = 99;
        MemorySource source(bytes({3, 12}));
        const auto view = makeView(1, 0, 16);
        QVERIFY(view.has_value());
        auto tree = AnalysisTree::create(QStringLiteral("malformed-context"));
        QVERIFY(tree.has_value());
        RuleExecutionSession session(std::move(*compiled.program));

        const auto result = session.run(
            makeRequest(source, spsIndex, *view, *tree));

        QCOMPARE(result.status, RuleExecutionStatus::InvalidDefinition);
        QCOMPARE(result.execution.status, DslExecutionStatus::InvalidDefinition);
        QCOMPARE(source.readCount(), quint64(0));
        QCOMPARE(session.contextDirectory().definitionCount(), std::size_t(0));
    }

    void rejectsContextMappingOutsideItsEnclosingSpanBeforeReadingSource() {
        const auto compiled = compileContextProgram();
        QVERIFY(compiled.succeeded());
        const auto spsIndex = *compiled.program->structureIndex(QStringLiteral("Sps"));
        MemorySource source(bytes({3, 12}));
        const auto mappingView = makeView(1, 0, 16);
        const auto enclosingView = makeView(2, 8, 8);
        QVERIFY(mappingView.has_value());
        QVERIFY(enclosingView.has_value());
        auto tree = AnalysisTree::create(QStringLiteral("outside-enclosing-span"));
        QVERIFY(tree.has_value());
        RuleExecutionSession session(*compiled.program);
        auto request = makeRequest(source, spsIndex, *mappingView, *tree);
        request.enclosingSourceSpan = enclosingView->span;

        const auto result = session.run(request);

        QCOMPARE(result.status, RuleExecutionStatus::InvalidDefinition);
        QCOMPARE(result.execution.status, DslExecutionStatus::InvalidDefinition);
        QCOMPARE(source.readCount(), quint64(0));
        QCOMPARE(session.contextDirectory().definitionCount(), std::size_t(0));

        MemorySource validSource(bytes({3, 12}));
        auto validTree = AnalysisTree::create(QStringLiteral("valid-after-rejection"));
        QVERIFY(validTree.has_value());
        const auto valid = session.run(
            makeRequest(validSource, spsIndex, *mappingView, *validTree));
        QVERIFY(valid.materialized());
        QCOMPARE(session.contextDirectory().definitionCount(), std::size_t(1));
    }
};

QTEST_APPLESS_MAIN(RuleExecutionSessionTest)

#include "rule_execution_session_test.moc"
