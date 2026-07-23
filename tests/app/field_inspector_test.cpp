#include "field_inspector.h"

#include <streamview/core/analysis_model.h>
#include <streamview/core/coordinates.h>

#include <QLabel>
#include <QTest>

using streamview::app::FieldInspector;
using streamview::core::AnalysisNodeKind;
using streamview::core::AnalysisNodeSpec;
using streamview::core::AnalysisTree;
using streamview::core::DiagnosticCode;
using streamview::core::DiagnosticSeverity;
using streamview::core::LogicalBitAddress;
using streamview::core::LogicalRange;
using streamview::core::LogicalViewId;
using streamview::core::MaterializationState;
using streamview::core::SourceBitAddress;
using streamview::core::SourceMapping;
using streamview::core::SourceSpan;

namespace {

std::optional<streamview::core::FieldLocation> multiSpanLocation() {
    const auto first = SourceSpan::create(SourceBitAddress(8), 8);
    const auto second = SourceSpan::create(SourceBitAddress(32), 8);
    if (!first || !second) {
        return std::nullopt;
    }
    const auto mapping = SourceMapping::create(LogicalViewId(7), {*first, *second});
    const auto range = LogicalRange::create(LogicalBitAddress(LogicalViewId(7), 0), 16);
    return mapping && range ? mapping->locate(*range) : std::nullopt;
}

QLabel* label(FieldInspector& inspector, const QString& objectName) {
    return inspector.findChild<QLabel*>(objectName);
}

} // namespace

class FieldInspectorTest final : public QObject {
    Q_OBJECT

private slots:
    void displaysCoordinatesSpecificationAndDiagnostics() {
        auto tree = AnalysisTree::create(QStringLiteral("root"));
        QVERIFY(tree.has_value());

        AnalysisNodeSpec field;
        field.kind = AnalysisNodeKind::SyntaxField;
        field.name = QStringLiteral("mapped_field");
        field.state = MaterializationState::Invalid;
        field.value = QStringLiteral("value");
        field.location = multiSpanLocation();
        field.metadata.typeName = QStringLiteral("bits");
        field.metadata.description = QStringLiteral("A mapped field.");
        field.metadata.specification =
            streamview::core::AnalysisSpecification{QStringLiteral("Test Standard"),
                                                    QStringLiteral("4.2")};
        const auto fieldId = tree->appendChild(tree->rootId(), std::move(field));
        QVERIFY(fieldId.has_value());

        streamview::core::ParseDiagnostic diagnostic;
        diagnostic.code = DiagnosticCode::InvalidSyntax;
        diagnostic.severity = DiagnosticSeverity::Error;
        diagnostic.message = QStringLiteral("bad value");
        diagnostic.fieldPath = QStringLiteral("mapped_field");
        diagnostic.location = multiSpanLocation();
        QVERIFY(tree->addDiagnostic(*fieldId, std::move(diagnostic)));

        const auto node = tree->node(*fieldId);
        QVERIFY(node.has_value());
        FieldInspector inspector;
        inspector.setNode(*node);

        QVERIFY(label(inspector, QStringLiteral("fieldInspectorName")) != nullptr);
        QCOMPARE(label(inspector, QStringLiteral("fieldInspectorName"))->text(),
                 QStringLiteral("mapped_field"));
        QCOMPARE(label(inspector, QStringLiteral("fieldInspectorWidth"))->text(),
                 QStringLiteral("16 bits"));
        QCOMPARE(label(inspector, QStringLiteral("fieldInspectorSourceSpans"))->text(),
                 QStringLiteral("[8, 16), [32, 40)"));
        QCOMPARE(label(inspector, QStringLiteral("fieldInspectorLogicalRange"))->text(),
                 QStringLiteral("view 7: [0, 16)"));
        QCOMPARE(label(inspector, QStringLiteral("fieldInspectorSpecification"))->text(),
                 QStringLiteral("Test Standard 4.2"));
        QVERIFY(label(inspector, QStringLiteral("fieldInspectorDiagnostics"))
                    ->text()
                    .contains(QStringLiteral("invalid-syntax")));
        QVERIFY(label(inspector, QStringLiteral("fieldInspectorDiagnostics"))
                    ->text()
                    .contains(QStringLiteral("bad value")));
    }
};

QTEST_MAIN(FieldInspectorTest)

#include "field_inspector_test.moc"
