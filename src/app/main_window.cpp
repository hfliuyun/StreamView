#include "main_window.h"

#include "analysis_tree_model.h"
#include "field_inspector.h"
#include "raw_data_view.h"

#include <QAction>
#include <QDockWidget>
#include <QFileDialog>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QKeySequence>
#include <QMenuBar>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QTimer>
#include <QTreeView>

namespace streamview::app {

namespace {

constexpr std::size_t kAnalysisBatchRecords = 1;
constexpr quint64 kAnalysisWorkBudget = 64U * 1024U;

} // namespace

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
    fieldInspector_ = new FieldInspector(inspectorDock);
    fieldInspector_->setObjectName(QStringLiteral("fieldInspector"));
    inspectorDock->setWidget(fieldInspector_);
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

    const quint64 generation = ++analysisGeneration_;
    clearSourceSelection();
    {
        const QSignalBlocker blocker(analysisTreeView_->selectionModel());
        analysisTreeView_->selectionModel()->clear();
    }
    fieldInspector_->clear();
    rawDataView_->clear();
    analysisModel_->clear();
    session_ = std::move(candidate);

    rawError_.clear();
    rawLoaded_ = rawDataView_->setSource(
        &session_->source(), session_->initialPage(), &rawError_);
    analysisModel_->resetFromTree(session_->tree());

    // Publish the first batch before returning so the new session is immediately useful.
    advanceAnalysis(generation);

    // Auto-expand the first two levels for visibility.
    analysisTreeView_->expandToDepth(1);

    // Resize columns to content.
    for (int i = 0; i < AnalysisTreeModel::ColumnCount; ++i) {
        analysisTreeView_->resizeColumnToContents(i);
    }

    if (errorMessage != nullptr) {
        errorMessage->clear();
    }
    return true;
}

void MainWindow::advanceAnalysis(quint64 generation) {
    if (generation != analysisGeneration_ || !session_) {
        return;
    }

    const auto batch = session_->analyzeBatch(kAnalysisBatchRecords, kAnalysisWorkBudget);
    if (!batch.nalUnitNodes.empty() &&
        !analysisModel_->appendTopLevelNodes(session_->tree(), batch.nalUnitNodes)) {
        analysisModel_->resetFromTree(session_->tree());
        analysisModel_->updateFromTree(session_->tree());
        ++analysisGeneration_;
        statusBar()->showMessage(tr("Analysis tree publication failed"));
        return;
    }
    analysisModel_->updateFromTree(session_->tree());

    const QModelIndex currentIndex = analysisTreeView_->currentIndex();
    if (currentIndex.isValid()) {
        const auto currentId = analysisModel_->nodeIdAt(currentIndex);
        const auto currentNode = currentId ? session_->tree().node(*currentId) : std::nullopt;
        if (currentNode) {
            fieldInspector_->setNode(*currentNode);
        }
    }

    if (batch.status == rules::H264AnnexBAnalysisStatus::InvalidBatchSize) {
        ++analysisGeneration_;
        statusBar()->showMessage(
            tr("Analysis batch rejected: %1").arg(batch.errorMessage));
        return;
    }
    if (batch.status == rules::H264AnnexBAnalysisStatus::SourceError ||
        batch.status == rules::H264AnnexBAnalysisStatus::Cancelled ||
        batch.status == rules::H264AnnexBAnalysisStatus::ResourceLimit ||
        batch.status == rules::H264AnnexBAnalysisStatus::InvalidRule) {
        publishAnalysisStatus(batch.status, batch.errorMessage);
        return;
    }

    if (!session_->finished()) {
        const quint64 cursor = session_->scanCursor();
        statusBar()->showMessage(
            tr("Analyzing %1: %2/%3 bytes, %4 nodes")
                .arg(session_->identity())
                .arg(cursor)
                .arg(session_->sizeBytes())
                .arg(session_->tree().nodeCount()));
        QTimer::singleShot(0, this, [this, generation] { advanceAnalysis(generation); });
        return;
    }

    publishAnalysisStatus(batch.status, batch.errorMessage);
}

void MainWindow::publishAnalysisStatus(rules::H264AnnexBAnalysisStatus status,
                                       const QString& errorMessage) {
    if (!session_) {
        return;
    }
    if (status == rules::H264AnnexBAnalysisStatus::Cancelled) {
        statusBar()->showMessage(
            tr("Analysis cancelled: %1 nodes").arg(session_->tree().nodeCount()));
        return;
    }
    if (status == rules::H264AnnexBAnalysisStatus::SourceError ||
        status == rules::H264AnnexBAnalysisStatus::ResourceLimit ||
        status == rules::H264AnnexBAnalysisStatus::InvalidRule) {
        const QString detail = errorMessage.isEmpty() ? tr("unknown analysis error") : errorMessage;
        statusBar()->showMessage(
            tr("Analysis stopped: %1 (%2 nodes)").arg(detail).arg(session_->tree().nodeCount()));
        return;
    }
    if (!rawLoaded_) {
        statusBar()->showMessage(tr("Opened %1, but raw data could not be read: %2")
                                     .arg(session_->identity(), rawError_));
    } else if (session_->tree().isFullyMaterialized()) {
        statusBar()->showMessage(
            tr("Analysis complete: %1 nodes").arg(session_->tree().nodeCount()));
    } else {
        statusBar()->showMessage(
            tr("Analysis finished with partial results: %1 nodes")
                .arg(session_->tree().nodeCount()));
    }
}

QString MainWindow::currentSourceIdentity() const {
    return session_ ? session_->identity() : QString();
}

void MainWindow::selectAnalysisNode(const QModelIndex& current) {
    if (!session_) {
        fieldInspector_->clear();
        clearSourceSelection();
        return;
    }
    const auto nodeId = analysisModel_->nodeIdAt(current);
    const auto node = nodeId ? session_->tree().node(*nodeId) : std::nullopt;
    if (!node) {
        fieldInspector_->clear();
        clearSourceSelection();
        return;
    }
    fieldInspector_->setNode(*node);
    if (!node->location() || node->location()->sourceSpans().empty()) {
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
    if (!nodeIndex.isValid()) {
        const QSignalBlocker blocker(analysisTreeView_->selectionModel());
        analysisTreeView_->selectionModel()->clear();
        fieldInspector_->clear();
        return;
    }

    {
        const QSignalBlocker blocker(analysisTreeView_->selectionModel());
        for (QModelIndex ancestor = nodeIndex.parent(); ancestor.isValid();
             ancestor = ancestor.parent()) {
            analysisTreeView_->expand(ancestor);
        }
        analysisTreeView_->selectionModel()->setCurrentIndex(
            nodeIndex, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        analysisTreeView_->scrollTo(nodeIndex, QAbstractItemView::PositionAtCenter);
    }
    const auto node = session_->tree().node(*nodeId);
    if (node) {
        fieldInspector_->setNode(*node);
    }
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
