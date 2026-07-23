#include "analysis_tree_model.h"

#include <streamview/core/analysis_model.h>
#include <streamview/core/coordinates.h>

#include <QSignalSpy>
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

std::optional<AnalysisNodeId> appendNal(AnalysisTree& tree, const QString& name) {
    AnalysisNodeSpec nal;
    nal.kind = AnalysisNodeKind::Region;
    nal.name = name;
    nal.state = MaterializationState::Indexing;
    nal.location = testLocation();
    const auto nalId = tree.appendChild(tree.rootId(), std::move(nal));
    if (!nalId) {
        return std::nullopt;
    }

    AnalysisNodeSpec field;
    field.kind = AnalysisNodeKind::SyntaxField;
    field.name = name + QStringLiteral(".field");
    field.state = MaterializationState::Materialized;
    field.location = testLocation();
    if (!tree.appendChild(*nalId, std::move(field)) ||
        !tree.transition(*nalId, MaterializationState::Materialized)) {
        return std::nullopt;
    }
    return nalId;
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

    void appendsCompleteSubtreesWithoutResettingExistingIndexes() {
        auto tree = AnalysisTree::create(QStringLiteral("root"));
        QVERIFY(tree.has_value());
        const auto firstId = appendNal(*tree, QStringLiteral("nal_unit[0]"));
        QVERIFY(firstId.has_value());

        AnalysisTreeModel model;
        model.resetFromTree(*tree);
        const QModelIndex firstIndex = model.indexForNodeId(*firstId);
        QVERIFY(firstIndex.isValid());
        QCOMPARE(model.rowCount(), 1);

        QSignalSpy inserted(&model, &QAbstractItemModel::rowsInserted);
        QSignalSpy reset(&model, &QAbstractItemModel::modelReset);
        const auto secondId = appendNal(*tree, QStringLiteral("nal_unit[1]"));
        QVERIFY(secondId.has_value());

        QVERIFY(model.appendTopLevelNodes(*tree, {*secondId}));

        QCOMPARE(inserted.count(), 1);
        QCOMPARE(reset.count(), 0);
        const QList<QVariant> arguments = inserted.takeFirst();
        QVERIFY(!qvariant_cast<QModelIndex>(arguments.at(0)).isValid());
        QCOMPARE(arguments.at(1).toInt(), 1);
        QCOMPARE(arguments.at(2).toInt(), 1);
        QCOMPARE(model.rowCount(), 2);
        QVERIFY(firstIndex.isValid());
        QCOMPARE(model.nodeIdAt(firstIndex), firstId);
        QCOMPARE(firstIndex.data().toString(), QStringLiteral("nal_unit[0]"));

        const QModelIndex secondIndex = model.indexForNodeId(*secondId);
        QVERIFY(secondIndex.isValid());
        QCOMPARE(secondIndex.row(), 1);
        QCOMPARE(model.rowCount(secondIndex), 1);
        QVERIFY(!model.appendTopLevelNodes(*tree, {*secondId}));
    }
};

QTEST_GUILESS_MAIN(AnalysisTreeModelTest)

#include "analysis_tree_model_test.moc"
