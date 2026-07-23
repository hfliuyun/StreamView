#include "field_inspector.h"

#include <QFormLayout>
#include <QLabel>
#include <QSizePolicy>

namespace streamview::app {

namespace {

QLabel* makeValueLabel(QWidget* parent, const QString& objectName) {
    auto* label = new QLabel(parent);
    label->setObjectName(objectName);
    label->setWordWrap(true);
    label->setTextFormat(Qt::PlainText);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    return label;
}

} // namespace

FieldInspector::FieldInspector(QWidget* parent) : QWidget(parent) {
    auto* form = new QFormLayout(this);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);

    name_ = makeValueLabel(this, QStringLiteral("fieldInspectorName"));
    value_ = makeValueLabel(this, QStringLiteral("fieldInspectorValue"));
    type_ = makeValueLabel(this, QStringLiteral("fieldInspectorType"));
    width_ = makeValueLabel(this, QStringLiteral("fieldInspectorWidth"));
    sourceSpans_ = makeValueLabel(this, QStringLiteral("fieldInspectorSourceSpans"));
    logicalRange_ = makeValueLabel(this, QStringLiteral("fieldInspectorLogicalRange"));
    description_ = makeValueLabel(this, QStringLiteral("fieldInspectorDescription"));
    specification_ = makeValueLabel(this, QStringLiteral("fieldInspectorSpecification"));
    diagnostics_ = makeValueLabel(this, QStringLiteral("fieldInspectorDiagnostics"));

    form->addRow(tr("Field"), name_);
    form->addRow(tr("Value"), value_);
    form->addRow(tr("Type"), type_);
    form->addRow(tr("Width"), width_);
    form->addRow(tr("Source spans"), sourceSpans_);
    form->addRow(tr("Logical range"), logicalRange_);
    form->addRow(tr("Description"), description_);
    form->addRow(tr("Specification"), specification_);
    form->addRow(tr("Diagnostics"), diagnostics_);
    clear();
}

void FieldInspector::clear() {
    name_->setText(tr("No field selected."));
    value_->setText(QStringLiteral("-"));
    type_->setText(QStringLiteral("-"));
    width_->setText(QStringLiteral("-"));
    sourceSpans_->setText(QStringLiteral("-"));
    logicalRange_->setText(QStringLiteral("-"));
    description_->setText(QStringLiteral("-"));
    specification_->setText(QStringLiteral("-"));
    diagnostics_->setText(QStringLiteral("-"));
}

void FieldInspector::setNode(const core::AnalysisNode& node) {
    name_->setText(node.name());
    value_->setText(node.value().isValid() ? node.value().toString() : QStringLiteral("-"));
    type_->setText(node.metadata().typeName.isEmpty() ? kindName(node.kind())
                                                       : node.metadata().typeName);
    if (node.location()) {
        width_->setText(QString::number(node.location()->logicalRange().bitLength()) +
                        QStringLiteral(" bits"));
    } else {
        width_->setText(QStringLiteral("-"));
    }
    sourceSpans_->setText(formatSourceSpans(node));
    logicalRange_->setText(formatLogicalRange(node));
    description_->setText(node.metadata().description.isEmpty() ? QStringLiteral("-")
                                                                 : node.metadata().description);
    specification_->setText(formatSpecification(node));
    diagnostics_->setText(formatDiagnostics(node));
}

QString FieldInspector::formatSourceSpans(const core::AnalysisNode& node) {
    if (!node.location() || node.location()->sourceSpans().empty()) {
        return QStringLiteral("-");
    }
    QStringList values;
    for (const auto& span : node.location()->sourceSpans()) {
        values.append(QStringLiteral("[%1, %2)")
                          .arg(span.start().absoluteBitOffset())
                          .arg(span.endExclusive().absoluteBitOffset()));
    }
    return values.join(QStringLiteral(", "));
}

QString FieldInspector::formatLogicalRange(const core::AnalysisNode& node) {
    if (!node.location()) {
        return QStringLiteral("-");
    }
    const auto& range = node.location()->logicalRange();
    return QStringLiteral("view %1: [%2, %3)")
        .arg(range.start().viewId().value())
        .arg(range.start().bitOffset())
        .arg(range.endOffsetExclusive());
}

QString FieldInspector::formatSpecification(const core::AnalysisNode& node) {
    if (!node.metadata().specification) {
        return QStringLiteral("-");
    }
    const auto& specification = *node.metadata().specification;
    if (specification.standard.isEmpty()) {
        return specification.clause.isEmpty() ? QStringLiteral("-") : specification.clause;
    }
    return specification.clause.isEmpty()
               ? specification.standard
               : specification.standard + QStringLiteral(" ") + specification.clause;
}

QString FieldInspector::formatDiagnostics(const core::AnalysisNode& node) {
    if (node.diagnostics().empty()) {
        return QStringLiteral("-");
    }
    QStringList values;
    for (const auto& diagnostic : node.diagnostics()) {
        QString value = QStringLiteral("%1: %2")
                            .arg(diagnosticSeverityName(diagnostic.severity))
                            .arg(diagnostic.message);
        if (!diagnostic.fieldPath.isEmpty()) {
            value += QStringLiteral(" [") + diagnostic.fieldPath + QLatin1Char(']');
        }
        value += QStringLiteral(" (") + diagnosticCodeName(diagnostic.code) + QLatin1Char(')');
        values.append(value);
    }
    return values.join(QStringLiteral("\n"));
}

QString FieldInspector::kindName(core::AnalysisNodeKind kind) {
    switch (kind) {
    case core::AnalysisNodeKind::Root:
        return QStringLiteral("root");
    case core::AnalysisNodeKind::Structure:
        return QStringLiteral("struct");
    case core::AnalysisNodeKind::SyntaxField:
        return QStringLiteral("bits");
    case core::AnalysisNodeKind::ComputedField:
        return QStringLiteral("computed");
    case core::AnalysisNodeKind::CompressedPayload:
        return QStringLiteral("payload");
    case core::AnalysisNodeKind::Region:
        return QStringLiteral("region");
    }
    return QStringLiteral("-");
}

QString FieldInspector::diagnosticCodeName(core::DiagnosticCode code) {
    switch (code) {
    case core::DiagnosticCode::TruncatedSource:
        return QStringLiteral("truncated-source");
    case core::DiagnosticCode::InvalidSyntax:
        return QStringLiteral("invalid-syntax");
    case core::DiagnosticCode::UnsupportedSyntax:
        return QStringLiteral("unsupported-syntax");
    case core::DiagnosticCode::Cancelled:
        return QStringLiteral("cancelled");
    case core::DiagnosticCode::SourceError:
        return QStringLiteral("source-error");
    case core::DiagnosticCode::ResourceLimit:
        return QStringLiteral("resource-limit");
    case core::DiagnosticCode::DependencyUnavailable:
        return QStringLiteral("dependency-unavailable");
    }
    return QStringLiteral("diagnostic");
}

QString FieldInspector::diagnosticSeverityName(core::DiagnosticSeverity severity) {
    switch (severity) {
    case core::DiagnosticSeverity::Info:
        return QStringLiteral("info");
    case core::DiagnosticSeverity::Warning:
        return QStringLiteral("warning");
    case core::DiagnosticSeverity::Error:
        return QStringLiteral("error");
    }
    return QStringLiteral("diagnostic");
}

} // namespace streamview::app
