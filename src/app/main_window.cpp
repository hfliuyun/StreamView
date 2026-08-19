#include "main_window.h"

#include "analysis_tree_model.h"
#include "field_inspector.h"
#include "raw_data_view.h"

#include <streamview/rules/aac_adts_analyzer.h>
#include <streamview/rules/h264_annex_b_analyzer.h>
#include <streamview/rules/mp4_isobmff_analyzer.h>

#include <QAction>
#include <QBoxLayout>
#include <QDockWidget>
#include <QFileDialog>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>

#include <utility>

namespace streamview::app {

namespace {

constexpr std::size_t kAnalysisBatchRecords = 1;
constexpr quint64 kAnalysisWorkBudget = 64U * 1024U;

} // namespace

MainWindow::MainWindow(QWidget* parent) : MainWindow({}, parent) {}

MainWindow::MainWindow(AnalysisSessionCacheOptions cacheOptions, QWidget* parent)
    : QMainWindow(parent), cacheOptions_(std::move(cacheOptions)) {
    setWindowTitle(tr("StreamView"));
    resize(1280, 800);

    auto aac = rules::loadAacAdtsRulePackage();
    if (aac.succeeded() && aac.package) {
        static_cast<void>(catalog_.registerPackage(std::move(*aac.package)));
    }
    auto h264 = rules::loadH264AnnexBRulePackage();
    if (h264.succeeded() && h264.package) {
        static_cast<void>(catalog_.registerPackage(std::move(*h264.package)));
    }
    auto mp4 = rules::loadMp4IsobmffRulePackage();
    if (mp4.succeeded() && mp4.package) {
        static_cast<void>(catalog_.registerPackage(std::move(*mp4.package)));
    }

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

    auto* treeContainer = new QWidget(analysisDock);
    auto* treeLayout = new QVBoxLayout(treeContainer);
    treeLayout->setContentsMargins(0, 0, 0, 0);
    treeLayout->setSpacing(2);

    auto* navBar = new QWidget(treeContainer);
    navBar->setObjectName(QStringLiteral("analysisTreeNavBar"));
    auto* navLayout = new QHBoxLayout(navBar);
    navLayout->setContentsMargins(4, 2, 4, 2);
    navLayout->setSpacing(6);

    navigationBackButton_ = new QToolButton(navBar);
    navigationBackButton_->setObjectName(QStringLiteral("navigationBackButton"));
    navigationBackButton_->setIcon(style()->standardIcon(QStyle::SP_ArrowBack));
    navigationBackButton_->setToolTip(tr("Return to parent"));
    navigationBackButton_->setAccessibleName(tr("Return to parent format"));
    navigationBackButton_->setEnabled(false);
    navigationBackButton_->setAutoRaise(true);
    connect(navigationBackButton_, &QToolButton::clicked,
            this, &MainWindow::returnToParentFormat);
    navLayout->addWidget(navigationBackButton_);

    navigationBreadcrumbLabel_ = new QLabel(navBar);
    navigationBreadcrumbLabel_->setObjectName(QStringLiteral("navigationBreadcrumbLabel"));
    navigationBreadcrumbLabel_->setText(QString());
    navLayout->addWidget(navigationBreadcrumbLabel_, 1);

    treeLayout->addWidget(navBar);

    analysisTreeView_ = new QTreeView(treeContainer);
    analysisTreeView_->setObjectName(QStringLiteral("analysisTreeView"));
    analysisModel_ = new AnalysisTreeModel(this);
    analysisTreeView_->setModel(analysisModel_);
    analysisTreeView_->setAlternatingRowColors(true);
    analysisTreeView_->setSelectionBehavior(QAbstractItemView::SelectRows);
    analysisTreeView_->setSelectionMode(QAbstractItemView::SingleSelection);
    analysisTreeView_->setUniformRowHeights(true);
    analysisTreeView_->header()->setStretchLastSection(true);
    analysisTreeView_->installEventFilter(this);

    connect(analysisTreeView_->selectionModel(), &QItemSelectionModel::currentChanged,
            this, [this](const QModelIndex& current, const QModelIndex&) {
                selectAnalysisNode(current);
            });
    connect(analysisTreeView_, &QTreeView::doubleClicked,
            this, &MainWindow::onTreeDoubleClicked);

    treeLayout->addWidget(analysisTreeView_, 1);
    analysisDock->setWidget(treeContainer);
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
    session_.reset();
    navigationBreadcrumbFormats_.clear();
    candidate->enableCache(cacheOptions_);
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

    updateNavigationUI();

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
    const bool rootTreeIsActive = session_->navigationDepth() == 0;
    if (rootTreeIsActive) {
        if (!batch.topLevelNodes.empty() &&
            !analysisModel_->appendTopLevelNodes(session_->tree(), batch.topLevelNodes)) {
            analysisModel_->resetFromTree(session_->tree());
            analysisModel_->updateFromTree(session_->tree());
            ++analysisGeneration_;
            statusBar()->showMessage(tr("Analysis tree publication failed"));
            return;
        }
        analysisModel_->updateFromTree(session_->tree());
    }
    if (session_->finished() && session_->cacheWritesPending()) {
        QTimer::singleShot(0, this, [this, generation] { pollAnalysisCache(generation); });
    }

    const QModelIndex currentIndex = analysisTreeView_->currentIndex();
    if (rootTreeIsActive && session_->navigationDepth() == 0 && currentIndex.isValid()) {
        const auto currentId = analysisModel_->nodeIdAt(currentIndex);
        const auto currentNode = currentId ? session_->tree().node(*currentId) : std::nullopt;
        if (currentNode) {
            fieldInspector_->setNode(*currentNode);
        }
    }

    if (batch.status == AnalysisBatchStatus::InvalidBatchSize) {
        ++analysisGeneration_;
        statusBar()->showMessage(
            tr("Analysis batch rejected: %1").arg(batch.errorMessage));
        return;
    }
    if (batch.status == AnalysisBatchStatus::SourceError ||
        batch.status == AnalysisBatchStatus::Cancelled ||
        batch.status == AnalysisBatchStatus::ResourceLimit ||
        batch.status == AnalysisBatchStatus::InvalidRule) {
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

void MainWindow::pollAnalysisCache(quint64 generation) {
    if (generation != analysisGeneration_ || !session_) {
        return;
    }
    session_->pollCacheWrites();
    if (session_->cacheWritesPending()) {
        QTimer::singleShot(1, this, [this, generation] { pollAnalysisCache(generation); });
    }
}

void MainWindow::publishAnalysisStatus(AnalysisBatchStatus status,
                                       const QString& errorMessage) {
    if (!session_) {
        return;
    }
    if (status == AnalysisBatchStatus::Cancelled) {
        statusBar()->showMessage(
            tr("Analysis cancelled: %1 nodes").arg(session_->tree().nodeCount()));
        return;
    }
    if (status == AnalysisBatchStatus::SourceError ||
        status == AnalysisBatchStatus::ResourceLimit ||
        status == AnalysisBatchStatus::InvalidRule) {
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

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    if (watched == analysisTreeView_ && event->type() == QEvent::KeyPress) {
        auto* keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter) {
            return enterChildFormatOnCurrentNode();
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::onTreeDoubleClicked(const QModelIndex& index) {
    if (index.isValid()) {
        analysisTreeView_->setCurrentIndex(index);
    }
    static_cast<void>(enterChildFormatOnCurrentNode());
}

bool MainWindow::enterChildFormatOnCurrentNode() {
    if (!session_) {
        return false;
    }
    const QModelIndex currentIndex = analysisTreeView_->currentIndex();
    if (!currentIndex.isValid()) {
        return false;
    }
    const auto nodeId = analysisModel_->nodeIdAt(currentIndex);
    if (!nodeId) {
        return false;
    }
    const auto node = session_->activeTree().node(*nodeId);
    if (!node || !node->metadata().targetFormat.has_value() ||
        node->metadata().targetFormat->trimmed().isEmpty()) {
        return false;
    }
    const QString targetFormat = *node->metadata().targetFormat;

    const auto navResult = session_->enterChildFormat(*nodeId, catalog_);
    if (!navResult.succeeded()) {
        statusBar()->showMessage(tr("Cannot enter sub-format: %1").arg(navResult.errorMessage));
        return true;
    }

    analysisModel_->resetFromTree(session_->activeTree());
    analysisModel_->updateFromTree(session_->activeTree());

    const QModelIndex childRootIndex = navResult.childRootStructureNodeId.has_value()
                                            ? analysisModel_->indexForNodeId(
                                                  *navResult.childRootStructureNodeId)
                                            : QModelIndex{};
    if (!childRootIndex.isValid()) {
        const auto rollback = session_->returnToParent();
        if (rollback.returned()) {
            analysisModel_->resetFromTree(session_->activeTree());
            analysisModel_->updateFromTree(session_->activeTree());
            const QModelIndex parentIndex = rollback.restoredParentTargetNodeId.has_value()
                                                ? analysisModel_->indexForNodeId(
                                                      *rollback.restoredParentTargetNodeId)
                                                : QModelIndex{};
            if (parentIndex.isValid()) {
                analysisTreeView_->setCurrentIndex(parentIndex);
                selectAnalysisNode(parentIndex);
            } else {
                analysisTreeView_->selectionModel()->clear();
                fieldInspector_->clear();
                clearSourceSelection();
            }
        }
        updateNavigationUI();
        statusBar()->showMessage(tr("Cannot enter sub-format: child root is unavailable"));
        return true;
    }

    navigationBreadcrumbFormats_.append(targetFormat);
    analysisTreeView_->setCurrentIndex(childRootIndex);
    selectAnalysisNode(childRootIndex);
    analysisTreeView_->expandToDepth(1);
    updateNavigationUI();
    return true;
}

void MainWindow::returnToParentFormat() {
    if (!session_ || !session_->canReturnToParent()) {
        return;
    }

    const auto retResult = session_->returnToParent();
    if (retResult.status == AnalysisSessionReturnStatus::Returned) {
        if (!navigationBreadcrumbFormats_.isEmpty()) {
            navigationBreadcrumbFormats_.removeLast();
        }
        analysisModel_->resetFromTree(session_->activeTree());
        analysisModel_->updateFromTree(session_->activeTree());

        if (retResult.restoredParentTargetNodeId.has_value()) {
            const QModelIndex parentIndex =
                analysisModel_->indexForNodeId(*retResult.restoredParentTargetNodeId);
            if (parentIndex.isValid()) {
                for (QModelIndex ancestor = parentIndex.parent(); ancestor.isValid();
                     ancestor = ancestor.parent()) {
                    analysisTreeView_->expand(ancestor);
                }
                analysisTreeView_->setCurrentIndex(parentIndex);
                selectAnalysisNode(parentIndex);
            } else {
                analysisTreeView_->selectionModel()->clear();
                clearSourceSelection();
                fieldInspector_->clear();
            }
        } else {
            clearSourceSelection();
            fieldInspector_->clear();
        }

        updateNavigationUI();
    }
}

void MainWindow::updateNavigationUI() {
    if (!session_) {
        navigationBackButton_->setEnabled(false);
        navigationBreadcrumbLabel_->setText(QString());
        return;
    }

    navigationBackButton_->setEnabled(session_->canReturnToParent());

    const QString rootFormat = session_->ruleIdentity().entryPointId().isEmpty()
                                   ? session_->identity()
                                   : session_->ruleIdentity().entryPointId();
    QStringList breadcrumbParts{rootFormat};
    breadcrumbParts.append(navigationBreadcrumbFormats_);
    navigationBreadcrumbLabel_->setText(breadcrumbParts.join(QStringLiteral(" > ")));
}

void MainWindow::selectAnalysisNode(const QModelIndex& current) {
    if (!session_) {
        fieldInspector_->clear();
        clearSourceSelection();
        return;
    }
    const auto nodeId = analysisModel_->nodeIdAt(current);
    const auto node = nodeId ? session_->activeTree().node(*nodeId) : std::nullopt;
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

    const auto nodeId = session_->activeTree().mostSpecificMaterializedNodeAt(
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
    const auto node = session_->activeTree().node(*nodeId);
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
