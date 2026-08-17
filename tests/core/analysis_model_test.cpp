#include <streamview/core/analysis_model.h>

#include <QTest>

#include <initializer_list>
#include <utility>
#include <vector>

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

std::optional<streamview::core::FieldLocation> testLocationFromSpans(
    std::initializer_list<std::pair<quint64, quint64>> ranges) {
    std::vector<SourceSpan> spans;
    spans.reserve(ranges.size());
    for (const auto& [sourceBitOffset, bitLength] : ranges) {
        const auto span = SourceSpan::create(SourceBitAddress(sourceBitOffset), bitLength);
        if (!span) {
            return std::nullopt;
        }
        spans.push_back(*span);
    }
    const auto mapping = SourceMapping::create(LogicalViewId(1), std::move(spans));
    if (!mapping) {
        return std::nullopt;
    }
    const auto range = LogicalRange::create(
        LogicalBitAddress(LogicalViewId(1), 0), mapping->logicalBitLength());
    if (!range) {
        return std::nullopt;
    }
    return mapping->locate(*range);
}

std::optional<streamview::core::FieldLocation> testLocation(quint64 sourceBitOffset = 8,
                                                            quint64 bitLength = 8) {
    return testLocationFromSpans({{sourceBitOffset, bitLength}});
}

} // namespace

class AnalysisModelTest final : public QObject {
    Q_OBJECT

private slots:
    void preservesInstanceIdentityOnlyAcrossMoves() {
        auto original = AnalysisTree::create(QStringLiteral("identity"));
        QVERIFY(original.has_value());
        const quint64 originalIdentity = original->instanceIdentity();
        QVERIFY(originalIdentity != 0);

        AnalysisTree copy(*original);
        QVERIFY(copy.instanceIdentity() != originalIdentity);
        AnalysisTree copyAssigned = copy;
        const quint64 copyAssignedIdentity = copyAssigned.instanceIdentity();
        QVERIFY(copyAssignedIdentity != copy.instanceIdentity());
        copyAssigned = *original;
        QVERIFY(copyAssigned.instanceIdentity() != copyAssignedIdentity);
        QVERIFY(copyAssigned.instanceIdentity() != originalIdentity);

        AnalysisTree moved(std::move(*original));
        QCOMPARE(moved.instanceIdentity(), originalIdentity);
        QVERIFY(original->instanceIdentity() != originalIdentity);
        auto moveAssigned = AnalysisTree::create(QStringLiteral("move-assigned"));
        QVERIFY(moveAssigned.has_value());
        *moveAssigned = std::move(moved);
        QCOMPARE(moveAssigned->instanceIdentity(), originalIdentity);
        QVERIFY(moved.instanceIdentity() != originalIdentity);
    }

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

