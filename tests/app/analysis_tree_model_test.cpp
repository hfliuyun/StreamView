#include "analysis_tree_model.h"

#include <streamview/core/analysis_model.h>
#include <streamview/core/coordinates.h>

#include <QTest>

using streamview::app::AnalysisTreeModel;
using streamview::core::AnalysisNodeId;
using streamview::core::AnalysisNodeKind;
using streamview::core::AnalysisNodeSpec;
using streamview::core::AnalysisTree;
using streamview::core::LogicalBitAddress;
using streamview::core::LogicalRange;
using streamview::core::LogicalViewId;
using streamview::core::MaterializationState;
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
    return range ? mapping->locate(*range) : std::nullopt;
}

} // namespace

class AnalysisTreeModelTest final : public QObject {
    Q_OBJECT

private slots:
    void rejectsInvalidIndexesAndFindsMaterializedNodesById() {
        auto tree = AnalysisTree::create(QStringLiteral("root"));
        QVERIFY(tree.has_value());

        AnalysisNodeSpec field;
        field.kind = AnalysisNodeKind::SyntaxField;
        field.name = QStringLiteral("field");
        field.state = MaterializationState::Materialized;
        field.location = testLocation();
        const auto fieldId = tree->appendChild(tree->rootId(), std::move(field));
        QVERIFY(fieldId.has_value());
        QVERIFY(tree->transition(tree->rootId(), MaterializationState::Materialized));

        AnalysisTreeModel model;
        model.resetFromTree(*tree);

        QVERIFY(!model.nodeIdAt({}).has_value());
        QVERIFY(!model.data({}).isValid());
        const QModelIndex fieldIndex = model.indexForNodeId(*fieldId);
        QVERIFY(fieldIndex.isValid());
        QCOMPARE(fieldIndex.data().toString(), QStringLiteral("field"));
        QVERIFY(!model.indexForNodeId(AnalysisNodeId(999)).isValid());

        AnalysisTreeModel otherModel;
        otherModel.resetFromTree(*tree);
        const QModelIndex foreignIndex = otherModel.indexForNodeId(*fieldId);
        QVERIFY(foreignIndex.isValid());
        QVERIFY(!model.nodeIdAt(foreignIndex).has_value());
        QVERIFY(!model.data(foreignIndex).isValid());
    }
};

QTEST_GUILESS_MAIN(AnalysisTreeModelTest)

#include "analysis_tree_model_test.moc"
