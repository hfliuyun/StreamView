#include <streamview/core/analysis_model.h>

#include <QTest>

using streamview::core::AnalysisNodeId;
using streamview::core::AnalysisNodeKind;
using streamview::core::AnalysisNodeSpec;
using streamview::core::AnalysisTree;
using streamview::core::DiagnosticCode;
using streamview::core::DiagnosticSeverity;
using streamview::core::LogicalBitAddress;
using streamview::core::LogicalRange;
using streamview::core::LogicalViewId;
using streamview::core::MaterializationState;
using streamview::core::ParseDiagnostic;
using streamview::core::SourceBitAddress;
using streamview::core::SourceMapping;
using streamview::core::SourceSpan;

namespace {

std::optional<streamview::core::FieldLocation> testLocation() {
    const auto span = SourceSpan::create(SourceBitAddress(8), 8);
    if (!span) {
        return std::nullopt;
    }
    const auto mapping = SourceMapping::create(LogicalViewId(1), {*span});
    if (!mapping) {
        return std::nullopt;
    }
    const auto range = LogicalRange::create(LogicalBitAddress(LogicalViewId(1), 0), 8);
    if (!range) {
        return std::nullopt;
    }
    return mapping->locate(*range);
}

} // namespace

class AnalysisModelTest final : public QObject {
    Q_OBJECT

private slots:
    void createsAnIndexingRootAndStableSnapshots() {
        const auto empty = AnalysisTree::create({});
        QVERIFY(!empty.has_value());

        auto tree = AnalysisTree::create(QStringLiteral("H.264 stream"));
        QVERIFY(tree.has_value());
        QCOMPARE(tree->rootId().value(), quint64{1});
        QCOMPARE(tree->nodeCount(), std::size_t{1});

        const auto before = tree->node(tree->rootId());
        QVERIFY(before.has_value());
        QVERIFY(before->children().empty());
        QCOMPARE(before->state(), MaterializationState::Indexing);

        AnalysisNodeSpec structure;
        structure.kind = AnalysisNodeKind::Structure;
        structure.name = QStringLiteral("NAL unit");
        structure.state = MaterializationState::Indexing;
        const auto structureId = tree->appendChild(tree->rootId(), structure);
        QVERIFY(structureId.has_value());

        QVERIFY(before->children().empty());
        const auto current = tree->node(tree->rootId());
        QVERIFY(current.has_value());
        QCOMPARE(current->children().size(), std::size_t{1});
    }

    void enforcesFieldLocationAndParentRules() {
        auto tree = AnalysisTree::create(QStringLiteral("root"));
        QVERIFY(tree.has_value());

        AnalysisNodeSpec syntax;
        syntax.kind = AnalysisNodeKind::SyntaxField;
        syntax.name = QStringLiteral("forbidden_zero_bit");
        syntax.state = MaterializationState::Materialized;
        QVERIFY(!tree->appendChild(tree->rootId(), syntax).has_value());

        AnalysisNodeSpec computed;
        computed.kind = AnalysisNodeKind::ComputedField;
        computed.name = QStringLiteral("is_vcl");
        computed.location = testLocation();
        QVERIFY(computed.location.has_value());
        QVERIFY(!tree->appendChild(tree->rootId(), computed).has_value());

        AnalysisNodeSpec structure;
        structure.kind = AnalysisNodeKind::Structure;
        structure.name = QStringLiteral("NAL header");
        structure.state = MaterializationState::Materialized;
        const auto structureId = tree->appendChild(tree->rootId(), structure);
        QVERIFY(structureId.has_value());
        QVERIFY(!tree->appendChild(*structureId, computed).has_value());

        QVERIFY(tree->transition(tree->rootId(), MaterializationState::Materialized));
        QVERIFY(!tree->appendChild(tree->rootId(), structure).has_value());
    }

    void retainsChildrenAndDiagnosticsForPartialResults() {
        auto tree = AnalysisTree::create(QStringLiteral("root"));
        QVERIFY(tree.has_value());

        AnalysisNodeSpec structure;
        structure.kind = AnalysisNodeKind::Structure;
        structure.name = QStringLiteral("SPS");
        structure.state = MaterializationState::Indexing;
        const auto structureId = tree->appendChild(tree->rootId(), structure);
        QVERIFY(structureId.has_value());

        AnalysisNodeSpec field;
        field.kind = AnalysisNodeKind::SyntaxField;
        field.name = QStringLiteral("profile_idc");
        field.state = MaterializationState::Materialized;
        field.value = QVariant::fromValue(qulonglong{100});
        field.location = testLocation();
        QVERIFY(field.location.has_value());
        const auto fieldId = tree->appendChild(*structureId, field);
        QVERIFY(fieldId.has_value());

        ParseDiagnostic diagnostic;
        diagnostic.code = DiagnosticCode::TruncatedSource;
        diagnostic.severity = DiagnosticSeverity::Error;
        diagnostic.message = QStringLiteral("SPS ended before seq_parameter_set_id");
        diagnostic.fieldPath = QStringLiteral("sps.seq_parameter_set_id");
        diagnostic.location = testLocation();
        QVERIFY(tree->markPartial(*structureId, MaterializationState::Invalid, diagnostic));

        QVERIFY(tree->hasPartialResults());
        QVERIFY(!tree->isFullyMaterialized());
        const auto snapshot = tree->node(*structureId);
        QVERIFY(snapshot.has_value());
        QCOMPARE(snapshot->state(), MaterializationState::Invalid);
        QCOMPARE(snapshot->children().size(), std::size_t{1});
        QCOMPARE(snapshot->diagnostics().size(), std::size_t{1});
        QCOMPARE(snapshot->diagnostics().front().fieldPath,
                 QStringLiteral("sps.seq_parameter_set_id"));
        QVERIFY(snapshot->diagnostics().front().location.has_value());
        QVERIFY(!tree->transition(*structureId, MaterializationState::Indexing));
    }

    void allowsCancellationResumeAndControlledTransitions() {
        auto tree = AnalysisTree::create(QStringLiteral("root"));
        QVERIFY(tree.has_value());

        AnalysisNodeSpec region;
        region.kind = AnalysisNodeKind::Region;
        region.name = QStringLiteral("slice data");
        region.state = MaterializationState::Indexing;
        const auto regionId = tree->appendChild(tree->rootId(), region);
        QVERIFY(regionId.has_value());

        ParseDiagnostic cancelled;
        cancelled.code = DiagnosticCode::Cancelled;
        cancelled.severity = DiagnosticSeverity::Info;
        cancelled.message = QStringLiteral("Indexing cancelled by user");
        QVERIFY(tree->markPartial(*regionId, MaterializationState::Cancelled, cancelled));
        QVERIFY(tree->transition(*regionId, MaterializationState::Indexing));
        QVERIFY(tree->transition(*regionId, MaterializationState::WaitingDependency));
        QVERIFY(tree->transition(*regionId, MaterializationState::Materialized));
        QVERIFY(!tree->transition(*regionId, MaterializationState::Invalid));
        QVERIFY(!tree->addDiagnostic(AnalysisNodeId(999), cancelled));
        QVERIFY(tree->transition(tree->rootId(), MaterializationState::Materialized));
        QVERIFY(tree->isFullyMaterialized());
        QVERIFY(!tree->hasPartialResults());
    }
};

QTEST_GUILESS_MAIN(AnalysisModelTest)

#include "analysis_model_test.moc"
