#include "diagnostics_summary_dock.h"

#include <streamview/core/analysis_model.h>
#include <streamview/core/coordinates.h>

#include <QComboBox>
#include <QLabel>
#include <QSignalSpy>
#include <QTableWidget>
#include <QTest>

using streamview::app::DiagnosticsSummaryDock;
using streamview::core::AnalysisNodeId;
using streamview::core::AnalysisNodeKind;
using streamview::core::AnalysisNodeSpec;
using streamview::core::AnalysisTree;
using streamview::core::DiagnosticCode;
using streamview::core::DiagnosticSeverity;
using streamview::core::FieldLocation;
using streamview::core::LogicalBitAddress;
using streamview::core::LogicalRange;
using streamview::core::LogicalViewId;
using streamview::core::MaterializationState;
using streamview::core::ParseDiagnostic;
using streamview::core::SourceBitAddress;
using streamview::core::SourceMapping;
using streamview::core::SourceSpan;

namespace {

std::optional<FieldLocation> makeLocation(quint64 byteOffset, quint64 byteLength) {
    const auto span = SourceSpan::create(SourceBitAddress(byteOffset * 8), byteLength * 8);
    if (!span) {
        return std::nullopt;
    }
    const auto mapping = SourceMapping::create(LogicalViewId(1), {*span});
    if (!mapping) {
        return std::nullopt;
    }
    const auto range = LogicalRange::create(LogicalBitAddress(LogicalViewId(1), 0), byteLength * 8);
    return range ? mapping->locate(*range) : std::nullopt;
}

} // namespace

class DiagnosticsSummaryDockTest final : public QObject {
    Q_OBJECT

private slots:
    void initialState() {
        DiagnosticsSummaryDock dock;
        QCOMPARE(dock.displayedCount(), 0);
        QVERIFY(dock.allEntries().empty());

        auto* label = dock.findChild<QLabel*>(QStringLiteral("diagnosticsSummaryLabel"));
        QVERIFY(label != nullptr);
        QCOMPARE(label->text(), QStringLiteral("Total: 0"));

        auto* table = dock.findChild<QTableWidget*>(QStringLiteral("diagnosticsTableWidget"));
        QVERIFY(table != nullptr);
        QCOMPARE(table->rowCount(), 0);
        QCOMPARE(table->columnCount(), 4);
    }

