#include "diagnostics_summary_dock.h"

#include <QColor>
#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QVBoxLayout>

namespace streamview::app {

DiagnosticsSummaryDock::DiagnosticsSummaryDock(QWidget* parent)
    : QDockWidget(tr("Diagnostics"), parent) {
    setObjectName(QStringLiteral("diagnosticsSummaryDock"));

    auto* container = new QWidget(this);
    auto* mainLayout = new QVBoxLayout(container);
    mainLayout->setContentsMargins(4, 4, 4, 4);
    mainLayout->setSpacing(4);

    auto* topLayout = new QHBoxLayout();
    topLayout->setContentsMargins(2, 2, 2, 2);
    topLayout->setSpacing(6);

    summaryLabel_ = new QLabel(tr("Total: 0"), container);
    summaryLabel_->setObjectName(QStringLiteral("diagnosticsSummaryLabel"));
    topLayout->addWidget(summaryLabel_);

    topLayout->addStretch();

    auto* filterLabel = new QLabel(tr("Filter:"), container);
    topLayout->addWidget(filterLabel);

    severityFilterComboBox_ = new QComboBox(container);
    severityFilterComboBox_->setObjectName(QStringLiteral("severityFilterComboBox"));
    severityFilterComboBox_->addItems({
        tr("All Severities"),
        tr("Errors Only"),
        tr("Warnings & Errors"),
        tr("Info")
    });
    connect(severityFilterComboBox_, &QComboBox::currentIndexChanged,
            this, &DiagnosticsSummaryDock::onFilterChanged);
    topLayout->addWidget(severityFilterComboBox_);

    mainLayout->addLayout(topLayout);

    tableWidget_ = new QTableWidget(container);
    tableWidget_->setObjectName(QStringLiteral("diagnosticsTableWidget"));
    tableWidget_->setColumnCount(4);
    tableWidget_->setHorizontalHeaderLabels({
        tr("Severity"),
        tr("Message"),
        tr("Field / Node"),
        tr("Offset / Range")
    });
    tableWidget_->setSelectionBehavior(QAbstractItemView::SelectRows);
    tableWidget_->setSelectionMode(QAbstractItemView::SingleSelection);
    tableWidget_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tableWidget_->horizontalHeader()->setStretchLastSection(false);
    tableWidget_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    tableWidget_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    tableWidget_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    tableWidget_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    tableWidget_->verticalHeader()->setVisible(false);

    connect(tableWidget_, &QTableWidget::itemSelectionChanged,
            this, &DiagnosticsSummaryDock::onTableRowSelected);

    mainLayout->addWidget(tableWidget_, 1);
    setWidget(container);
}

void DiagnosticsSummaryDock::setTree(const core::AnalysisTree* tree) {
    allEntries_.clear();

    if (tree != nullptr) {
        std::vector<core::AnalysisNodeId> queue;
        queue.push_back(tree->rootId());
        std::size_t head = 0;
        while (head < queue.size()) {
            const auto nodeId = queue[head++];
            const auto nodeOpt = tree->node(nodeId);
            if (!nodeOpt.has_value()) {
                continue;
            }
            for (const auto& diag : nodeOpt->diagnostics()) {
                allEntries_.push_back(DiagnosticEntry{
                    nodeId,
                    nodeOpt->name(),
                    diag
                });
            }
            for (const auto childId : nodeOpt->children()) {
                queue.push_back(childId);
            }
        }
    }

    updateSummaryLabel();
    rebuildTable();
}

void DiagnosticsSummaryDock::clear() {
    allEntries_.clear();
    updateSummaryLabel();
    rebuildTable();
}

void DiagnosticsSummaryDock::onFilterChanged(int) {
    rebuildTable();
}

void DiagnosticsSummaryDock::onTableRowSelected() {
    if (isSyncingSelection_) {
        return;
    }
    int row = tableWidget_->currentRow();
    if (row < 0) {
        const auto selected = tableWidget_->selectedItems();
        if (!selected.isEmpty()) {
            row = selected.front()->row();
        }
    }
    if (row < 0 || row >= static_cast<int>(displayedIndices_.size())) {
        return;
    }
    const auto& entry = allEntries_[displayedIndices_[static_cast<std::size_t>(row)]];
    emit diagnosticSelected(entry.nodeId, entry.diagnostic.location);
}

void DiagnosticsSummaryDock::selectDiagnosticForNode(core::AnalysisNodeId nodeId) {
    isSyncingSelection_ = true;
    for (int row = 0; row < static_cast<int>(displayedIndices_.size()); ++row) {
        if (allEntries_[displayedIndices_[static_cast<std::size_t>(row)]].nodeId == nodeId) {
            tableWidget_->selectRow(row);
            if (auto* item = tableWidget_->item(row, 0)) {
                tableWidget_->scrollToItem(item);
            }
            isSyncingSelection_ = false;
            return;
        }
    }
    tableWidget_->clearSelection();
    isSyncingSelection_ = false;
}

void DiagnosticsSummaryDock::updateSummaryLabel() {
    std::size_t errors = 0;
    std::size_t warnings = 0;
    std::size_t info = 0;

    for (const auto& entry : allEntries_) {
        switch (entry.diagnostic.severity) {
        case core::DiagnosticSeverity::Error:
            ++errors;
            break;
        case core::DiagnosticSeverity::Warning:
            ++warnings;
            break;
        case core::DiagnosticSeverity::Info:
            ++info;
            break;
        }
    }

    if (allEntries_.empty()) {
        summaryLabel_->setText(tr("Total: 0"));
    } else {
        summaryLabel_->setText(
            tr("Total: %1 (Errors: %2, Warnings: %3, Info: %4)")
                .arg(allEntries_.size())
                .arg(errors)
                .arg(warnings)
                .arg(info));
    }
}

void DiagnosticsSummaryDock::rebuildTable() {
    const QSignalBlocker blocker(tableWidget_);
    tableWidget_->setRowCount(0);
    displayedIndices_.clear();

    const int filterIndex = severityFilterComboBox_->currentIndex();

    for (std::size_t i = 0; i < allEntries_.size(); ++i) {
        const auto& entry = allEntries_[i];
        bool matches = false;

        switch (filterIndex) {
        case 0: // All Severities
            matches = true;
            break;
        case 1: // Errors Only
            matches = (entry.diagnostic.severity == core::DiagnosticSeverity::Error);
            break;
        case 2: // Warnings & Errors
            matches = (entry.diagnostic.severity == core::DiagnosticSeverity::Error ||
                       entry.diagnostic.severity == core::DiagnosticSeverity::Warning);
            break;
        case 3: // Info
            matches = (entry.diagnostic.severity == core::DiagnosticSeverity::Info);
            break;
        default:
            matches = true;
            break;
        }

        if (!matches) {
            continue;
        }

        displayedIndices_.push_back(i);
        const int row = tableWidget_->rowCount();
        tableWidget_->insertRow(row);

        // Column 0: Severity
        QString sevText;
        QColor sevColor;
        switch (entry.diagnostic.severity) {
        case core::DiagnosticSeverity::Error:
            sevText = tr("Error");
            sevColor = QColor(0xd9, 0x53, 0x4f);
            break;
        case core::DiagnosticSeverity::Warning:
            sevText = tr("Warning");
            sevColor = QColor(0xec, 0x97, 0x1f);
            break;
        case core::DiagnosticSeverity::Info:
            sevText = tr("Info");
            sevColor = QColor(0x02, 0x75, 0xd8);
            break;
        }
        auto* sevItem = new QTableWidgetItem(sevText);
        sevItem->setForeground(sevColor);
        tableWidget_->setItem(row, 0, sevItem);

        // Column 1: Message
        auto* msgItem = new QTableWidgetItem(entry.diagnostic.message);
        tableWidget_->setItem(row, 1, msgItem);

        // Column 2: Field / Node
        const QString fieldOrNode = entry.diagnostic.fieldPath.isEmpty()
                                        ? entry.nodeName
                                        : entry.diagnostic.fieldPath;
        auto* fieldItem = new QTableWidgetItem(fieldOrNode);
        tableWidget_->setItem(row, 2, fieldItem);

        // Column 3: Offset / Range
        QString locText;
        if (entry.diagnostic.location.has_value() &&
            !entry.diagnostic.location->sourceSpans().empty()) {
            const auto span = entry.diagnostic.location->sourceSpans().front();
            locText = QStringLiteral("0x%1 (%2..%3 bit)")
                          .arg(QString::number(span.start().byteOffset(), 16).rightJustified(8, '0'))
                          .arg(span.start().absoluteBitOffset())
                          .arg(span.endExclusive().absoluteBitOffset());
        } else {
            locText = QStringLiteral("-");
        }
        auto* locItem = new QTableWidgetItem(locText);
        tableWidget_->setItem(row, 3, locItem);
    }
}

} // namespace streamview::app