    void restoresTransactionalSnapshotWithoutChangingIdentity() {
        auto tree = AnalysisTree::create(QStringLiteral("transaction"));
        QVERIFY(tree.has_value());
        const quint64 identity = tree->instanceIdentity();
        auto snapshot = tree->snapshot();

        AnalysisNodeSpec child;
        child.kind = AnalysisNodeKind::Region;
        child.name = QStringLiteral("temporary");
        child.state = MaterializationState::Materialized;
        QVERIFY(tree->appendChild(tree->rootId(), std::move(child)).has_value());
        QCOMPARE(tree->nodeCount(), std::size_t{2});

        QVERIFY(tree->restore(std::move(snapshot)));
        QCOMPARE(tree->instanceIdentity(), identity);
        QCOMPARE(tree->nodeCount(), std::size_t{1});
        const auto root = tree->node(tree->rootId());
        QVERIFY(root.has_value());
        QVERIFY(root->children().empty());
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

        AnalysisNodeSpec child;
        child.kind = AnalysisNodeKind::Region;
        child.name = QStringLiteral("committed partial child");
        child.state = MaterializationState::Indexing;
        const auto childId = tree->appendChild(*regionId, child);
        QVERIFY(childId.has_value());

        ParseDiagnostic cancelled;
        cancelled.code = DiagnosticCode::Cancelled;
        cancelled.severity = DiagnosticSeverity::Info;
        cancelled.message = QStringLiteral("Indexing cancelled by user");

        ParseDiagnostic truncated;
        truncated.code = DiagnosticCode::TruncatedSource;
        truncated.severity = DiagnosticSeverity::Error;
        truncated.message = QStringLiteral("A prior diagnostic remains relevant");
        QVERIFY(tree->addDiagnostic(*regionId, truncated));
        QVERIFY(tree->markPartial(*regionId, MaterializationState::Cancelled, cancelled));
        cancelled.message = QStringLiteral("A second cancellation diagnostic");
        QVERIFY(tree->addDiagnostic(*regionId, cancelled));
        QVERIFY(tree->markPartial(*childId, MaterializationState::Cancelled, cancelled));

        AnalysisNodeSpec materialized;
        materialized.kind = AnalysisNodeKind::Region;
        materialized.name = QStringLiteral("completed region");
        materialized.state = MaterializationState::Materialized;
        const auto materializedId = tree->appendChild(tree->rootId(), materialized);
        QVERIFY(materializedId.has_value());
        QVERIFY(tree->addDiagnostic(*materializedId, cancelled));

        AnalysisNodeSpec waitingNode;
        waitingNode.kind = AnalysisNodeKind::Region;
        waitingNode.name = QStringLiteral("waiting region");
        waitingNode.state = MaterializationState::Indexing;
        const auto waitingId = tree->appendChild(tree->rootId(), waitingNode);
        QVERIFY(waitingId.has_value());
        ParseDiagnostic waiting;
        waiting.code = DiagnosticCode::DependencyUnavailable;
        waiting.severity = DiagnosticSeverity::Error;
        waiting.message = QStringLiteral("dependency unavailable");
        QVERIFY(tree->markPartial(
            *waitingId, MaterializationState::WaitingDependency, waiting));
        const auto waitingRegion = tree->node(*waitingId);
        QVERIFY(waitingRegion.has_value());
        QCOMPARE(waitingRegion->state(), MaterializationState::WaitingDependency);
        QCOMPARE(waitingRegion->diagnostics().back().code,
                 DiagnosticCode::DependencyUnavailable);
        QVERIFY(tree->transition(*waitingId, MaterializationState::Indexing));
        QVERIFY(tree->transition(*waitingId, MaterializationState::Materialized));

        const auto nodeCount = tree->nodeCount();
        QVERIFY(!tree->resumeCancelled(AnalysisNodeId(999)));
        QVERIFY(!tree->resumeCancelled(*materializedId));
        QCOMPARE(tree->nodeCount(), nodeCount);
        const auto unchanged = tree->node(*materializedId);
        QVERIFY(unchanged.has_value());
        QCOMPARE(unchanged->state(), MaterializationState::Materialized);
        QCOMPARE(unchanged->diagnostics().size(), std::size_t{1});
        QCOMPARE(unchanged->diagnostics().front().code, DiagnosticCode::Cancelled);

        QVERIFY(tree->resumeCancelled(*regionId));
        const auto resumed = tree->node(*regionId);
        QVERIFY(resumed.has_value());
        QCOMPARE(resumed->state(), MaterializationState::Indexing);
        QCOMPARE(resumed->diagnostics().size(), std::size_t{1});
        QCOMPARE(resumed->diagnostics().front().code, DiagnosticCode::TruncatedSource);

        const auto unchangedChild = tree->node(*childId);
        QVERIFY(unchangedChild.has_value());
        QCOMPARE(unchangedChild->state(), MaterializationState::Cancelled);
        QCOMPARE(unchangedChild->diagnostics().size(), std::size_t{1});
        QCOMPARE(unchangedChild->diagnostics().front().code, DiagnosticCode::Cancelled);
        QVERIFY(tree->hasPartialResults());

        QVERIFY(!tree->resumeCancelled(*regionId));
        QCOMPARE(tree->node(*regionId)->diagnostics().size(), std::size_t{1});
        QVERIFY(tree->transition(*regionId, MaterializationState::WaitingDependency));
        QVERIFY(tree->transition(*regionId, MaterializationState::Materialized));
        QVERIFY(!tree->transition(*regionId, MaterializationState::Invalid));
        QVERIFY(tree->resumeCancelled(*childId));
        QVERIFY(tree->transition(*childId, MaterializationState::Materialized));
        QVERIFY(!tree->addDiagnostic(AnalysisNodeId(999), cancelled));
        QVERIFY(tree->transition(tree->rootId(), MaterializationState::Materialized));
        QVERIFY(tree->isFullyMaterialized());
        QVERIFY(!tree->hasPartialResults());
    }

    void findsTheMostSpecificMaterializedNodeAtASourceBit() {
        auto tree = AnalysisTree::create(QStringLiteral("root"));
        QVERIFY(tree.has_value());

        AnalysisNodeSpec region;
        region.kind = AnalysisNodeKind::Region;
        region.name = QStringLiteral("nal_unit[0]");
        region.state = MaterializationState::Indexing;
        region.location = testLocation(8, 32);
        const auto regionId = tree->appendChild(tree->rootId(), region);
        QVERIFY(regionId.has_value());

        AnalysisNodeSpec field;
        field.kind = AnalysisNodeKind::SyntaxField;
        field.name = QStringLiteral("nal_unit_type");
        field.state = MaterializationState::Materialized;
        field.location = testLocation(12, 5);
        const auto fieldId = tree->appendChild(*regionId, field);
        QVERIFY(fieldId.has_value());
        QVERIFY(tree->transition(*regionId, MaterializationState::Materialized));

        QCOMPARE(tree->mostSpecificMaterializedNodeAt(SourceBitAddress(13)), fieldId);
        QCOMPARE(tree->mostSpecificMaterializedNodeAt(SourceBitAddress(9)), regionId);
        QVERIFY(!tree->mostSpecificMaterializedNodeAt(SourceBitAddress(40)).has_value());
    }

