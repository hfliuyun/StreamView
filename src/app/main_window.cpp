#include "main_window.h"

#include "analysis_tree_model.h"
#include "raw_data_view.h"

#include <QAction>
#include <QDockWidget>
#include <QFileDialog>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QKeySequence>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QTreeView>

namespace streamview::app {

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(tr("StreamView"));
    resize(1280, 800);

    rawDataView_ = new RawDataView(this);
    setCentralWidget(rawDataView_);

    setupDocks();
    setupMenus();
    connect(rawDataView_, &RawDataView::sourceBitSelected,
            this, &MainWindow::selectSourceBit);

    statusBar()->showMessage(tr("Ready"));
}

void MainWindow::setupMenus() {
    auto* fileMenu = menuBar()->addMenu(tr("&File"));
    auto* openAction = fileMenu->addAction(tr("&Open..."));
    openAction->setShortcut(QKeySequence::Open);
    connect(openAction, &QAction::triggered, this, &MainWindow::openFile);
}

void MainWindow::setupDocks() {
    // --- Analysis Tree dock (left) ---
    auto* analysisDock = new QDockWidget(tr("Analysis Tree"), this);
    analysisDock->setObjectName(QStringLiteral("analysisTreeDock"));

    analysisTreeView_ = new QTreeView(analysisDock);
    analysisTreeView_->setObjectName(QStringLiteral("analysisTreeView"));
    analysisModel_ = new AnalysisTreeModel(this);
    analysisTreeView_->setModel(analysisModel_);
    analysisTreeView_->setAlternatingRowColors(true);
    analysisTreeView_->setSelectionBehavior(QAbstractItemView::SelectRows);
    analysisTreeView_->setSelectionMode(QAbstractItemView::SingleSelection);
    analysisTreeView_->setUniformRowHeights(true);
    analysisTreeView_->header()->setStretchLastSection(true);
    connect(analysisTreeView_->selectionModel(), &QItemSelectionModel::currentChanged,
            this, [this](const QModelIndex& current, const QModelIndex&) {
                selectAnalysisNode(current);
            });
    analysisDock->setWidget(analysisTreeView_);
    addDockWidget(Qt::LeftDockWidgetArea, analysisDock);

    // --- Field Inspector dock (right) ---
    auto* inspectorDock = new QDockWidget(tr("Field Inspector"), this);
    inspectorDock->setObjectName(QStringLiteral("fieldInspectorDock"));
    auto* inspectorPlaceholder = new QLabel(tr("No field selected."), inspectorDock);
    inspectorPlaceholder->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    inspectorDock->setWidget(inspectorPlaceholder);
    addDockWidget(Qt::RightDockWidgetArea, inspectorDock);
}

void MainWindow::openFile() {
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Open Media File"), QString(),
        tr("H.264 Annex B (*.264 *.h264 *.bin);;All Files (*)"));

    if (path.isEmpty()) {
        return;
    }

    QString errorMessage;
    if (!openMediaSource(path, &errorMessage)) {
        QMessageBox::warning(this, tr("Cannot Open File"),
                             tr("Could not open %1:\n%2").arg(path, errorMessage));
    }
}

bool MainWindow::openMediaSource(const QString& path, QString* errorMessage) {
    QString candidateError;
    auto candidate = AnalysisSession::openFile(path, &candidateError);
    if (!candidate) {
        if (errorMessage != nullptr) {
            *errorMessage = candidateError;
        }
        return false;
    }

    // Analysis remains synchronous until the M2 worker/publishing slice.
    while (!candidate->finished()) {
        (void)candidate->analyzeBatch();
    }

    clearSourceSelection();
    rawDataView_->clear();
    analysisModel_->clear();
    session_ = std::move(candidate);

    QString rawError;
    const bool rawLoaded = rawDataView_->setSource(
        &session_->source(), session_->initialPage(), &rawError);
    analysisModel_->resetFromTree(session_->tree());

    // Auto-expand the first two levels for visibility.
    analysisTreeView_->expandToDepth(1);

    // Resize columns to content.
    for (int i = 0; i < AnalysisTreeModel::ColumnCount; ++i) {
        analysisTreeView_->resizeColumnToContents(i);
    }

    if (!rawLoaded) {
        statusBar()->showMessage(tr("Opened %1, but raw data could not be read: %2")
                                     .arg(session_->identity(), rawError));
    } else if (session_->tree().isFullyMaterialized()) {
        statusBar()->showMessage(
            tr("Analysis complete: %1 nodes").arg(session_->tree().nodeCount()));
    } else {
        statusBar()->showMessage(
            tr("Analysis finished with partial results: %1 nodes")
                .arg(session_->tree().nodeCount()));
    }

    if (errorMessage != nullptr) {
        errorMessage->clear();
    }
    return true;
}

QString MainWindow::currentSourceIdentity() const {
    return session_ ? session_->identity() : QString();
}

void MainWindow::selectAnalysisNode(const QModelIndex& current) {
    if (!session_) {
        clearSourceSelection();
        return;
    }
    const auto nodeId = analysisModel_->nodeIdAt(current);
    const auto node = nodeId ? session_->tree().node(*nodeId) : std::nullopt;
    if (!node || !node->location() || node->location()->sourceSpans().empty()) {
        clearSourceSelection();
        return;
    }

    SourceSelection selection;
    selection.sourceIdentity = session_->identity();
    selection.sourceSpans = node->location()->sourceSpans();
    setSourceSelection(std::move(selection));
}

void MainWindow::selectSourceBit(quint64 absoluteBitOffset) {
    if (!session_) {
        return;
    }
    const auto selectedSpan =
        core::SourceSpan::create(core::SourceBitAddress(absoluteBitOffset), 1);
    if (!selectedSpan) {
        return;
    }

    SourceSelection selection;
    selection.sourceIdentity = session_->identity();
    selection.sourceSpans = {*selectedSpan};
    setSourceSelection(std::move(selection));

    const auto nodeId = session_->tree().mostSpecificMaterializedNodeAt(
        core::SourceBitAddress(absoluteBitOffset));
    const QModelIndex nodeIndex =
        nodeId ? analysisModel_->indexForNodeId(*nodeId) : QModelIndex{};
    QSignalBlocker blocker(analysisTreeView_->selectionModel());
    if (!nodeIndex.isValid()) {
        analysisTreeView_->selectionModel()->clear();
        return;
    }

    for (QModelIndex ancestor = nodeIndex.parent(); ancestor.isValid();
         ancestor = ancestor.parent()) {
        analysisTreeView_->expand(ancestor);
    }
    analysisTreeView_->selectionModel()->setCurrentIndex(
        nodeIndex, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    analysisTreeView_->scrollTo(nodeIndex, QAbstractItemView::PositionAtCenter);
}

void MainWindow::setSourceSelection(SourceSelection selection) {
    if (!session_ || selection.isEmpty() || selection.sourceIdentity != session_->identity()) {
        clearSourceSelection();
        return;
    }
    sourceSelection_ = std::move(selection);
    rawDataView_->setSourceSelection(sourceSelection_.sourceSpans);
}

void MainWindow::clearSourceSelection() {
    sourceSelection_ = {};
    rawDataView_->setSourceSelection({});
}

} // namespace streamview::app
