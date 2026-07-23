#pragma once

#include <streamview/core/analysis_model.h>

#include <QWidget>

class QLabel;

namespace streamview::app {

/// Read-only presentation of the currently selected analysis node.
class FieldInspector final : public QWidget {
    Q_OBJECT

public:
    explicit FieldInspector(QWidget* parent = nullptr);

    void clear();
    void setNode(const core::AnalysisNode& node);

private:
    static QString formatSourceSpans(const core::AnalysisNode& node);
    static QString formatLogicalRange(const core::AnalysisNode& node);
    static QString formatSpecification(const core::AnalysisNode& node);
    static QString formatDiagnostics(const core::AnalysisNode& node);
    static QString kindName(core::AnalysisNodeKind kind);
    static QString diagnosticCodeName(core::DiagnosticCode code);
    static QString diagnosticSeverityName(core::DiagnosticSeverity severity);

    QLabel* name_ = nullptr;
    QLabel* value_ = nullptr;
    QLabel* type_ = nullptr;
    QLabel* width_ = nullptr;
    QLabel* sourceSpans_ = nullptr;
    QLabel* logicalRange_ = nullptr;
    QLabel* description_ = nullptr;
    QLabel* specification_ = nullptr;
    QLabel* diagnostics_ = nullptr;
};

} // namespace streamview::app