    void resolvesDisjointSourceSpansWithHalfOpenBoundaries() {
        auto tree = AnalysisTree::create(QStringLiteral("root"));
        QVERIFY(tree.has_value());

        AnalysisNodeSpec field;
        field.kind = AnalysisNodeKind::SyntaxField;
        field.name = QStringLiteral("mapped_field");
        field.state = MaterializationState::Materialized;
        field.location = testLocationFromSpans({{8, 4}, {20, 4}});
        QVERIFY(field.location.has_value());
        const auto fieldId = tree->appendChild(tree->rootId(), field);
        QVERIFY(fieldId.has_value());

        QCOMPARE(tree->mostSpecificMaterializedNodeAt(SourceBitAddress(8)), fieldId);
        QCOMPARE(tree->mostSpecificMaterializedNodeAt(SourceBitAddress(11)), fieldId);
        QVERIFY(!tree->mostSpecificMaterializedNodeAt(SourceBitAddress(12)).has_value());
        QCOMPARE(tree->mostSpecificMaterializedNodeAt(SourceBitAddress(20)), fieldId);
        QVERIFY(!tree->mostSpecificMaterializedNodeAt(SourceBitAddress(24)).has_value());
    }

    void usesCoverageAndNodeIdAsDeterministicSameDepthTieBreaks() {
        auto tree = AnalysisTree::create(QStringLiteral("root"));
        QVERIFY(tree.has_value());

        const auto appendField = [&tree](const QString& name, quint64 start, quint64 length) {
            AnalysisNodeSpec field;
            field.kind = AnalysisNodeKind::SyntaxField;
            field.name = name;
            field.state = MaterializationState::Materialized;
            field.location = testLocation(start, length);
            return tree->appendChild(tree->rootId(), std::move(field));
        };

        const auto widerId = appendField(QStringLiteral("wider"), 8, 16);
        const auto firstNarrowId = appendField(QStringLiteral("first_narrow"), 10, 4);
        const auto secondNarrowId = appendField(QStringLiteral("second_narrow"), 10, 4);
        QVERIFY(widerId.has_value());
        QVERIFY(firstNarrowId.has_value());
        QVERIFY(secondNarrowId.has_value());

        QCOMPARE(tree->mostSpecificMaterializedNodeAt(SourceBitAddress(10)), firstNarrowId);
    }

    void prioritizesDepthAndIgnoresNonMaterializedCandidates() {
        auto tree = AnalysisTree::create(QStringLiteral("root"));
        QVERIFY(tree.has_value());

        AnalysisNodeSpec shallow;
        shallow.kind = AnalysisNodeKind::SyntaxField;
        shallow.name = QStringLiteral("shallow");
        shallow.state = MaterializationState::Materialized;
        shallow.location = testLocation(10, 1);
        const auto shallowId = tree->appendChild(tree->rootId(), shallow);
        QVERIFY(shallowId.has_value());

        AnalysisNodeSpec parent;
        parent.kind = AnalysisNodeKind::Region;
        parent.name = QStringLiteral("parent");
        parent.state = MaterializationState::Indexing;
        parent.location = testLocation(0, 32);
        const auto parentId = tree->appendChild(tree->rootId(), parent);
        QVERIFY(parentId.has_value());

        AnalysisNodeSpec deep;
        deep.kind = AnalysisNodeKind::SyntaxField;
        deep.name = QStringLiteral("deep_but_wide");
        deep.state = MaterializationState::Materialized;
        deep.location = testLocation(0, 32);
        const auto deepId = tree->appendChild(*parentId, deep);
        QVERIFY(deepId.has_value());
        QVERIFY(tree->transition(*parentId, MaterializationState::Materialized));

        AnalysisNodeSpec pending;
        pending.kind = AnalysisNodeKind::Region;
        pending.name = QStringLiteral("pending");
        pending.state = MaterializationState::Indexing;
        pending.location = testLocation(48, 1);
        QVERIFY(tree->appendChild(tree->rootId(), pending).has_value());

        QCOMPARE(tree->mostSpecificMaterializedNodeAt(SourceBitAddress(10)), deepId);
        QVERIFY(!tree->mostSpecificMaterializedNodeAt(SourceBitAddress(48)).has_value());
    }
};

QTEST_GUILESS_MAIN(AnalysisModelTest)

#include "analysis_model_test.moc"