    void populateFromTree() {
        DiagnosticsSummaryDock dock;
        auto treeOpt = AnalysisTree::create(QStringLiteral("stream"));
        QVERIFY(treeOpt.has_value());
        auto& tree = *treeOpt;

        AnalysisNodeSpec node1Spec;
        node1Spec.kind = AnalysisNodeKind::Region;
        node1Spec.name = QStringLiteral("nal_unit_0");
        node1Spec.state = MaterializationState::Indexing;
        node1Spec.location = makeLocation(0, 10);
        auto node1Id = tree.appendChild(tree.rootId(), std::move(node1Spec));
        QVERIFY(node1Id.has_value());

        ParseDiagnostic diag1;
        diag1.code = DiagnosticCode::InvalidSyntax;
        diag1.severity = DiagnosticSeverity::Error;
        diag1.message = QStringLiteral("Invalid NAL unit type");
        diag1.fieldPath = QStringLiteral("nal_unit_type");
        diag1.location = makeLocation(1, 1);
        QVERIFY(tree.addDiagnostic(*node1Id, diag1));

        AnalysisNodeSpec node2Spec;
        node2Spec.kind = AnalysisNodeKind::SyntaxField;
        node2Spec.name = QStringLiteral("pic_parameter_set_id");
        node2Spec.location = makeLocation(2, 2);
        auto node2Id = tree.appendChild(*node1Id, std::move(node2Spec));
        QVERIFY(node2Id.has_value());

        ParseDiagnostic diag2;
        diag2.code = DiagnosticCode::UnsupportedSyntax;
        diag2.severity = DiagnosticSeverity::Warning;
        diag2.message = QStringLiteral("Deprecated parameter used");
        QVERIFY(tree.addDiagnostic(*node2Id, diag2));

        ParseDiagnostic diag3;
        diag3.code = DiagnosticCode::InvalidSyntax;
        diag3.severity = DiagnosticSeverity::Info;
        diag3.message = QStringLiteral("Reference PPS 0 inferred");
        diag3.location = makeLocation(2, 2);
        QVERIFY(tree.addDiagnostic(*node2Id, diag3));

        dock.setTree(&tree);

        QCOMPARE(dock.displayedCount(), 3);
        QCOMPARE(dock.allEntries().size(), std::size_t{3});

        auto* label = dock.findChild<QLabel*>(QStringLiteral("diagnosticsSummaryLabel"));
        QVERIFY(label != nullptr);
        QCOMPARE(label->text(), QStringLiteral("Total: 3 (Errors: 1, Warnings: 1, Info: 1)"));

        auto* table = dock.findChild<QTableWidget*>(QStringLiteral("diagnosticsTableWidget"));
        QVERIFY(table != nullptr);
        QCOMPARE(table->rowCount(), 3);

        // Check row 0 (diag1)
        QCOMPARE(table->item(0, 0)->text(), QStringLiteral("Error"));
        QCOMPARE(table->item(0, 1)->text(), QStringLiteral("Invalid NAL unit type"));
        QCOMPARE(table->item(0, 2)->text(), QStringLiteral("nal_unit_type"));
        QCOMPARE(table->item(0, 3)->text(), QStringLiteral("0x00000001 (8..16 bit)"));

        // Check row 1 (diag2)
        QCOMPARE(table->item(1, 0)->text(), QStringLiteral("Warning"));
        QCOMPARE(table->item(1, 1)->text(), QStringLiteral("Deprecated parameter used"));
        QCOMPARE(table->item(1, 2)->text(), QStringLiteral("pic_parameter_set_id"));
        QCOMPARE(table->item(1, 3)->text(), QStringLiteral("-"));

        // Check row 2 (diag3)
        QCOMPARE(table->item(2, 0)->text(), QStringLiteral("Info"));
        QCOMPARE(table->item(2, 1)->text(), QStringLiteral("Reference PPS 0 inferred"));
        QCOMPARE(table->item(2, 2)->text(), QStringLiteral("pic_parameter_set_id"));
        QCOMPARE(table->item(2, 3)->text(), QStringLiteral("0x00000002 (16..32 bit)"));
    }

    void severityFiltering() {
        DiagnosticsSummaryDock dock;
        auto treeOpt = AnalysisTree::create(QStringLiteral("stream"));
        QVERIFY(treeOpt.has_value());
        auto& tree = *treeOpt;

        AnalysisNodeSpec nodeSpec;
        nodeSpec.kind = AnalysisNodeKind::Region;
        nodeSpec.name = QStringLiteral("header");
        auto nodeId = tree.appendChild(tree.rootId(), std::move(nodeSpec));
        QVERIFY(nodeId.has_value());

        ParseDiagnostic err;
        err.severity = DiagnosticSeverity::Error;
        err.message = QStringLiteral("Fatal parse error");
        QVERIFY(tree.addDiagnostic(*nodeId, err));

        ParseDiagnostic warn;
        warn.severity = DiagnosticSeverity::Warning;
        warn.message = QStringLiteral("Warning note");
        QVERIFY(tree.addDiagnostic(*nodeId, warn));

        ParseDiagnostic info;
        info.severity = DiagnosticSeverity::Info;
        info.message = QStringLiteral("Info note");
        QVERIFY(tree.addDiagnostic(*nodeId, info));

        dock.setTree(&tree);
        QCOMPARE(dock.displayedCount(), 3);

        auto* filterCombo = dock.findChild<QComboBox*>(QStringLiteral("severityFilterComboBox"));
        auto* table = dock.findChild<QTableWidget*>(QStringLiteral("diagnosticsTableWidget"));
        QVERIFY(filterCombo != nullptr);
        QVERIFY(table != nullptr);

        // Filter 1: Errors Only
        filterCombo->setCurrentIndex(1);
        QCOMPARE(dock.displayedCount(), 1);
        QCOMPARE(table->rowCount(), 1);
        QCOMPARE(table->item(0, 0)->text(), QStringLiteral("Error"));

        // Filter 2: Warnings & Errors
        filterCombo->setCurrentIndex(2);
        QCOMPARE(dock.displayedCount(), 2);
        QCOMPARE(table->rowCount(), 2);

        // Filter 3: Info
        filterCombo->setCurrentIndex(3);
        QCOMPARE(dock.displayedCount(), 1);
        QCOMPARE(table->rowCount(), 1);
        QCOMPARE(table->item(0, 0)->text(), QStringLiteral("Info"));

        // Filter 0: All
        filterCombo->setCurrentIndex(0);
        QCOMPARE(dock.displayedCount(), 3);
        QCOMPARE(table->rowCount(), 3);
    }

