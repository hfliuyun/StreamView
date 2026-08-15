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
        "bits<8> entropy_mode @context_export; } "
        "@context_import(\"h264-pps\", pps_id) "
        "struct Slice { bits<8> marker; bits<8> pps_id; } entry Sps;"));
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

    void importsExactContextPayloadAndDependencyClosure() {
        const auto compiled = compileContextProgram();
        QVERIFY(compiled.succeeded());
        const auto spsIndex = *compiled.program->structureIndex(QStringLiteral("Sps"));
        const auto ppsIndex = *compiled.program->structureIndex(QStringLiteral("Pps"));
        const auto sliceIndex = *compiled.program->structureIndex(QStringLiteral("Slice"));
        MemorySource source(bytes({3, 12, 5, 3, 1, 0xaa, 5}));
        const auto spsView = makeView(1, 0, 16);
        const auto ppsView = makeView(2, 16, 24);
        const auto sliceView = makeView(3, 40, 16);
        QVERIFY(spsView.has_value());
        QVERIFY(ppsView.has_value());
        QVERIFY(sliceView.has_value());
        auto tree = AnalysisTree::create(QStringLiteral("context-import"));
        QVERIFY(tree.has_value());
        RuleExecutionSession session(*compiled.program);

        const auto sps =
            session.run(makeRequest(source, spsIndex, *spsView, *tree));
        const auto pps =
            session.run(makeRequest(source, ppsIndex, *ppsView, *tree));
        const auto slice =
            session.run(makeRequest(source, sliceIndex, *sliceView, *tree));

        QVERIFY(sps.materialized());
        QVERIFY(pps.materialized());
        QVERIFY(slice.materialized());
        QCOMPARE(slice.execution.contextImports.size(), std::size_t(1));
        QVERIFY(slice.execution.contextImports.front().key.has_value());
        QCOMPARE(slice.execution.contextImports.front().key->value, quint64(5));
        QCOMPARE(slice.execution.contextImports.front().key->location->sourceSpans().front()
                     .start().absoluteBitOffset(),
                 quint64(48));
        QCOMPARE(slice.importedContexts.size(), std::size_t(1));
        const auto& imported = slice.importedContexts.front();
        QCOMPARE(imported.definitionId, *pps.publishedDefinition);
        QCOMPARE(imported.definitions.size(), std::size_t(2));
        QCOMPARE(imported.definitions.at(0).definitionId, *pps.publishedDefinition);
        QCOMPARE(imported.definitions.at(0).kind,
                 ContextDefinitionKind::H264PictureParameterSet);
        QCOMPARE(imported.definitions.at(0).structureIndex, ppsIndex);
        QCOMPARE(imported.definitions.at(0).values, std::vector<quint64>({1}));
        QCOMPARE(imported.definitions.at(0).dependencies,
                 std::vector<streamview::core::ContextDefinitionId>(
                     {*sps.publishedDefinition}));
        QCOMPARE(imported.definitions.at(1).definitionId, *sps.publishedDefinition);
        QCOMPARE(imported.definitions.at(1).kind,
                 ContextDefinitionKind::H264SequenceParameterSet);
        QCOMPARE(imported.definitions.at(1).structureIndex, spsIndex);
        QCOMPARE(imported.definitions.at(1).values, std::vector<quint64>({12}));
        QVERIFY(imported.definitions.at(1).dependencies.empty());
    }

    void evaluatesDynamicBitWidthsFromTheExactImportedGeneration() {
        const auto parsed = DslParser::parse(QStringLiteral(R"(
            @context("h264-sps", id)
            struct Sps { bits<8> id; bits<8> width_minus4 @context_export; }
            @context("h264-pps", id)
            @context_dependency("h264-sps", sps_id)
            struct Pps { bits<8> id; bits<8> sps_id; }
            @context_import("h264-pps", pps_id)
            struct Slice {
                bits<8> pps_id;
                bits<context_value(pps_id, h264_sps, width_minus4) + 4> frame_num;
            }
            entry Slice;
        )"));
        QVERIFY(parsed.succeeded());
        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY2(compiled.succeeded(),
                 compiled.diagnostics.empty()
                     ? ""
                     : qPrintable(compiled.diagnostics.front().message));
        const auto spsIndex = *compiled.program->structureIndex(QStringLiteral("Sps"));
        const auto ppsIndex = *compiled.program->structureIndex(QStringLiteral("Pps"));
        const auto sliceIndex = *compiled.program->structureIndex(QStringLiteral("Slice"));
        MemorySource source(bytes({3, 5, 7, 3, 7, 0x80, 0x80}));
        const auto spsView = makeView(1, 0, 16);
        const auto ppsView = makeView(2, 16, 16);
        const auto sliceView = makeView(3, 32, 17);
        QVERIFY(spsView.has_value());
        QVERIFY(ppsView.has_value());
        QVERIFY(sliceView.has_value());
        auto tree = AnalysisTree::create(QStringLiteral("dynamic-imported-width"));
        QVERIFY(tree.has_value());
        RuleExecutionSession session(*compiled.program);

        QVERIFY(session.run(makeRequest(source, spsIndex, *spsView, *tree))
                    .materialized());
        QVERIFY(session.run(makeRequest(source, ppsIndex, *ppsView, *tree))
                    .materialized());
        const auto slice =
            session.run(makeRequest(source, sliceIndex, *sliceView, *tree));

        QVERIFY(slice.materialized());
        QCOMPARE(slice.execution.bitsConsumed, quint64(17));
        QCOMPARE(slice.importedContexts.size(), std::size_t(1));
        const auto structure = tree->node(*slice.execution.structureNode);
        QVERIFY(structure.has_value());
        QCOMPARE(structure->children().size(), std::size_t(2));
        const auto frameNum = tree->node(structure->children().at(1));
        QVERIFY(frameNum.has_value());
        QCOMPARE(frameNum->value().toULongLong(), qulonglong(257));
        QCOMPARE(frameNum->location()->logicalRange().bitLength(), quint64(9));
    }

    void evaluatesImportedContextEqualityConditionsWithoutReadingSkippedFields() {
        const auto parsed = DslParser::parse(QStringLiteral(R"(
            @context("h264-pps", id)
            struct Pps {
                bits<8> id;
                bits<1> outer_present @context_export;
                bits<1> inner_present @context_export;
                bits<6> reserved;
            }
            @context_import("h264-pps", id)
            struct Slice {
                bits<8> id;
                if (context_value(id, h264_pps, outer_present) == 1) {
                    bits<1> outer_value;
                    if (context_value(id, h264_pps, inner_present) == 1) {
                        bits<1> inner_value;
                    }
                }
                bits<1> tail;
            }
            entry Slice;
        )"));
        QVERIFY(parsed.succeeded());
        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY2(compiled.succeeded(),
                 compiled.diagnostics.empty()
                     ? ""
                     : qPrintable(compiled.diagnostics.front().message));
        const quint32 ppsIndex =
            *compiled.program->structureIndex(QStringLiteral("Pps"));
        const quint32 sliceIndex =
            *compiled.program->structureIndex(QStringLiteral("Slice"));

        struct Scenario final {
            quint8 flags = 0;
            quint8 suffix = 0;
            quint64 sliceBits = 0;
            std::size_t childCount = 0;
            bool hasInner = false;
        };
        const std::vector<Scenario> scenarios{
            {0xc0, 0xe0, 11, 4, true},
            {0x80, 0xc0, 10, 3, false},
            {0x40, 0x80, 9, 2, false},
        };
        for (const Scenario scenario : scenarios) {
            MemorySource source(bytes({1, scenario.flags, 1, scenario.suffix}));
            const auto ppsView = makeView(1, 0, 16);
            const auto sliceView = makeView(2, 16, scenario.sliceBits);
            QVERIFY(ppsView.has_value());
            QVERIFY(sliceView.has_value());
            auto tree = AnalysisTree::create(
                QStringLiteral("imported-condition-%1").arg(scenario.flags));
            QVERIFY(tree.has_value());
            RuleExecutionSession session(*compiled.program);
            QVERIFY(session.run(makeRequest(source, ppsIndex, *ppsView, *tree))
                        .materialized());

            const auto result =
                session.run(makeRequest(source, sliceIndex, *sliceView, *tree));

            QVERIFY(result.materialized());
            QCOMPARE(result.execution.bitsConsumed, scenario.sliceBits);
            QCOMPARE(result.importedContexts.size(), std::size_t(1));
            const auto structure = tree->node(*result.execution.structureNode);
            QVERIFY(structure.has_value());
            QCOMPARE(structure->children().size(), scenario.childCount);
            const bool innerPresent = std::any_of(
                structure->children().begin(),
                structure->children().end(),
                [&tree](streamview::core::AnalysisNodeId childId) {
                    const auto child = tree->node(childId);
                    return child && child->name() == QStringLiteral("inner_value");
                });
            QCOMPARE(innerPresent, scenario.hasInner);
            const auto tail = tree->node(structure->children().back());
            QVERIFY(tail.has_value());
            QCOMPARE(tail->name(), QStringLiteral("tail"));
            QCOMPARE(tail->value().toULongLong(), qulonglong(1));
            QCOMPARE(tail->location()->logicalRange().bitLength(), quint64(1));
        }
    }

    void evaluatesSequenceElementConditionsFromSuppliedElementValues() {
        const auto parsed = DslParser::parse(QStringLiteral(R"(
            struct NalUnitHeader {
                bits<2> nal_ref_idc;
                bits<5> nal_unit_type;
                bits<1> reserved;
            }
            struct Slice {
                bits<4> first_mb_in_slice;
                if (header_value(nal_ref_idc) == 0) {
                } else {
                    bits<1> adaptive_ref_pic_marking_mode_flag;
                }
                bits<1> tail;
            }
            @index(progressive)
            sequence<NalUnitHeader> nal_units = scan(h264_start_code);
            payload<rbsp> nal_units switch (nal_unit_type) {
                case 1: Slice;
            }
            entry nal_units;
        )"));
        QVERIFY(parsed.succeeded());
        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY2(compiled.succeeded(),
                 compiled.diagnostics.empty()
                     ? ""
                     : qPrintable(compiled.diagnostics.front().message));
        const quint32 sliceIndex =
            *compiled.program->structureIndex(QStringLiteral("Slice"));

        struct Scenario final {
            quint64 referencePriority = 0;
            quint64 sliceBits = 0;
            std::size_t childCount = 0;
            bool hasMarking = false;
        };
        const std::vector<Scenario> scenarios{
            {0, 5, 2, false},
            {2, 6, 3, true},
        };
        for (const Scenario scenario : scenarios) {
            MemorySource source(bytes({0xa5}));
            const auto sliceView = makeView(1, 0, scenario.sliceBits);
            QVERIFY(sliceView.has_value());
            auto tree = AnalysisTree::create(
                QStringLiteral("element-%1").arg(scenario.referencePriority));
            QVERIFY(tree.has_value());
            RuleExecutionSession session(*compiled.program);
            auto request = makeRequest(source, sliceIndex, *sliceView, *tree);
            request.options.sequenceElementValues = {scenario.referencePriority, 1, 0};

            const auto result = session.run(request);

            QVERIFY2(result.materialized(), qPrintable(result.errorMessage));
            QCOMPARE(result.execution.bitsConsumed, scenario.sliceBits);
            const auto structure = tree->node(*result.execution.structureNode);
            QVERIFY(structure.has_value());
            QCOMPARE(structure->children().size(), scenario.childCount);
            const bool markingPresent = std::any_of(
                structure->children().begin(),
                structure->children().end(),
                [&tree](streamview::core::AnalysisNodeId childId) {
                    const auto child = tree->node(childId);
                    return child &&
                           child->name() ==
                               QStringLiteral("adaptive_ref_pic_marking_mode_flag");
                });
            QCOMPARE(markingPresent, scenario.hasMarking);
            const auto tail = tree->node(structure->children().back());
            QVERIFY(tail.has_value());
            QCOMPARE(tail->name(), QStringLiteral("tail"));
        }
    }

    void evaluatesComputedFieldsThatReadSequenceElementValues() {
        const auto parsed = DslParser::parse(QStringLiteral(R"(
            struct NalUnitHeader {
                bits<2> nal_ref_idc;
                bits<5> nal_unit_type;
                bits<1> reserved;
            }
            struct Slice {
                bits<4> first_mb_in_slice;
                computed<bool> is_reference_picture = header_value(nal_ref_idc) != 0;
                computed<u64> weight_count = header_value(nal_ref_idc) + 1;
                if (is_reference_picture) {
                    repeat (weight_count, 4) {
                        bits<1> luma_weight_flag;
                    }
                }
                bits<1> tail;
            }
            @index(progressive)
            sequence<NalUnitHeader> nal_units = scan(h264_start_code);
            payload<rbsp> nal_units switch (nal_unit_type) {
                case 1: Slice;
            }
            entry nal_units;
        )"));
        QVERIFY(parsed.succeeded());
        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY2(compiled.succeeded(),
                 compiled.diagnostics.empty()
                     ? ""
                     : qPrintable(compiled.diagnostics.front().message));
        const quint32 sliceIndex =
            *compiled.program->structureIndex(QStringLiteral("Slice"));

        struct Scenario final {
            quint64 referencePriority = 0;
            quint64 sliceBits = 0;
            std::size_t weightFields = 0;
        };
        // nal_ref_idc == 0 skips the repeat entirely; otherwise the computed
        // controller admits exactly nal_ref_idc + 1 iterations.
        const std::vector<Scenario> scenarios{
            {0, 5, 0},
            {1, 7, 2},
            {3, 9, 4},
        };
        for (const Scenario scenario : scenarios) {
            MemorySource source(bytes({0xa5, 0x5a}));
            const auto sliceView = makeView(1, 0, scenario.sliceBits);
            QVERIFY(sliceView.has_value());
            auto tree = AnalysisTree::create(
                QStringLiteral("computed-%1").arg(scenario.referencePriority));
            QVERIFY(tree.has_value());
            RuleExecutionSession session(*compiled.program);
            auto request = makeRequest(source, sliceIndex, *sliceView, *tree);
            request.options.sequenceElementValues = {scenario.referencePriority, 1, 0};

            const auto result = session.run(request);

            QVERIFY2(result.materialized(), qPrintable(result.errorMessage));
            QCOMPARE(result.execution.bitsConsumed, scenario.sliceBits);
            const auto structure = tree->node(*result.execution.structureNode);
            QVERIFY(structure.has_value());
            const auto weightFields = std::count_if(
                structure->children().begin(),
                structure->children().end(),
                [&tree](streamview::core::AnalysisNodeId childId) {
                    const auto child = tree->node(childId);
                    return child &&
                           child->name().startsWith(QStringLiteral("luma_weight_flag"));
                });
            QCOMPARE(std::size_t(weightFields), scenario.weightFields);
            const auto tail = tree->node(structure->children().back());
            QVERIFY(tail.has_value());
            QCOMPARE(tail->name(), QStringLiteral("tail"));
        }
    }

    void selectsOptionalFieldValuesOrTheirDeclaredFallbacks() {
        const auto parsed = DslParser::parse(QStringLiteral(R"(
            @context("h264-pps", pps_id)
            struct Pps {
                bits<8> pps_id;
                bits<8> default_count_minus1 @context_export;
            }
            @context_import("h264-pps", pps_id)
            struct Slice {
                bits<8> pps_id;
                bits<1> override_flag;
                computed<bool> has_override = override_flag != 0;
                if (has_override) {
                    bits<3> local_count_minus1;
                }
                computed<u64> effective_count =
                    optional_value(local_count_minus1,
                                   context_value(pps_id, h264_pps,
                                                 default_count_minus1)) + 1;
                repeat (effective_count, 8) {
                    bits<1> luma_weight_flag;
                }
                bits<1> tail;
            }
            entry Slice;
        )"));
        QVERIFY(parsed.succeeded());
        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY2(compiled.succeeded(),
                 compiled.diagnostics.empty()
                     ? ""
                     : qPrintable(compiled.diagnostics.front().message));
        const quint32 ppsIndex =
            *compiled.program->structureIndex(QStringLiteral("Pps"));
        const quint32 sliceIndex =
            *compiled.program->structureIndex(QStringLiteral("Slice"));

        struct Scenario final {
            quint8 slicePayload = 0;
            quint64 sliceBits = 0;
            quint64 effectiveCount = 0;
            std::size_t weightFields = 0;
        };
        // The PPS exports default_count_minus1 == 4. When override_flag is set the
        // guarded local_count_minus1 == 2 wins, and when it is clear the field was
        // never materialized so the imported default is what the fallback yields.
        const std::vector<Scenario> scenarios{
            {0xab, 16, 3, 3},
            {0x6a, 15, 5, 5},
        };
        for (const Scenario scenario : scenarios) {
            MemorySource source(bytes({3, 4, 3, scenario.slicePayload}));
            const auto ppsView = makeView(1, 0, 16);
            const auto sliceView = makeView(2, 16, scenario.sliceBits);
            QVERIFY(ppsView.has_value());
            QVERIFY(sliceView.has_value());
            auto tree = AnalysisTree::create(
                QStringLiteral("optional-value-%1").arg(scenario.effectiveCount));
            QVERIFY(tree.has_value());
            RuleExecutionSession session(*compiled.program);
            QVERIFY(session.run(makeRequest(source, ppsIndex, *ppsView, *tree))
                        .materialized());

            const auto result =
                session.run(makeRequest(source, sliceIndex, *sliceView, *tree));

            QVERIFY2(result.materialized(), qPrintable(result.errorMessage));
            QCOMPARE(result.execution.bitsConsumed, scenario.sliceBits);
            const auto structure = tree->node(*result.execution.structureNode);
            QVERIFY(structure.has_value());
            std::optional<quint64> effective;
            std::size_t weightFields = 0;
            for (const streamview::core::AnalysisNodeId childId :
                 structure->children()) {
                const auto child = tree->node(childId);
                QVERIFY(child.has_value());
                if (child->name() == QStringLiteral("effective_count")) {
                    effective = child->value().toULongLong();
                } else if (child->name().startsWith(
                               QStringLiteral("luma_weight_flag"))) {
                    ++weightFields;
                }
            }
            QVERIFY(effective.has_value());
            QCOMPARE(*effective, scenario.effectiveCount);
            QCOMPARE(weightFields, scenario.weightFields);
            const auto tail = tree->node(structure->children().back());
            QVERIFY(tail.has_value());
            QCOMPARE(tail->name(), QStringLiteral("tail"));
        }
    }

    void rejectsSequenceElementReferencesWithoutSuppliedElementValues() {
        const auto parsed = DslParser::parse(QStringLiteral(R"(
            struct NalUnitHeader {
                bits<2> nal_ref_idc;
                bits<5> nal_unit_type;
                bits<1> reserved;
            }
            struct Slice {
                bits<4> first_mb_in_slice;
                if (header_value(nal_ref_idc) == 0) {
                } else {
                    bits<1> adaptive_ref_pic_marking_mode_flag;
                }
                bits<1> tail;
            }
            @index(progressive)
            sequence<NalUnitHeader> nal_units = scan(h264_start_code);
            payload<rbsp> nal_units switch (nal_unit_type) {
                case 1: Slice;
            }
            entry nal_units;
        )"));
        QVERIFY(parsed.succeeded());
        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY(compiled.succeeded());
        const quint32 sliceIndex =
            *compiled.program->structureIndex(QStringLiteral("Slice"));

        MemorySource source(bytes({0xa5}));
        const auto sliceView = makeView(1, 0, 6);
        QVERIFY(sliceView.has_value());
        auto tree = AnalysisTree::create(QStringLiteral("element-missing"));
        QVERIFY(tree.has_value());
        RuleExecutionSession session(*compiled.program);

        const auto result =
            session.run(makeRequest(source, sliceIndex, *sliceView, *tree));

        QVERIFY(!result.materialized());
        QCOMPARE(result.status, RuleExecutionStatus::InvalidDefinition);
    }

    void evaluatesImportedContextAssertionsAtTheirDeclaredAnchors() {
        const auto parsed = DslParser::parse(QStringLiteral(R"(
            @context("h264-pps", id)
            struct Pps {
                bits<8> id;
                bits<1> present @context_export;
                bits<7> reserved;
            }
            @context_import("h264-pps", id)
            struct Slice {
                bits<8> id;
                bits<1> marker;
                assert(marker == 0 ||
                       context_value(id, h264_pps, present) == 1)
                    at marker;
                bits<1> tail;
            }
            entry Slice;
        )"));
        QVERIFY2(parsed.succeeded(),
                 parsed.diagnostics.empty()
                     ? ""
                     : qPrintable(parsed.diagnostics.front().message));
        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY2(compiled.succeeded(),
                 compiled.diagnostics.empty()
                     ? ""
                     : qPrintable(compiled.diagnostics.front().message));
        const quint32 ppsIndex =
            *compiled.program->structureIndex(QStringLiteral("Pps"));
        const quint32 sliceIndex =
            *compiled.program->structureIndex(QStringLiteral("Slice"));

        struct Scenario final {
            quint8 ppsFlags = 0;
            RuleExecutionStatus expected = RuleExecutionStatus::Materialized;
            quint64 bitsConsumed = 0;
            std::size_t childCount = 0;
        };
        const std::vector<Scenario> scenarios{
            {0x80, RuleExecutionStatus::Materialized, 10, 3},
            {0x00, RuleExecutionStatus::InvalidSyntax, 9, 2},
        };

        for (const Scenario scenario : scenarios) {
            MemorySource source(bytes({1, scenario.ppsFlags, 1, 0xc0}));
            const auto ppsView = makeView(1, 0, 16);
            const auto sliceView = makeView(2, 16, 10);
            QVERIFY(ppsView.has_value());
            QVERIFY(sliceView.has_value());
            auto tree = AnalysisTree::create(
                QStringLiteral("imported-assertion-%1").arg(scenario.ppsFlags));
            QVERIFY(tree.has_value());
            RuleExecutionSession session(*compiled.program);
            QVERIFY(session.run(makeRequest(source, ppsIndex, *ppsView, *tree))
                        .materialized());

            const auto result =
                session.run(makeRequest(source, sliceIndex, *sliceView, *tree));

            QCOMPARE(result.status, scenario.expected);
            QCOMPARE(result.execution.bitsConsumed, scenario.bitsConsumed);
            const auto structure = tree->node(*result.execution.structureNode);
            QVERIFY(structure.has_value());
            QCOMPARE(structure->children().size(), scenario.childCount);
            if (scenario.expected == RuleExecutionStatus::Materialized) {
                QVERIFY(structure->diagnostics().empty());
                QCOMPARE(result.importedContexts.size(), std::size_t(1));
                const auto tail = tree->node(structure->children().back());
                QVERIFY(tail.has_value());
                QCOMPARE(tail->name(), QStringLiteral("tail"));
                QCOMPARE(tail->value().toULongLong(), qulonglong(1));
                continue;
            }

            QCOMPARE(result.execution.status, DslExecutionStatus::InvalidSyntax);
            QCOMPARE(result.execution.errorMessage,
                     QStringLiteral("Assertion condition is false"));
            QCOMPARE(structure->diagnostics().size(), std::size_t(1));
            const auto& diagnostic = structure->diagnostics().front();
            QCOMPARE(diagnostic.code, DiagnosticCode::InvalidSyntax);
            QCOMPARE(diagnostic.fieldPath, QStringLiteral("Slice.marker"));
            QVERIFY(diagnostic.location.has_value());
            QCOMPARE(diagnostic.location->sourceSpans().front().start()
                         .absoluteBitOffset(),
                     quint64(24));
            QCOMPARE(diagnostic.location->sourceSpans().front().bitLength(),
                     quint64(1));
        }
    }

    void reportsImportedAssertionFailuresAtTheImportKey() {
        const auto parsed = DslParser::parse(QStringLiteral(R"(
            @context("h264-pps", id)
            struct Pps {
                bits<8> id;
                bits<1> present @context_export;
            }
            @context_import("h264-pps", id)
            struct Slice {
                bits<8> id;
                bits<1> marker;
                assert(marker == 0 ||
                       context_value(id, h264_pps, present) == 1)
                    at marker;
                bits<1> tail;
            }
            entry Slice;
        )"));
        QVERIFY(parsed.succeeded());
        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY2(compiled.succeeded(),
                 compiled.diagnostics.empty()
                     ? ""
                     : qPrintable(compiled.diagnostics.front().message));
        const quint32 sliceIndex =
            *compiled.program->structureIndex(QStringLiteral("Slice"));
        MemorySource source(bytes({1, 0xc0}));
        const auto sliceView = makeView(1, 0, 10);
        QVERIFY(sliceView.has_value());
        auto tree = AnalysisTree::create(QStringLiteral("missing-assertion-import"));
        QVERIFY(tree.has_value());
        RuleExecutionSession session(*compiled.program);

        const auto result =
            session.run(makeRequest(source, sliceIndex, *sliceView, *tree));

        QCOMPARE(result.status, RuleExecutionStatus::DependencyUnavailable);
        QCOMPARE(result.execution.status,
                 DslExecutionStatus::DependencyUnavailable);
        QCOMPARE(result.execution.bitsConsumed, quint64(9));
        QVERIFY(result.importedContexts.empty());
        const auto structure = tree->node(*result.execution.structureNode);
        QVERIFY(structure.has_value());
        QCOMPARE(structure->state(),
                 streamview::core::MaterializationState::WaitingDependency);
        QCOMPARE(structure->children().size(), std::size_t(2));
        QCOMPARE(structure->diagnostics().size(), std::size_t(1));
        const auto& diagnostic = structure->diagnostics().front();
        QCOMPARE(diagnostic.code, DiagnosticCode::DependencyUnavailable);
        QCOMPARE(diagnostic.fieldPath, QStringLiteral("Slice.id"));
        QVERIFY(diagnostic.location.has_value());
        QCOMPARE(diagnostic.location->sourceSpans().front().start()
                     .absoluteBitOffset(),
                 quint64(0));
        QCOMPARE(diagnostic.location->sourceSpans().front().bitLength(),
                 quint64(8));
    }

    void rejectsMalformedImportedConditionIrBeforeReadingSource() {
        const auto parsed = DslParser::parse(QStringLiteral(R"(
            @context("h264-pps", id)
            struct Pps { bits<8> id; bits<1> present @context_export; bits<7> reserved; }
            @context_import("h264-pps", id)
            struct Slice {
                bits<8> id;
                if (context_value(id, h264_pps, present) == 1) { bits<1> value; }
                bits<1> tail;
            }
            entry Slice;
        )"));
        QVERIFY(parsed.succeeded());
        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY(compiled.succeeded());
        const quint32 sliceIndex =
            *compiled.program->structureIndex(QStringLiteral("Slice"));
        std::vector<streamview::rules::DslTypedProgram> malformed;

        auto badImport = *compiled.program;
        badImport.structs.at(sliceIndex)
            .fields.at(1)
            .conditions.front()
            .expression->contextImportIndex = 99;
        malformed.push_back(std::move(badImport));

        auto badKind = *compiled.program;
        badKind.structs.at(sliceIndex)
            .fields.at(1)
            .conditions.front()
            .expression->kind = streamview::rules::DslTypedExpressionKind::UnsignedLiteral;
        malformed.push_back(std::move(badKind));

        auto badOperator = *compiled.program;
        badOperator.structs.at(sliceIndex)
            .fields.at(1)
            .conditions.front()
            .op = streamview::rules::DslConditionOperator::GreaterThan;
        malformed.push_back(std::move(badOperator));

        auto badFieldIndex = *compiled.program;
        badFieldIndex.structs.at(sliceIndex).fields.at(1).conditions.front().fieldIndex = 1;
        malformed.push_back(std::move(badFieldIndex));

        auto badType = *compiled.program;
        badType.structs.at(sliceIndex)
            .fields.at(1)
            .conditions.front()
            .expression->type = streamview::rules::DslScalarType::Bool;
        malformed.push_back(std::move(badType));

        auto badOperands = *compiled.program;
        badOperands.structs.at(sliceIndex)
            .fields.at(1)
            .conditions.front()
            .expression->operands.push_back({});
        malformed.push_back(std::move(badOperands));

        auto badStructure = *compiled.program;
        badStructure.structs.at(sliceIndex)
            .fields.at(1)
            .conditions.front()
            .expression->contextStructureIndex = 99;
        malformed.push_back(std::move(badStructure));

        auto badExport = *compiled.program;
        badExport.structs.at(sliceIndex)
            .fields.at(1)
            .conditions.front()
            .expression->contextExportIndex = 99;
        malformed.push_back(std::move(badExport));

        auto malformedPublisher = *compiled.program;
        malformedPublisher.structs.front().contextDefinition->exportFieldIndices.front() = 99;
        malformed.push_back(std::move(malformedPublisher));

        for (std::size_t index = 0; index < malformed.size(); ++index) {
            MemorySource source(bytes({1, 0}));
            const auto view = makeView(index + 1, 0, 16);
            QVERIFY(view.has_value());
            auto tree = AnalysisTree::create(
                QStringLiteral("malformed-imported-condition-%1").arg(index));
            QVERIFY(tree.has_value());
            RuleExecutionSession session(std::move(malformed.at(index)));

            const auto result =
                session.run(makeRequest(source, sliceIndex, *view, *tree));

            QCOMPARE(result.status, RuleExecutionStatus::InvalidDefinition);
            QCOMPARE(source.readCount(), quint64(0));
            QVERIFY(result.importedContexts.empty());
        }
    }

    void reportsImportedConditionFailuresAtTheImportKey() {
        const auto parsed = DslParser::parse(QStringLiteral(R"(
            @context("h264-sps", id)
            struct Sps { bits<8> id; bits<1> present @context_export; bits<7> reserved; }
            @context("h264-pps", id)
            @context_dependency("h264-sps", sps_id)
            struct Pps { bits<8> id; bits<8> sps_id; }
            @context_import("h264-pps", pps_id)
            struct Slice {
                bits<8> pps_id;
                if (context_value(pps_id, h264_sps, present) == 1) { bits<1> value; }
                bits<1> tail;
            }
            entry Slice;
        )"));
        QVERIFY(parsed.succeeded());
        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY2(compiled.succeeded(),
                 compiled.diagnostics.empty()
                     ? ""
                     : qPrintable(compiled.diagnostics.front().message));
        const quint32 spsIndex =
            *compiled.program->structureIndex(QStringLiteral("Sps"));
        const quint32 ppsIndex =
            *compiled.program->structureIndex(QStringLiteral("Pps"));
        const quint32 sliceIndex =
            *compiled.program->structureIndex(QStringLiteral("Slice"));

        {
            MemorySource source(bytes({7, 0x80}));
            const auto sliceView = makeView(1, 0, 9);
            QVERIFY(sliceView.has_value());
            auto tree = AnalysisTree::create(QStringLiteral("missing-condition-import"));
            QVERIFY(tree.has_value());
            RuleExecutionSession session(*compiled.program);

            const auto result =
                session.run(makeRequest(source, sliceIndex, *sliceView, *tree));

            QCOMPARE(result.status, RuleExecutionStatus::DependencyUnavailable);
            QCOMPARE(result.execution.bitsConsumed, quint64(8));
            QVERIFY(result.importedContexts.empty());
            const auto structure = tree->node(*result.execution.structureNode);
            QVERIFY(structure.has_value());
            QCOMPARE(structure->children().size(), std::size_t(1));
            QCOMPARE(structure->diagnostics().front().fieldPath,
                     QStringLiteral("Slice.pps_id"));
            QCOMPARE(structure->diagnostics().front().location->logicalRange().bitLength(),
                     quint64(8));
        }

        {
            MemorySource source(bytes({3, 0x80, 5, 3, 3, 0, 5, 0x80}));
            const auto firstSps = makeView(2, 0, 16);
            const auto pps = makeView(3, 16, 16);
            const auto secondSps = makeView(4, 32, 16);
            const auto slice = makeView(5, 48, 9);
            QVERIFY(firstSps.has_value());
            QVERIFY(pps.has_value());
            QVERIFY(secondSps.has_value());
            QVERIFY(slice.has_value());
            auto tree = AnalysisTree::create(QStringLiteral("stale-condition-import"));
            QVERIFY(tree.has_value());
            RuleExecutionSession session(*compiled.program);
            QVERIFY(session.run(makeRequest(source, spsIndex, *firstSps, *tree))
                        .materialized());
            QVERIFY(session.run(makeRequest(source, ppsIndex, *pps, *tree)).materialized());
            QVERIFY(session.run(makeRequest(source, spsIndex, *secondSps, *tree))
                        .materialized());

            const auto result =
                session.run(makeRequest(source, sliceIndex, *slice, *tree));

            QCOMPARE(result.status, RuleExecutionStatus::DependencyUnavailable);
            QCOMPARE(result.execution.bitsConsumed, quint64(8));
            QVERIFY(result.importedContexts.empty());
            const auto structure = tree->node(*result.execution.structureNode);
            QVERIFY(structure.has_value());
            QCOMPARE(structure->children().size(), std::size_t(1));
            QCOMPARE(structure->diagnostics().front().fieldPath,
                     QStringLiteral("Slice.pps_id"));
            QCOMPARE(structure->diagnostics().front()
                         .location->sourceSpans().front().start().absoluteBitOffset(),
                     quint64(48));
        }
    }

    void enforcesDynamicImportedBitWidthBoundsAndCheckedArithmetic() {
        const auto parsed = DslParser::parse(QStringLiteral(R"(
            @context("h264-sps", id)
            struct Sps { bits<8> id; bits<8> width @context_export; }
            @context_import("h264-sps", id)
            struct Slice {
                bits<8> id;
                bits<context_value(id, h264_sps, width)> value;
            }
            entry Slice;
        )"));
        QVERIFY(parsed.succeeded());
        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY(compiled.succeeded());
        const quint32 spsIndex =
            *compiled.program->structureIndex(QStringLiteral("Sps"));
        const quint32 sliceIndex =
            *compiled.program->structureIndex(QStringLiteral("Slice"));
        struct Scenario final {
            quint8 width = 0;
            quint8 availableValueBits = 0;
            RuleExecutionStatus expected = RuleExecutionStatus::InvalidSyntax;
        };
        const std::vector<Scenario> scenarios{
            {1, 1, RuleExecutionStatus::Materialized},
            {64, 64, RuleExecutionStatus::Materialized},
            {0, 0, RuleExecutionStatus::InvalidSyntax},
            {65, 0, RuleExecutionStatus::InvalidSyntax},
            {9, 5, RuleExecutionStatus::TruncatedSource},
        };
        for (const Scenario scenario : scenarios) {
            std::vector<std::byte> data{
                std::byte{1}, static_cast<std::byte>(scenario.width), std::byte{1}};
            const quint64 valueBits = scenario.availableValueBits;
            data.resize(static_cast<std::size_t>((24U + valueBits + 7U) / 8U),
                        std::byte{0});
            if (scenario.width == 1) {
                data.at(3) = std::byte{0x80};
            }
            MemorySource source(std::move(data));
            const auto spsView = makeView(1, 0, 16);
            const auto sliceView = makeView(2, 16, 8 + valueBits);
            QVERIFY(spsView.has_value());
            QVERIFY(sliceView.has_value());
            auto tree = AnalysisTree::create(
                QStringLiteral("dynamic-width-%1").arg(scenario.width));
            QVERIFY(tree.has_value());
            RuleExecutionSession session(*compiled.program);
            QVERIFY(session.run(makeRequest(source, spsIndex, *spsView, *tree))
                        .materialized());

            const auto result =
                session.run(makeRequest(source, sliceIndex, *sliceView, *tree));

            QCOMPARE(result.status, scenario.expected);
            if (scenario.expected == RuleExecutionStatus::Materialized) {
                QCOMPARE(result.execution.bitsConsumed, quint64(8 + scenario.width));
                const auto structure = tree->node(*result.execution.structureNode);
                QVERIFY(structure.has_value());
                const auto value = tree->node(structure->children().at(1));
                QVERIFY(value.has_value());
                QCOMPARE(value->location()->logicalRange().bitLength(),
                         quint64(scenario.width));
            } else {
                QCOMPARE(result.execution.bitsConsumed, quint64(8));
                QVERIFY(result.importedContexts.empty());
                if (scenario.expected == RuleExecutionStatus::TruncatedSource) {
                    const auto structure = tree->node(*result.execution.structureNode);
                    QVERIFY(structure.has_value());
                    QCOMPARE(structure->children().size(), std::size_t(1));
                    QCOMPARE(structure->diagnostics().front()
                                 .location->logicalRange().bitLength(),
                             quint64(scenario.availableValueBits));
                }
            }
        }

        const auto overflowParsed = DslParser::parse(QStringLiteral(R"(
            @context("h264-sps", id)
            struct Sps {
                bits<8> id;
                computed<u64> width = 18446744073709551615 @context_export;
            }
            @context_import("h264-sps", id)
            struct Slice {
                bits<8> id;
                bits<context_value(id, h264_sps, width) + 1> value;
            }
            entry Slice;
        )"));
        QVERIFY(overflowParsed.succeeded());
        const auto overflowCompiled = DslCompiler::compile(overflowParsed.program);
        QVERIFY(overflowCompiled.succeeded());
        MemorySource overflowSource(bytes({1, 1}));
        const auto overflowSpsView = makeView(3, 0, 8);
        const auto overflowSliceView = makeView(4, 8, 8);
        QVERIFY(overflowSpsView.has_value());
        QVERIFY(overflowSliceView.has_value());
        auto overflowTree = AnalysisTree::create(QStringLiteral("dynamic-width-overflow"));
        QVERIFY(overflowTree.has_value());
        RuleExecutionSession overflowSession(*overflowCompiled.program);
        QVERIFY(overflowSession
                    .run(makeRequest(overflowSource,
                                     *overflowCompiled.program->structureIndex(
                                         QStringLiteral("Sps")),
                                     *overflowSpsView,
                                     *overflowTree))
                    .materialized());
        const auto overflow = overflowSession.run(makeRequest(
            overflowSource,
            *overflowCompiled.program->structureIndex(QStringLiteral("Slice")),
            *overflowSliceView,
            *overflowTree));
        QCOMPARE(overflow.status, RuleExecutionStatus::InvalidSyntax);
        QCOMPARE(overflow.execution.bitsConsumed, quint64(8));
        QVERIFY(overflow.importedContexts.empty());
    }

    void reportsDynamicImportFailuresAtTheImportKey() {
        const auto parsed = DslParser::parse(QStringLiteral(R"(
            @context("h264-sps", id)
            struct Sps { bits<8> id; bits<8> width @context_export; }
            @context_import("h264-sps", id)
            struct Slice {
                bits<8> id;
                bits<context_value(id, h264_sps, width)> value;
            }
            entry Slice;
        )"));
        QVERIFY(parsed.succeeded());
        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY(compiled.succeeded());
        const quint32 sliceIndex =
            *compiled.program->structureIndex(QStringLiteral("Slice"));
        MemorySource source(bytes({7}));
        const auto view = makeView(1, 0, 8);
        QVERIFY(view.has_value());
        auto tree = AnalysisTree::create(QStringLiteral("missing-dynamic-import"));
        QVERIFY(tree.has_value());
        RuleExecutionSession session(*compiled.program);

        const auto result =
            session.run(makeRequest(source, sliceIndex, *view, *tree));

        QCOMPARE(result.status, RuleExecutionStatus::DependencyUnavailable);
        QCOMPARE(result.execution.bitsConsumed, quint64(8));
        QVERIFY(result.importedContexts.empty());
        const auto structure = tree->node(*result.execution.structureNode);
        QVERIFY(structure.has_value());
        QCOMPARE(structure->state(),
                 streamview::core::MaterializationState::WaitingDependency);
        QCOMPARE(structure->children().size(), std::size_t(1));
        QCOMPARE(structure->diagnostics().size(), std::size_t(1));
        QCOMPARE(structure->diagnostics().front().code,
                 DiagnosticCode::DependencyUnavailable);
        QCOMPARE(structure->diagnostics().front().fieldPath,
                 QStringLiteral("Slice.id"));
        QCOMPARE(structure->diagnostics().front().location->logicalRange().bitLength(),
                 quint64(8));
    }

    void mapsDynamicImportedFieldsAcrossSourceSpans() {
        const auto parsed = DslParser::parse(QStringLiteral(R"(
            @context("h264-sps", id)
            struct Sps { bits<8> id; bits<8> width_minus4 @context_export; }
            @context_import("h264-sps", id)
            struct Slice {
                bits<8> id;
                bits<context_value(id, h264_sps, width_minus4) + 4> frame_num;
            }
            entry Slice;
        )"));
        QVERIFY(parsed.succeeded());
        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY(compiled.succeeded());
        MemorySource source(bytes({3, 5, 3, 0x80, 0, 0x08}));
        const auto spsView = makeView(1, 0, 16);
        const auto firstSliceSpan = SourceSpan::create(SourceBitAddress(16), 12);
        const auto secondSliceSpan = SourceSpan::create(SourceBitAddress(40), 5);
        const auto enclosing = SourceSpan::create(SourceBitAddress(16), 29);
        QVERIFY(spsView.has_value());
        QVERIFY(firstSliceSpan.has_value());
        QVERIFY(secondSliceSpan.has_value());
        QVERIFY(enclosing.has_value());
        auto mapping = SourceMapping::create(
            streamview::core::LogicalViewId(2),
            {*firstSliceSpan, *secondSliceSpan});
        QVERIFY(mapping.has_value());
        const View sliceView{*enclosing, std::move(*mapping)};
        auto tree = AnalysisTree::create(QStringLiteral("mapped-dynamic-width"));
        QVERIFY(tree.has_value());
        RuleExecutionSession session(*compiled.program);
        QVERIFY(session
                    .run(makeRequest(source,
                                     *compiled.program->structureIndex(
                                         QStringLiteral("Sps")),
                                     *spsView,
                                     *tree))
                    .materialized());

        const auto slice = session.run(makeRequest(
            source,
            *compiled.program->structureIndex(QStringLiteral("Slice")),
            sliceView,
            *tree));

        QVERIFY(slice.materialized());
        const auto structure = tree->node(*slice.execution.structureNode);
        QVERIFY(structure.has_value());
        const auto frameNum = tree->node(structure->children().at(1));
        QVERIFY(frameNum.has_value());
        QCOMPARE(frameNum->value().toULongLong(), qulonglong(257));
        QCOMPARE(frameNum->location()->sourceSpans().size(), std::size_t(2));
        QCOMPARE(frameNum->location()->logicalRange().bitLength(), quint64(9));
    }

    void rejectsMalformedDynamicImportedWidthIrBeforeReadingSource() {
        const auto parsed = DslParser::parse(QStringLiteral(R"(
            @context("h264-sps", id)
            struct Sps { bits<8> id; bits<8> width @context_export; }
            @context("aac-asc", id)
            struct Asc { bits<8> id; bits<8> width @context_export; }
            @context_import("h264-sps", id)
            struct Slice {
                bits<8> id;
                bits<context_value(id, h264_sps, width)> value;
            }
            entry Slice;
        )"));
        QVERIFY(parsed.succeeded());
        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY(compiled.succeeded());
        const quint32 sliceIndex =
            *compiled.program->structureIndex(QStringLiteral("Slice"));
        const quint32 ascIndex =
            *compiled.program->structureIndex(QStringLiteral("Asc"));
        std::vector<streamview::rules::DslTypedProgram> malformed;

        auto badPublisherExport = *compiled.program;
        badPublisherExport.structs.front()
            .contextDefinition->exportFieldIndices.front() = 99;
        malformed.push_back(std::move(badPublisherExport));

        auto badPublisherKey = *compiled.program;
        badPublisherKey.structs.front().contextDefinition->keyFieldIndex = 99;
        malformed.push_back(std::move(badPublisherKey));

        auto badPublisherDependency = *compiled.program;
        badPublisherDependency.structs.front().contextDefinition->dependencies.push_back(
            {ContextDefinitionKind::H264PictureParameterSet, 99});
        malformed.push_back(std::move(badPublisherDependency));

        auto duplicatePublisherExport = *compiled.program;
        duplicatePublisherExport.structs.front()
            .contextDefinition->exportFieldIndices.push_back(1);
        malformed.push_back(std::move(duplicatePublisherExport));

        auto badPublisherKind = *compiled.program;
        const auto invalidKind = static_cast<ContextDefinitionKind>(0xff);
        badPublisherKind.structs.front().contextDefinition->kind = invalidKind;
        badPublisherKind.structs.at(sliceIndex)
            .fields.at(1)
            .bitWidthExpression->contextDefinitionKind = invalidKind;
        malformed.push_back(std::move(badPublisherKind));

        auto badImportIndex = *compiled.program;
        badImportIndex.structs.at(sliceIndex)
            .fields.at(1)
            .bitWidthExpression->contextImportIndex = 99;
        malformed.push_back(std::move(badImportIndex));

        auto badStructureIndex = *compiled.program;
        badStructureIndex.structs.at(sliceIndex)
            .fields.at(1)
            .bitWidthExpression->contextStructureIndex = 99;
        malformed.push_back(std::move(badStructureIndex));

        auto unrelatedTarget = *compiled.program;
        unrelatedTarget.structs.at(sliceIndex)
            .fields.at(1)
            .bitWidthExpression->contextDefinitionKind =
            ContextDefinitionKind::AacAudioSpecificConfig;
        unrelatedTarget.structs.at(sliceIndex)
            .fields.at(1)
            .bitWidthExpression->contextStructureIndex = ascIndex;
        malformed.push_back(std::move(unrelatedTarget));

        auto badDynamicEndian = *compiled.program;
        badDynamicEndian.structs.at(sliceIndex).fields.at(1).type.endian =
            streamview::rules::DslEndian::Little;
        malformed.push_back(std::move(badDynamicEndian));

        for (std::size_t index = 0; index < malformed.size(); ++index) {
            MemorySource source(bytes({1, 0}));
            const auto view = makeView(index + 1, 0, 16);
            QVERIFY(view.has_value());
            auto tree = AnalysisTree::create(
                QStringLiteral("malformed-dynamic-import-%1").arg(index));
            QVERIFY(tree.has_value());
            RuleExecutionSession session(std::move(malformed.at(index)));

            const auto result =
                session.run(makeRequest(source, sliceIndex, *view, *tree));

            QCOMPARE(result.status, RuleExecutionStatus::InvalidDefinition);
            QCOMPARE(source.readCount(), quint64(0));
            QVERIFY(result.importedContexts.empty());
        }
    }

    void rejectsMissingContextImportWithoutReturningAPartialClosure() {
        const auto compiled = compileContextProgram();
        QVERIFY(compiled.succeeded());
        const auto sliceIndex = *compiled.program->structureIndex(QStringLiteral("Slice"));
        MemorySource source(bytes({0xaa, 5}));
        const auto view = makeView(1, 0, 16);
        QVERIFY(view.has_value());
        auto tree = AnalysisTree::create(QStringLiteral("missing-import"));
        QVERIFY(tree.has_value());
        RuleExecutionSession session(*compiled.program);

        const auto result =
            session.run(makeRequest(source, sliceIndex, *view, *tree));

        QCOMPARE(result.status, RuleExecutionStatus::DependencyUnavailable);
        QVERIFY(result.importedContexts.empty());
        QVERIFY(result.execution.structureNode.has_value());
        const auto structure = tree->node(*result.execution.structureNode);
        QVERIFY(structure.has_value());
        QCOMPARE(structure->diagnostics().size(), std::size_t(1));
        QCOMPARE(structure->diagnostics().front().code,
                 DiagnosticCode::DependencyUnavailable);
        QCOMPARE(structure->diagnostics().front().fieldPath,
                 QStringLiteral("Slice.pps_id"));
        QCOMPARE(structure->diagnostics().front().location->sourceSpans().front()
                     .start().absoluteBitOffset(),
                 quint64(8));
    }

    void importFailureDoesNotReturnEarlierClosuresOrPublishAContext() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "@context(\"h264-sps\", id) struct Sps { bits<8> id; } "
            "@context(\"aac-asc\", publish_id) "
            "@context_import(\"h264-sps\", sps_id) "
            "@context_import(\"h264-pps\", pps_id) "
            "struct Combined { bits<8> publish_id; bits<8> sps_id; bits<8> pps_id; } "
            "entry Combined;"));
        QVERIFY(parsed.succeeded());
        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY(compiled.succeeded());
        const auto spsIndex = *compiled.program->structureIndex(QStringLiteral("Sps"));
        const auto combinedIndex =
            *compiled.program->structureIndex(QStringLiteral("Combined"));
        MemorySource source(bytes({3, 9, 3, 5}));
        const auto spsView = makeView(1, 0, 8);
        const auto combinedView = makeView(2, 8, 24);
        QVERIFY(spsView.has_value());
        QVERIFY(combinedView.has_value());
        auto tree = AnalysisTree::create(QStringLiteral("transactional-import"));
        QVERIFY(tree.has_value());
        RuleExecutionSession session(*compiled.program);

        const auto sps =
            session.run(makeRequest(source, spsIndex, *spsView, *tree));
        const auto combined =
            session.run(makeRequest(source, combinedIndex, *combinedView, *tree));

        QVERIFY(sps.materialized());
        QCOMPARE(combined.status, RuleExecutionStatus::DependencyUnavailable);
        QCOMPARE(combined.execution.contextImports.size(), std::size_t(2));
        QVERIFY(combined.importedContexts.empty());
        QVERIFY(!combined.publishedDefinition.has_value());
        QCOMPARE(session.contextDirectory().definitionCount(), std::size_t(1));
        const ContextKey unpublishedKey{
            ContextDefinitionKind::AacAudioSpecificConfig, 0, 9};
        QCOMPARE(session.contextDirectory()
                     .resolveBefore(unpublishedKey, SourceBitAddress(32))
                     .status,
                 ContextLookupStatus::NotFound);
        const auto structure = tree->node(*combined.execution.structureNode);
        QVERIFY(structure.has_value());
        QCOMPARE(structure->diagnostics().size(), std::size_t(1));
        QCOMPARE(structure->diagnostics().front().fieldPath,
                 QStringLiteral("Combined.pps_id"));
        QCOMPARE(structure->diagnostics().front().location->sourceSpans().front()
                     .start().absoluteBitOffset(),
                 quint64(24));
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
        const auto sliceIndex = *compiled.program->structureIndex(QStringLiteral("Slice"));
        MemorySource source(bytes({3, 12, 5, 3, 1, 3, 13, 0xaa, 5}));
        const auto firstSps = makeView(1, 0, 16);
        const auto pps = makeView(2, 16, 24);
        const auto secondSps = makeView(3, 40, 16);
        const auto slice = makeView(4, 56, 16);
        QVERIFY(firstSps.has_value());
        QVERIFY(pps.has_value());
        QVERIFY(secondSps.has_value());
        QVERIFY(slice.has_value());
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

        const auto staleImport = session.run(
            makeRequest(source, sliceIndex, *slice, *tree));

        const ContextKey ppsKey{ContextDefinitionKind::H264PictureParameterSet, 0, 5};
        QCOMPARE(session.contextDirectory().resolveBefore(ppsKey, SourceBitAddress(40)).status,
                 ContextLookupStatus::Found);
        QCOMPARE(session.contextDirectory().resolveBefore(ppsKey, SourceBitAddress(56)).status,
                 ContextLookupStatus::DependencyUnavailable);
        QCOMPARE(staleImport.status, RuleExecutionStatus::DependencyUnavailable);
        QVERIFY(staleImport.importedContexts.empty());
    }

    void doesNotImportAGenerationPublishedLaterInSourceOrder() {
        const auto compiled = compileContextProgram();
        QVERIFY(compiled.succeeded());
        const auto spsIndex = *compiled.program->structureIndex(QStringLiteral("Sps"));
        const auto sliceIndex = *compiled.program->structureIndex(QStringLiteral("Slice"));
        MemorySource source(bytes({0xaa, 3, 3, 12}));
        const auto consumerView = makeView(1, 0, 16);
        const auto futureSpsView = makeView(2, 16, 16);
        QVERIFY(consumerView.has_value());
        QVERIFY(futureSpsView.has_value());
        auto tree = AnalysisTree::create(QStringLiteral("future-import"));
        QVERIFY(tree.has_value());
        RuleExecutionSession session(*compiled.program);

        const auto future = session.run(
            makeRequest(source, spsIndex, *futureSpsView, *tree));
        const auto consumer = session.run(
            makeRequest(source, sliceIndex, *consumerView, *tree));

        QVERIFY(future.materialized());
        QCOMPARE(consumer.status, RuleExecutionStatus::DependencyUnavailable);
        QVERIFY(consumer.importedContexts.empty());
        const auto structure = tree->node(*consumer.execution.structureNode);
        QVERIFY(structure.has_value());
        QCOMPARE(structure->diagnostics().front().fieldPath,
                 QStringLiteral("Slice.pps_id"));
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

    void rejectsMalformedContextImportIrBeforeReadingSource_data() {
        QTest::addColumn<int>("mutation");

        QTest::newRow("out-of-range-key-index") << 0;
        QTest::newRow("invalid-kind") << 1;
        QTest::newRow("duplicate-import") << 2;
        QTest::newRow("seventeen-imports") << 3;
        QTest::newRow("fixed-array-element-key") << 4;
        QTest::newRow("duplicate-ambient-import") << 5;
    }

    void rejectsMalformedContextImportIrBeforeReadingSource() {
        QFETCH(int, mutation);
        const auto parsed = DslParser::parse(QStringLiteral(
            "@context_import(\"h264-pps\", key) "
            "struct Consumer { bits<8> key; bits<8> values[2]; } entry Consumer;"));
        QVERIFY(parsed.succeeded());
        auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY(compiled.succeeded());
        const auto consumerIndex =
            *compiled.program->structureIndex(QStringLiteral("Consumer"));
        auto& imports = compiled.program->structs.at(consumerIndex).contextImports;
        switch (mutation) {
        case 0:
            imports.front().keyFieldIndex = 99;
            break;
        case 1:
            imports.front().kind = static_cast<ContextDefinitionKind>(0xff);
            break;
        case 2:
            imports.push_back(imports.front());
            break;
        case 3:
            while (imports.size() <=
                   streamview::rules::DslTypedContextImport::maximumImports()) {
                imports.push_back(imports.front());
            }
            break;
        case 4:
            imports.front().keyFieldIndex = 1;
            break;
        case 5:
            imports.push_back({ContextDefinitionKind::H264SequenceParameterSet, std::nullopt});
            imports.push_back({ContextDefinitionKind::H264SequenceParameterSet, std::nullopt});
            break;
        default:
            QFAIL("Unknown malformed context-import mutation");
        }
        MemorySource source(bytes({5, 6, 7}));
        const auto view = makeView(1, 0, 24);
        QVERIFY(view.has_value());
        auto tree = AnalysisTree::create(QStringLiteral("malformed-import"));
        QVERIFY(tree.has_value());
        RuleExecutionSession session(std::move(*compiled.program));

        const auto result =
            session.run(makeRequest(source, consumerIndex, *view, *tree));

        QCOMPARE(result.status, RuleExecutionStatus::InvalidDefinition);
        QCOMPARE(result.execution.status, DslExecutionStatus::InvalidDefinition);
        QCOMPARE(source.readCount(), quint64(0));
        QVERIFY(result.importedContexts.empty());
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

    void boundsImportedContextDependencyClosures_data() {
        QTest::addColumn<quint32>("leafCount");
        QTest::addColumn<bool>("accepted");

        QTest::newRow("exactly-64-definitions") << quint32(47) << true;
        QTest::newRow("65-definitions") << quint32(48) << false;
    }

    void boundsImportedContextDependencyClosures() {
        QFETCH(quint32, leafCount);
        QFETCH(bool, accepted);
        QString sourceText = QStringLiteral(
            "@context(\"h264-sps\", id) struct Leaf { bits<8> id; } "
            "@context(\"h264-pps\", id) ");
        for (quint32 index = 0; index < 4; ++index) {
            sourceText += QStringLiteral(
                "@context_dependency(\"h264-sps\", dependency%1) ").arg(index);
        }
        sourceText += QStringLiteral("struct Middle { bits<8> id; ");
        for (quint32 index = 0; index < 4; ++index) {
            sourceText += QStringLiteral("bits<8> dependency%1; ").arg(index);
        }
        sourceText += QStringLiteral(
            "} @context(\"aac-asc\", id) ");
        for (quint32 index = 0; index < 16; ++index) {
            sourceText += QStringLiteral(
                "@context_dependency(\"h264-pps\", dependency%1) ").arg(index);
        }
        sourceText += QStringLiteral("struct Root { bits<8> id; ");
        for (quint32 index = 0; index < 16; ++index) {
            sourceText += QStringLiteral("bits<8> dependency%1; ").arg(index);
        }
        sourceText += QStringLiteral(
            "} @context_import(\"aac-asc\", id) "
            "struct Consumer { bits<8> id; } entry Consumer;");
        const auto parsed = DslParser::parse(sourceText);
        QVERIFY2(parsed.succeeded(),
                 parsed.diagnostics.empty()
                     ? ""
                     : qPrintable(parsed.diagnostics.front().message));
        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY2(compiled.succeeded(),
                 compiled.diagnostics.empty()
                     ? ""
                     : qPrintable(compiled.diagnostics.front().message));
        const auto leafIndex = *compiled.program->structureIndex(QStringLiteral("Leaf"));
        const auto middleIndex = *compiled.program->structureIndex(QStringLiteral("Middle"));
        const auto rootIndex = *compiled.program->structureIndex(QStringLiteral("Root"));
        const auto consumerIndex =
            *compiled.program->structureIndex(QStringLiteral("Consumer"));

        std::vector<std::byte> data;
        for (quint32 leaf = 0; leaf < leafCount; ++leaf) {
            data.push_back(static_cast<std::byte>(leaf));
        }
        for (quint32 middle = 0; middle < 16; ++middle) {
            data.push_back(static_cast<std::byte>(middle));
            for (quint32 dependency = 0; dependency < 4; ++dependency) {
                data.push_back(static_cast<std::byte>(
                    ((middle * 4) + dependency) % leafCount));
            }
        }
        data.push_back(std::byte{1});
        for (quint32 dependency = 0; dependency < 16; ++dependency) {
            data.push_back(static_cast<std::byte>(dependency));
        }
        data.push_back(std::byte{1});
        MemorySource source(std::move(data));
        auto tree = AnalysisTree::create(QStringLiteral("bounded-import-closure"));
        QVERIFY(tree.has_value());
        RuleExecutionSession session(*compiled.program);
        quint64 sourceBit = 0;
        quint64 viewId = 1;

        for (quint32 leaf = 0; leaf < leafCount; ++leaf) {
            const auto view = makeView(viewId++, sourceBit, 8);
            QVERIFY(view.has_value());
            const auto result =
                session.run(makeRequest(source, leafIndex, *view, *tree));
            QVERIFY(result.materialized());
            sourceBit += 8;
        }
        for (quint32 middle = 0; middle < 16; ++middle) {
            const auto view = makeView(viewId++, sourceBit, 40);
            QVERIFY(view.has_value());
            const auto result =
                session.run(makeRequest(source, middleIndex, *view, *tree));
            QVERIFY(result.materialized());
            sourceBit += 40;
        }
        const auto rootView = makeView(viewId++, sourceBit, 136);
        QVERIFY(rootView.has_value());
        const auto root =
            session.run(makeRequest(source, rootIndex, *rootView, *tree));
        QVERIFY(root.materialized());
        sourceBit += 136;
        const auto consumerView = makeView(viewId, sourceBit, 8);
        QVERIFY(consumerView.has_value());

        const auto result =
            session.run(makeRequest(source, consumerIndex, *consumerView, *tree));

        if (accepted) {
            QVERIFY(result.materialized());
            QCOMPARE(result.importedContexts.size(), std::size_t(1));
            QCOMPARE(result.importedContexts.front().definitions.size(),
                     std::size_t(64));
        } else {
            QCOMPARE(result.status, RuleExecutionStatus::ResourceLimit);
            QVERIFY(result.importedContexts.empty());
            QVERIFY(result.execution.structureNode.has_value());
            const auto consumer = tree->node(*result.execution.structureNode);
            QVERIFY(consumer.has_value());
            QCOMPARE(consumer->diagnostics().size(), std::size_t(1));
            QCOMPARE(consumer->diagnostics().front().code,
                     DiagnosticCode::ResourceLimit);
            QCOMPARE(consumer->diagnostics().front().fieldPath,
                     QStringLiteral("Consumer.id"));
        }
    }

    void executesLocallyScopedContextImportKeysInsideRepeatAndSwitch() {
        const auto parsed = DslParser::parse(QStringLiteral(R"(
            @context("h264-sps", id)
            struct Sps {
                bits<8> id;
                bits<8> delay_minus1 @context_export;
            }

            @context_import("h264-sps", seq_parameter_set_id)
            struct SeiRbsp {
                repeat (2) while (more_rbsp_data()) {
                    ff_coded<8> payload_type;
                    ff_coded<64> payload_size;
                    switch (payload_type) {
                        case 0: {
                            bits<8> seq_parameter_set_id;
                            bits<(context_value(seq_parameter_set_id, h264_sps, delay_minus1) + 1)> cpb_delay;
                        }
                        default: {
                            @lazy(payload_size) bytes payload_data;
                        }
                    }
                }
                rbsp_trailing_bits;
            }
            entry SeiRbsp;
        )"));
        QVERIFY2(parsed.succeeded(),
                 parsed.diagnostics.empty()
                     ? ""
                     : qPrintable(parsed.diagnostics.front().message));
        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY2(compiled.succeeded(),
                 compiled.diagnostics.empty()
                     ? ""
                     : qPrintable(compiled.diagnostics.front().message));

        const auto spsIndex = *compiled.program->structureIndex(QStringLiteral("Sps"));
        const auto seiIndex = *compiled.program->structureIndex(QStringLiteral("SeiRbsp"));

        // SPS 0: id = 0, delay_minus1 = 7 (width = 8)
        // SPS 1: id = 1, delay_minus1 = 15 (width = 16)
        // SEI: iter0 (type 0, size 2, sps 0, delay 0x55) + iter1 (type 0, size 3, sps 1, delay 0x1234) + 0x80 (trailing)
        MemorySource source(bytes({
            0x00, 0x07,                         // SPS 0 (16 bits)
            0x01, 0x0f,                         // SPS 1 (16 bits)
            0x00, 0x02, 0x00, 0x55,             // SEI iter0: type 0, size 2, sps 0, delay 8 bits (0x55)
            0x00, 0x03, 0x01, 0x12, 0x34,       // SEI iter1: type 0, size 3, sps 1, delay 16 bits (0x1234)
            0x80                                // rbsp_trailing_bits (8 bits)
        }));

        const auto sps0View = makeView(1, 0, 16);
        const auto sps1View = makeView(2, 16, 16);
        const auto seiView = makeView(3, 32, 80);
        QVERIFY(sps0View.has_value() && sps1View.has_value() && seiView.has_value());

        auto tree = AnalysisTree::create(QStringLiteral("locally-scoped-import-tree"));
        QVERIFY(tree.has_value());
        RuleExecutionSession session(*compiled.program);

        const auto sps0 = session.run(makeRequest(source, spsIndex, *sps0View, *tree));
        QVERIFY(sps0.materialized());
        const auto sps1 = session.run(makeRequest(source, spsIndex, *sps1View, *tree));
        QVERIFY(sps1.materialized());

        const auto sei = session.run(makeRequest(source, seiIndex, *seiView, *tree));
        QVERIFY2(sei.materialized(), qPrintable(sei.errorMessage));
        QCOMPARE(sei.importedContexts.size(), std::size_t(2));
        QCOMPARE(sei.importedContexts.at(0).definitionId, *sps0.publishedDefinition);
        QCOMPARE(sei.importedContexts.at(1).definitionId, *sps1.publishedDefinition);

        const auto seiNode = tree->node(*sei.execution.structureNode);
        QVERIFY(seiNode.has_value());
        const std::vector<QString> expectedChildren{
            QStringLiteral("payload_type[0]"),
            QStringLiteral("payload_size[0]"),
            QStringLiteral("seq_parameter_set_id[0]"),
            QStringLiteral("cpb_delay[0]"),
            QStringLiteral("payload_type[1]"),
            QStringLiteral("payload_size[1]"),
            QStringLiteral("seq_parameter_set_id[1]"),
            QStringLiteral("cpb_delay[1]"),
            QStringLiteral("rbsp_stop_one_bit"),
            QStringLiteral("rbsp_alignment_zero_bit[0]"),
            QStringLiteral("rbsp_alignment_zero_bit[1]"),
            QStringLiteral("rbsp_alignment_zero_bit[2]"),
            QStringLiteral("rbsp_alignment_zero_bit[3]"),
            QStringLiteral("rbsp_alignment_zero_bit[4]"),
            QStringLiteral("rbsp_alignment_zero_bit[5]"),
            QStringLiteral("rbsp_alignment_zero_bit[6]"),
        };
        std::vector<QString> actualChildren;
        for (const auto& childId : seiNode->children()) {
            actualChildren.push_back(tree->node(childId)->name());
        }
        QCOMPARE(actualChildren, expectedChildren);
        QCOMPARE(tree->node(seiNode->children().at(2))->value().toULongLong(), quint64(0));
        QCOMPARE(tree->node(seiNode->children().at(3))->value().toULongLong(), quint64(0x55));
        QCOMPARE(tree->node(seiNode->children().at(6))->value().toULongLong(), quint64(1));
        QCOMPARE(tree->node(seiNode->children().at(7))->value().toULongLong(), quint64(0x1234));
    }

    void handlesLocallyScopedContextImportFailureGracefully() {
        const auto parsed = DslParser::parse(QStringLiteral(R"(
            @context("h264-sps", id)
            struct Sps {
                bits<8> id;
                bits<8> delay_minus1 @context_export;
            }

            @context_import("h264-sps", seq_parameter_set_id)
            struct SeiRbsp {
                ff_coded<8> payload_type;
                switch (payload_type) {
                    case 0: {
                        bits<8> seq_parameter_set_id;
                        bits<(context_value(seq_parameter_set_id, h264_sps, delay_minus1) + 1)> cpb_delay;
                    }
                    default: {
                        bits<8> dummy;
                    }
                }
            }
            entry SeiRbsp;
        )"));
        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY(compiled.succeeded());

        const auto seiIndex = *compiled.program->structureIndex(QStringLiteral("SeiRbsp"));
        // payload_type = 0, seq_parameter_set_id = 99 (unknown SPS)
        MemorySource source(bytes({0x00, 99, 0x00}));
        const auto seiView = makeView(1, 0, 24);
        QVERIFY(seiView.has_value());
        auto tree = AnalysisTree::create(QStringLiteral("failure-tree"));
        QVERIFY(tree.has_value());
        RuleExecutionSession session(*compiled.program);

        const auto sei = session.run(makeRequest(source, seiIndex, *seiView, *tree));
        QCOMPARE(sei.status, RuleExecutionStatus::DependencyUnavailable);
    }

    void resolvesAmbientContextSuccessfullyAndExtractsExportedField() {
        const auto parsed = DslParser::parse(QStringLiteral(R"(
            @context("h264-sps", id)
            struct Sps {
                bits<8> id;
                bits<8> delay_minus1 @context_export;
            }

            @context_import("h264-sps")
            struct SeiRbsp {
                bits<8> payload_type;
                bits<(context_value(h264_sps, delay_minus1) + 1)> cpb_delay;
            }
            entry SeiRbsp;
        )"));
        QVERIFY(parsed.succeeded());
        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY(compiled.succeeded());

        const auto spsIndex = *compiled.program->structureIndex(QStringLiteral("Sps"));
        const auto seiIndex = *compiled.program->structureIndex(QStringLiteral("SeiRbsp"));

        // SPS at [0, 16): id = 0, delay_minus1 = 7 (7+1 = 8 bits)
        // SEI at [16, 32): payload_type = 1 (8 bits), cpb_delay = 0xAB (8 bits)
        MemorySource source(bytes({0x00, 0x07, 0x01, 0xAB}));
        const auto spsView = makeView(1, 0, 16);
        const auto seiView = makeView(2, 16, 16);
        QVERIFY(spsView.has_value() && seiView.has_value());
        auto tree = AnalysisTree::create(QStringLiteral("ambient-exec-tree"));
        QVERIFY(tree.has_value());

        RuleExecutionSession session(*compiled.program);
        const auto sps = session.run(makeRequest(source, spsIndex, *spsView, *tree));
        QVERIFY(sps.materialized());
        QVERIFY(sps.publishedDefinition.has_value());

        const auto sei = session.run(makeRequest(source, seiIndex, *seiView, *tree));
        QVERIFY2(sei.materialized(), qPrintable(sei.errorMessage));
        QCOMPARE(sei.execution.contextImports.size(), std::size_t(1));
        QVERIFY(!sei.execution.contextImports.front().key.has_value());
        QCOMPARE(sei.importedContexts.size(), std::size_t(1));
        QCOMPARE(sei.importedContexts.front().definitionId, *sps.publishedDefinition);

        const auto seiNode = tree->node(*sei.execution.structureNode);
        QVERIFY(seiNode.has_value());
        QCOMPARE(seiNode->children().size(), std::size_t(2));
        const auto delayNode = tree->node(seiNode->children().at(1));
        QVERIFY(delayNode.has_value());
        QCOMPARE(delayNode->name(), QStringLiteral("cpb_delay"));
        QCOMPARE(delayNode->value().toULongLong(), quint64(0xAB));
        QCOMPARE(delayNode->location()->sourceSpans().front().bitLength(), quint64(8));
    }

    void supportsCoexistenceOfKeyedAndAmbientImportsInSameExecution() {
        const auto parsed = DslParser::parse(QStringLiteral(R"(
            @context("h264-sps", id)
            struct Sps {
                bits<8> id;
                bits<8> val0 @context_export;
                bits<8> val1 @context_export;
            }

            @context_import("h264-sps", keyed_sps_id)
            @context_import("h264-sps")
            struct DualConsumer {
                bits<8> keyed_sps_id;
                bits<(context_value(keyed_sps_id, h264_sps, val0) + 1)> keyed_data;
                bits<(context_value(h264_sps, val1) + 1)> ambient_data;
            }
            entry DualConsumer;
        )"));
        QVERIFY(parsed.succeeded());
        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY(compiled.succeeded());

        const auto spsIndex = *compiled.program->structureIndex(QStringLiteral("Sps"));
        const auto dualIndex = *compiled.program->structureIndex(QStringLiteral("DualConsumer"));

        auto tree = AnalysisTree::create(QStringLiteral("dual-exec-tree"));
        QVERIFY(tree.has_value());
        RuleExecutionSession session(*compiled.program);

        // SPS 0 at [0, 24): id = 0, val0 = 3 (4 bits), val1 = 5 (6 bits)
        // SPS 1 at [24, 48): id = 1, val0 = 15 (16 bits), val1 = 7 (8 bits)
        // DualConsumer at [48, 68):
        // keyed_sps_id = 0 (8 bits) -> val0 of SPS 0 is 3 -> keyed_data is (3+1)=4 bits (value 0xA)
        // ambient SPS is SPS 1 (latest) -> val1 of SPS 1 is 7 -> ambient_data is (7+1)=8 bits (value 0x55)
        // Total bits = 8 + 4 + 8 = 20 bits
        // bytes: 0x00 (8 bits), 0xA5 (4 bits 0xA + 4 bits 0x5), 0x50 (4 bits 0x5 + 4 bits 0x0)
        MemorySource source(bytes({0x00, 0x03, 0x05, 0x01, 0x0F, 0x07, 0x00, 0xA5, 0x50}));
        const auto sps0View = makeView(1, 0, 24);
        const auto sps1View = makeView(2, 24, 24);
        const auto dualView = makeView(3, 48, 20);
        QVERIFY(sps0View.has_value() && sps1View.has_value() && dualView.has_value());

        const auto sps0 = session.run(makeRequest(source, spsIndex, *sps0View, *tree));
        QVERIFY(sps0.materialized());

        const auto sps1 = session.run(makeRequest(source, spsIndex, *sps1View, *tree));
        QVERIFY(sps1.materialized());

        const auto dual = session.run(makeRequest(source, dualIndex, *dualView, *tree));
        QVERIFY2(dual.materialized(), qPrintable(dual.errorMessage));
        QCOMPARE(dual.importedContexts.size(), std::size_t(2));
        QCOMPARE(dual.importedContexts.at(0).definitionId, *sps0.publishedDefinition);
        QCOMPARE(dual.importedContexts.at(1).definitionId, *sps1.publishedDefinition);

        const auto dualNode = tree->node(*dual.execution.structureNode);
        QVERIFY(dualNode.has_value());
        QCOMPARE(dualNode->children().size(), std::size_t(3));
        const auto keyedNode = tree->node(dualNode->children().at(1));
        QVERIFY(keyedNode.has_value());
        QCOMPARE(keyedNode->name(), QStringLiteral("keyed_data"));
        QCOMPARE(keyedNode->location()->sourceSpans().front().bitLength(), quint64(4));
        QCOMPARE(keyedNode->value().toULongLong(), quint64(0xA));

        const auto ambientNode = tree->node(dualNode->children().at(2));
        QVERIFY(ambientNode.has_value());
        QCOMPARE(ambientNode->name(), QStringLiteral("ambient_data"));
        QCOMPARE(ambientNode->location()->sourceSpans().front().bitLength(), quint64(8));
        QCOMPARE(ambientNode->value().toULongLong(), quint64(0x55));
    }

    void handlesAmbientContextNotFoundAndDependencyUnavailableIsolation() {
        const auto parsed = DslParser::parse(QStringLiteral(R"(
            @context("h264-sps", id)
            struct Sps {
                bits<8> id;
                bits<8> val @context_export;
            }

            @context("h264-pps", id)
            @context_dependency("h264-sps", sps_id)
            struct Pps {
                bits<8> id;
                bits<8> sps_id;
                bits<8> pps_val @context_export;
            }

            @context_import("h264-pps")
            struct AmbientPpsConsumer {
                bits<8> header;
                bits<(context_value(h264_pps, pps_val) + 1)> data;
            }
            entry AmbientPpsConsumer;
        )"));
        QVERIFY(parsed.succeeded());
        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY(compiled.succeeded());

        const auto spsIndex = *compiled.program->structureIndex(QStringLiteral("Sps"));
        const auto ppsIndex = *compiled.program->structureIndex(QStringLiteral("Pps"));
        const auto consumerIndex = *compiled.program->structureIndex(QStringLiteral("AmbientPpsConsumer"));

        // Case 1: NotFound (no PPS registered)
        {
            auto tree = AnalysisTree::create(QStringLiteral("not-found-tree"));
            QVERIFY(tree.has_value());
            RuleExecutionSession session(*compiled.program);

            MemorySource source(bytes({0x01, 0x00}));
            const auto view = makeView(1, 0, 16);
            QVERIFY(view.has_value());

            const auto res = session.run(makeRequest(source, consumerIndex, *view, *tree));
            QCOMPARE(res.status, RuleExecutionStatus::DependencyUnavailable);
            QCOMPARE(res.errorMessage, QStringLiteral("Imported context was not defined before this structure"));
        }

        // Case 2: DependencyUnavailable (PPS registered with SPS 0 gen 1, then SPS 0 redefined to gen 2)
        {
            auto tree = AnalysisTree::create(QStringLiteral("dep-unavail-tree"));
            QVERIFY(tree.has_value());
            RuleExecutionSession session(*compiled.program);

            // SPS 0 gen 1 at [0, 16)
            // PPS 0 at [16, 40) depending on SPS 0 gen 1
            // SPS 0 gen 2 at [40, 56) (redefines SPS 0, invalidating PPS 0's dependency)
            // AmbientPpsConsumer at [56, 72)
            MemorySource source(bytes({0x00, 0x07, 0x00, 0x00, 0x03, 0x00, 0x0F, 0x01, 0x00}));
            const auto sps0Gen1View = makeView(1, 0, 16);
            const auto pps0View = makeView(2, 16, 24);
            const auto sps0Gen2View = makeView(3, 40, 16);
            const auto consumerView = makeView(4, 56, 16);
            QVERIFY(sps0Gen1View.has_value() && pps0View.has_value() &&
                    sps0Gen2View.has_value() && consumerView.has_value());

            const auto sps0Gen1 = session.run(makeRequest(source, spsIndex, *sps0Gen1View, *tree));
            QVERIFY(sps0Gen1.materialized());

            const auto pps0 = session.run(makeRequest(source, ppsIndex, *pps0View, *tree));
            QVERIFY(pps0.materialized());

            const auto sps0Gen2 = session.run(makeRequest(source, spsIndex, *sps0Gen2View, *tree));
            QVERIFY(sps0Gen2.materialized());

            const auto res = session.run(makeRequest(source, consumerIndex, *consumerView, *tree));
            QCOMPARE(res.status, RuleExecutionStatus::DependencyUnavailable);
            QCOMPARE(res.errorMessage, QStringLiteral("Imported context generation is unavailable"));
        }
    }

    void materializesStructureWithUnaccessedAmbientImportWithoutContextAndIsolatesFailures() {
        const auto parsed = DslParser::parse(QStringLiteral(R"(
            @context("h264-sps", id)
            struct Sps {
                bits<8> id;
                bits<8> val @context_export;
            }

            @context_import("h264-sps")
            struct MultiBranchMessageContainer {
                bits<8> msg_type;
                switch (msg_type) {
                    case 0: {
                        bits<16> unaccessed_payload;
                    }
                    case 1: {
                        bits<(context_value(h264_sps, val) + 1)> dependent_payload;
                    }
                    default: {
                        bits<8> fallback_payload;
                    }
                }
            }
            entry MultiBranchMessageContainer;
        )"));
        QVERIFY(parsed.succeeded());
        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY(compiled.succeeded());

        const auto spsIndex = *compiled.program->structureIndex(QStringLiteral("Sps"));
        const auto containerIndex =
            *compiled.program->structureIndex(QStringLiteral("MultiBranchMessageContainer"));

        auto tree = AnalysisTree::create(QStringLiteral("ambient-isolation-tree"));
        QVERIFY(tree.has_value());
        RuleExecutionSession session(*compiled.program);

        // Stream layout:
        // [0, 24): Message 0 (msg_type = 0, unaccessed_payload = 0x1234) -> 24 bits (3 bytes)
        // [24, 40): Message 1 (msg_type = 1, dummy 8 bits) -> 16 bits (2 bytes)
        // [40, 56): SPS 0 (id = 0, val = 7 -> 8-bit payload) -> 16 bits (2 bytes)
        // [56, 72): Message 2 (msg_type = 1, dependent_payload = 0xAB) -> 16 bits (2 bytes)
        // [72, 96): Message 3 (msg_type = 0, unaccessed_payload = 0x5678) -> 24 bits (3 bytes)
        MemorySource source(bytes({
            0x00, 0x12, 0x34, // [0, 24): msg_type=0, unaccessed_payload=0x1234
            0x01, 0x00,       // [24, 40): msg_type=1, missing SPS
            0x00, 0x07,       // [40, 56): SPS id=0, val=7
            0x01, 0xAB,       // [56, 72): msg_type=1, dependent_payload=0xAB
            0x00, 0x56, 0x78  // [72, 96): msg_type=0, unaccessed_payload=0x5678
        }));

        const auto msg0View = makeView(1, 0, 24);
        const auto msg1View = makeView(2, 24, 16);
        const auto spsView = makeView(3, 40, 16);
        const auto msg2View = makeView(4, 56, 16);
        const auto msg3View = makeView(5, 72, 24);
        QVERIFY(msg0View.has_value() && msg1View.has_value() && spsView.has_value() &&
                msg2View.has_value() && msg3View.has_value());

        // (a) Message 0: branch does not evaluate context_value, materializes without preceding SPS
        const auto msg0 = session.run(makeRequest(source, containerIndex, *msg0View, *tree));
        QVERIFY2(msg0.materialized(), qPrintable(msg0.errorMessage));
        QVERIFY(msg0.importedContexts.empty());
        const auto msg0Node = tree->node(*msg0.execution.structureNode);
        QVERIFY(msg0Node.has_value());
        QCOMPARE(msg0Node->children().size(), std::size_t(2));
        const auto unacc0Node = tree->node(msg0Node->children().at(1));
        QVERIFY(unacc0Node.has_value());
        QCOMPARE(unacc0Node->name(), QStringLiteral("unaccessed_payload"));
        QCOMPARE(unacc0Node->value().toULongLong(), quint64(0x1234));

        // (b) Message 1: branch evaluates context_value with no SPS, fails with DependencyUnavailable isolated to this message
        const auto msg1 = session.run(makeRequest(source, containerIndex, *msg1View, *tree));
        QCOMPARE(msg1.status, RuleExecutionStatus::DependencyUnavailable);
        QCOMPARE(msg1.errorMessage,
                 QStringLiteral("Imported context was not defined before this structure"));

        // Register SPS 0
        const auto sps = session.run(makeRequest(source, spsIndex, *spsView, *tree));
        QVERIFY(sps.materialized());
        QVERIFY(sps.publishedDefinition.has_value());

        // (c) Message 2: branch evaluates context_value with SPS present, materializes and binds SPS
        const auto msg2 = session.run(makeRequest(source, containerIndex, *msg2View, *tree));
        QVERIFY2(msg2.materialized(), qPrintable(msg2.errorMessage));
        QCOMPARE(msg2.importedContexts.size(), std::size_t(1));
        QCOMPARE(msg2.importedContexts.front().definitionId, *sps.publishedDefinition);
        const auto msg2Node = tree->node(*msg2.execution.structureNode);
        QVERIFY(msg2Node.has_value());
        QCOMPARE(msg2Node->children().size(), std::size_t(2));
        const auto dep2Node = tree->node(msg2Node->children().at(1));
        QVERIFY(dep2Node.has_value());
        QCOMPARE(dep2Node->name(), QStringLiteral("dependent_payload"));
        QCOMPARE(dep2Node->value().toULongLong(), quint64(0xAB));
        QCOMPARE(dep2Node->location()->sourceSpans().front().bitLength(), quint64(8));

        // (d) Message 3: followup unaccessed branch after SPS, materializes without attaching unused import
        const auto msg3 = session.run(makeRequest(source, containerIndex, *msg3View, *tree));
        QVERIFY2(msg3.materialized(), qPrintable(msg3.errorMessage));
        QVERIFY(msg3.importedContexts.empty());
        const auto msg3Node = tree->node(*msg3.execution.structureNode);
        QVERIFY(msg3Node.has_value());
        QCOMPARE(msg3Node->children().size(), std::size_t(2));
        const auto unacc3Node = tree->node(msg3Node->children().at(1));
        QVERIFY(unacc3Node.has_value());
        QCOMPARE(unacc3Node->name(), QStringLiteral("unaccessed_payload"));
        QCOMPARE(unacc3Node->value().toULongLong(), quint64(0x5678));
    }
};

QTEST_APPLESS_MAIN(RuleExecutionSessionTest)

#include "rule_execution_session_test.moc"
