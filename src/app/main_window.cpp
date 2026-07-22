#include "main_window.h"

#include "analysis_tree_model.h"

#include <QDockWidget>
#include <QFileDialog>
#include <QHeaderView>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QStatusBar>
#include <QTreeView>

namespace streamview::app {

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(tr("StreamView"));
    resize(1280, 800);

    auto* rawDataPlaceholder = new QLabel(tr("Open a media file to inspect its bits."), this);
    rawDataPlaceholder->setAlignment(Qt::AlignCenter);
    setCentralWidget(rawDataPlaceholder);

    setupDocks();
    setupMenus();

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
    analysisModel_ = new AnalysisTreeModel(this);
    analysisTreeView_->setModel(analysisModel_);
    analysisTreeView_->setAlternatingRowColors(true);
    analysisTreeView_->setUniformRowHeights(true);
    analysisTreeView_->header()->setStretchLastSection(true);
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
    auto newSource = core::FileSource::open(path, &errorMessage);
    if (!newSource) {
        QMessageBox::warning(this, tr("Cannot Open File"),
                             tr("Could not open %1:\n%2").arg(path, errorMessage));
        return;
    }

    // Replace current session.
    source_ = std::move(newSource);
    analyzer_.reset();
    analysisModel_->clear();

    statusBar()->showMessage(tr("Opened: %1 (%2 bytes)")
                                 .arg(source_->identity())
                                 .arg(source_->sizeBytes()));

    analyzeCurrentSource();
}

void MainWindow::analyzeCurrentSource() {
    if (!source_) {
        return;
    }

    QString errorMessage;
    auto analyzer = rules::H264AnnexBAnalyzer::create(*source_, &errorMessage);
    if (!analyzer) {
        QMessageBox::warning(this, tr("Analysis Failed"),
                             tr("Could not create analyzer:\n%1").arg(errorMessage));
        return;
    }

    analyzer_ = std::move(*analyzer);

    // Run all batches synchronously for now (M2 will move to async).
    while (!analyzer_->finished()) {
        (void)analyzer_->analyzeBatch();
    }

    analysisModel_->resetFromTree(analyzer_->tree());

    // Auto-expand the first two levels for visibility.
    analysisTreeView_->expandToDepth(1);

    // Resize columns to content.
    for (int i = 0; i < AnalysisTreeModel::ColumnCount; ++i) {
        analysisTreeView_->resizeColumnToContents(i);
    }

    const auto rootNode = analyzer_->tree().node(analyzer_->tree().rootId());
    if (rootNode && rootNode->state() == core::MaterializationState::Materialized) {
        statusBar()->showMessage(
            tr("Analysis complete: %1 nodes").arg(analyzer_->tree().nodeCount()));
    } else {
        statusBar()->showMessage(
            tr("Analysis finished with partial results: %1 nodes")
                .arg(analyzer_->tree().nodeCount()));
    }
}

} // namespace streamview::app