    void selectionForwardAndReverse() {
        DiagnosticsSummaryDock dock;
        auto treeOpt = AnalysisTree::create(QStringLiteral("stream"));
        QVERIFY(treeOpt.has_value());
        auto& tree = *treeOpt;

        AnalysisNodeSpec n1Spec;
        n1Spec.kind = AnalysisNodeKind::Region;
        n1Spec.name = QStringLiteral("n1");
        auto n1Id = tree.appendChild(tree.rootId(), std::move(n1Spec));
        QVERIFY(n1Id.has_value());

        ParseDiagnostic d1;
        d1.severity = DiagnosticSeverity::Error;
        d1.message = QStringLiteral("err1");
        d1.location = makeLocation(10, 4);
        QVERIFY(tree.addDiagnostic(*n1Id, d1));

        AnalysisNodeSpec n2Spec;
        n2Spec.kind = AnalysisNodeKind::Region;
        n2Spec.name = QStringLiteral("n2");
        auto n2Id = tree.appendChild(tree.rootId(), std::move(n2Spec));
        QVERIFY(n2Id.has_value());

        ParseDiagnostic d2;
        d2.severity = DiagnosticSeverity::Warning;
        d2.message = QStringLiteral("warn2");
        QVERIFY(tree.addDiagnostic(*n2Id, d2));

        dock.setTree(&tree);

        auto* table = dock.findChild<QTableWidget*>(QStringLiteral("diagnosticsTableWidget"));
        QVERIFY(table != nullptr);

        // Test forward navigation signal emission
        QSignalSpy spy(&dock, &DiagnosticsSummaryDock::diagnosticSelected);
        table->selectRow(0);

        QCOMPARE(spy.count(), 1);
        const auto emittedNodeId = spy.at(0).at(0).value<AnalysisNodeId>();
        QCOMPARE(emittedNodeId, *n1Id);

        // Test reverse navigation: selectDiagnosticForNode should not emit signal
        dock.selectDiagnosticForNode(*n2Id);
        QCOMPARE(table->currentRow(), 1);
        QCOMPARE(spy.count(), 1); // No new signal emitted!

        // Select non-existent node
        dock.selectDiagnosticForNode(AnalysisNodeId(999));
        QCOMPARE(table->selectedItems().count(), 0);
    }

    void clearResetsState() {
        DiagnosticsSummaryDock dock;
        auto treeOpt = AnalysisTree::create(QStringLiteral("stream"));
        QVERIFY(treeOpt.has_value());
        auto& tree = *treeOpt;

        AnalysisNodeSpec nodeSpec;
        nodeSpec.kind = AnalysisNodeKind::Region;
        nodeSpec.name = QStringLiteral("header");
        auto nodeId = tree.appendChild(tree.rootId(), std::move(nodeSpec));
        QVERIFY(nodeId.has_value());

        ParseDiagnostic err;
        err.severity = DiagnosticSeverity::Error;
        err.message = QStringLiteral("Fatal parse error");
        QVERIFY(tree.addDiagnostic(*nodeId, err));

        dock.setTree(&tree);
        QCOMPARE(dock.displayedCount(), 1);

        dock.clear();
        QCOMPARE(dock.displayedCount(), 0);
        QVERIFY(dock.allEntries().empty());

        auto* label = dock.findChild<QLabel*>(QStringLiteral("diagnosticsSummaryLabel"));
        QVERIFY(label != nullptr);
        QCOMPARE(label->text(), QStringLiteral("Total: 0"));

        auto* table = dock.findChild<QTableWidget*>(QStringLiteral("diagnosticsTableWidget"));
        QVERIFY(table != nullptr);
        QCOMPARE(table->rowCount(), 0);
    }
};

QTEST_MAIN(DiagnosticsSummaryDockTest)

#include "diagnostics_summary_dock_test.moc"
