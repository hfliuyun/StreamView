#pragma once

#include <streamview/core/analysis_model.h>
#include <streamview/core/coordinates.h>

#include <QDockWidget>
#include <QTableWidget>

#include <memory>
#include <vector>

class QComboBox;
class QLabel;

namespace streamview::app {

struct DiagnosticEntry {
    core::AnalysisNodeId nodeId;
    QString nodeName;
    core::ParseDiagnostic diagnostic;
};

class DiagnosticsSummaryDock : public QDockWidget {
    Q_OBJECT

public:
    explicit DiagnosticsSummaryDock(QWidget* parent = nullptr);

    void setTree(const core::AnalysisTree* tree);
    void clear();

    [[nodiscard]] const std::vector<DiagnosticEntry>& allEntries() const noexcept {
        return allEntries_;
    }
    [[nodiscard]] int displayedCount() const noexcept {
        return static_cast<int>(displayedIndices_.size());
    }

signals:
    void diagnosticSelected(core::AnalysisNodeId nodeId, std::optional<core::FieldLocation> location);

public slots:
    void selectDiagnosticForNode(core::AnalysisNodeId nodeId);

private slots:
    void onFilterChanged(int index);
    void onTableRowSelected();

private:
    void rebuildTable();
    void updateSummaryLabel();

    QLabel* summaryLabel_ = nullptr;
    QComboBox* severityFilterComboBox_ = nullptr;
    QTableWidget* tableWidget_ = nullptr;

    std::vector<DiagnosticEntry> allEntries_;
    std::vector<std::size_t> displayedIndices_;
    bool isSyncingSelection_ = false;
};

} // namespace streamview::app
