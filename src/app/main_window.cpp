#include "main_window.h"

#include "analysis_tree_model.h"
#include "field_inspector.h"
#include "format_override_dialog.h"
#include "raw_data_view.h"
#include "rule_manager_dialog.h"
#include "timeline_table_model.h"

#include <streamview/rules/aac_adts_analyzer.h>
#include <streamview/rules/h264_annex_b_analyzer.h>
#include <streamview/rules/mp4_isobmff_analyzer.h>
#include <streamview/rules/rule_package_store.h>

#include <QAction>
#include <QBoxLayout>
#include <QCloseEvent>
#include <QComboBox>
#include <QDir>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyle>
#include <QTableView>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>

#include <algorithm>
#include <functional>
#include <utility>

namespace streamview::app {

namespace {

constexpr std::size_t kAnalysisBatchRecords = 1;
constexpr quint64 kAnalysisWorkBudget = 64U * 1024U;
constexpr quint64 kSamplePageSize = 256;

} // namespace

MainWindow::MainWindow(QWidget* parent) : MainWindow({}, parent) {}

MainWindow::MainWindow(AnalysisSessionCacheOptions cacheOptions, QWidget* parent)
    : QMainWindow(parent), cacheOptions_(std::move(cacheOptions)) {
    resize(1280, 800);

    const QString appData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (!appData.isEmpty()) {
        ruleStorePath_ = QDir(appData).filePath(QStringLiteral("rules"));
    }

    bundledPackageIds_ = {
        QStringLiteral("org.streamview.aac"),
        QStringLiteral("org.streamview.h264"),
        QStringLiteral("org.streamview.mp4")
    };

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

    if (!ruleStorePath_.isEmpty()) {
        for (auto&& pkg : rules::RulePackageStore::discoverInstalled(ruleStorePath_)) {
            static_cast<void>(catalog_.registerPackage(std::move(pkg)));
        }
    }

    rawDataView_ = new RawDataView(this);
    setCentralWidget(rawDataView_);

    setupDocks();
    setupMenus();
    connect(rawDataView_, &RawDataView::sourceBitSelected,
            this, &MainWindow::selectSourceBit);

    formatAmbiguityBannerWidget_ = new QWidget(this);
    formatAmbiguityBannerWidget_->setObjectName(QStringLiteral("formatAmbiguityBanner"));
    auto* bannerLayout = new QHBoxLayout(formatAmbiguityBannerWidget_);
    bannerLayout->setContentsMargins(0, 0, 0, 0);
    bannerLayout->setSpacing(6);

    formatAmbiguityLabel_ = new QLabel(formatAmbiguityBannerWidget_);
    formatAmbiguityLabel_->setObjectName(QStringLiteral("formatAmbiguityLabel"));
    formatAmbiguityLabel_->setStyleSheet(
        QStringLiteral("color: #d9534f; font-weight: bold;"));
    bannerLayout->addWidget(formatAmbiguityLabel_);

    resolveAmbiguityButton_ = new QPushButton(tr("Resolve Ambiguity..."), formatAmbiguityBannerWidget_);
    resolveAmbiguityButton_->setObjectName(QStringLiteral("resolveAmbiguityButton"));
    resolveAmbiguityButton_->setStyleSheet(
        QStringLiteral("font-size: 11px; padding: 2px 6px; font-weight: normal;"));
    connect(resolveAmbiguityButton_, &QPushButton::clicked,
            this, &MainWindow::overrideFormat);
    bannerLayout->addWidget(resolveAmbiguityButton_);

    formatAmbiguityBannerWidget_->hide();
    statusBar()->addPermanentWidget(formatAmbiguityBannerWidget_);

    updateWindowTitle();
    updateActionStates();
    statusBar()->showMessage(tr("Ready"));
}

void MainWindow::setupMenus() {
    auto* fileMenu = menuBar()->addMenu(tr("&File"));
    fileMenu->setObjectName(QStringLiteral("menuFile"));

    actionOpen_ = fileMenu->addAction(tr("&Open..."));
    actionOpen_->setObjectName(QStringLiteral("actionOpen"));
    actionOpen_->setShortcut(QKeySequence::Open);
    connect(actionOpen_, &QAction::triggered, this, &MainWindow::openFile);

    actionOpenSession_ = fileMenu->addAction(tr("Open &Session..."));
    actionOpenSession_->setObjectName(QStringLiteral("actionOpenSession"));
    connect(actionOpenSession_, &QAction::triggered, this, &MainWindow::openSession);

    fileMenu->addSeparator();

    actionSaveSession_ = fileMenu->addAction(tr("&Save Session"));
    actionSaveSession_->setObjectName(QStringLiteral("actionSaveSession"));
    actionSaveSession_->setShortcut(QKeySequence::Save);
    actionSaveSession_->setEnabled(false);
    connect(actionSaveSession_, &QAction::triggered, this, [this] { static_cast<void>(saveSession()); });

    actionSaveSessionAs_ = fileMenu->addAction(tr("Save Session &As..."));
    actionSaveSessionAs_->setObjectName(QStringLiteral("actionSaveSessionAs"));
    actionSaveSessionAs_->setShortcut(QKeySequence::SaveAs);
    actionSaveSessionAs_->setEnabled(false);
    connect(actionSaveSessionAs_, &QAction::triggered, this, [this] { static_cast<void>(saveSessionAs()); });

    fileMenu->addSeparator();

    actionExit_ = fileMenu->addAction(tr("E&xit"));
    actionExit_->setObjectName(QStringLiteral("actionExit"));
    actionExit_->setShortcut(QKeySequence::Quit);
    connect(actionExit_, &QAction::triggered, this, &QWidget::close);

    auto* analysisMenu = menuBar()->addMenu(tr("&Analysis"));
    analysisMenu->setObjectName(QStringLiteral("menuAnalysis"));

    actionOverrideFormat_ = analysisMenu->addAction(tr("&Override Format..."));
    actionOverrideFormat_->setObjectName(QStringLiteral("actionOverrideFormat"));
    actionOverrideFormat_->setEnabled(false);
    connect(actionOverrideFormat_, &QAction::triggered, this, &MainWindow::overrideFormat);

    auto* toolsMenu = menuBar()->addMenu(tr("&Tools"));
    toolsMenu->setObjectName(QStringLiteral("menuTools"));

    actionManageRules_ = toolsMenu->addAction(tr("&Manage Rules..."));
    actionManageRules_->setObjectName(QStringLiteral("actionManageRules"));
    connect(actionManageRules_, &QAction::triggered, this, &MainWindow::openRuleManager);
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

    // --- Timeline & Samples dock (bottom) ---
    timelineDock_ = new QDockWidget(tr("Timeline & Samples"), this);
    timelineDock_->setObjectName(QStringLiteral("timelineDock"));

    auto* timelineContainer = new QWidget(timelineDock_);
    auto* timelineLayout = new QVBoxLayout(timelineContainer);
    timelineLayout->setContentsMargins(4, 4, 4, 4);
    timelineLayout->setSpacing(4);

    auto* controlsBar = new QWidget(timelineContainer);
    controlsBar->setObjectName(QStringLiteral("timelineControlsBar"));
    auto* controlsLayout = new QHBoxLayout(controlsBar);
    controlsLayout->setContentsMargins(0, 0, 0, 0);
    controlsLayout->setSpacing(6);

    auto* trackLabel = new QLabel(tr("Track:"), controlsBar);
    controlsLayout->addWidget(trackLabel);

    timelineTrackComboBox_ = new QComboBox(controlsBar);
    timelineTrackComboBox_->setObjectName(QStringLiteral("timelineTrackComboBox"));
    timelineTrackComboBox_->setMinimumWidth(200);
    connect(timelineTrackComboBox_, &QComboBox::currentIndexChanged,
            this, &MainWindow::onTrackSelectionChanged);
    controlsLayout->addWidget(timelineTrackComboBox_);

    timelinePrevPageButton_ = new QToolButton(controlsBar);
    timelinePrevPageButton_->setObjectName(QStringLiteral("timelinePrevPageButton"));
    timelinePrevPageButton_->setIcon(style()->standardIcon(QStyle::SP_ArrowBack));
    timelinePrevPageButton_->setToolTip(tr("Previous sample page"));
    timelinePrevPageButton_->setAutoRaise(true);
    timelinePrevPageButton_->setEnabled(false);
    connect(timelinePrevPageButton_, &QToolButton::clicked,
            this, &MainWindow::onPrevPageClicked);
    controlsLayout->addWidget(timelinePrevPageButton_);

    timelinePageLabel_ = new QLabel(controlsBar);
    timelinePageLabel_->setObjectName(QStringLiteral("timelinePageLabel"));
    timelinePageLabel_->setText(tr("No samples"));
    controlsLayout->addWidget(timelinePageLabel_);

    timelineNextPageButton_ = new QToolButton(controlsBar);
    timelineNextPageButton_->setObjectName(QStringLiteral("timelineNextPageButton"));
    timelineNextPageButton_->setIcon(style()->standardIcon(QStyle::SP_ArrowForward));
    timelineNextPageButton_->setToolTip(tr("Next sample page"));
    timelineNextPageButton_->setAutoRaise(true);
    timelineNextPageButton_->setEnabled(false);
    connect(timelineNextPageButton_, &QToolButton::clicked,
            this, &MainWindow::onNextPageClicked);
    controlsLayout->addWidget(timelineNextPageButton_);

    timelineStatusLabel_ = new QLabel(controlsBar);
    timelineStatusLabel_->setObjectName(QStringLiteral("timelineStatusLabel"));
    timelineStatusLabel_->setStyleSheet(
        QStringLiteral("color: #d9534f; font-weight: bold; margin-left: 8px;"));
    timelineStatusLabel_->hide();
    controlsLayout->addWidget(timelineStatusLabel_, 1);

    timelineLayout->addWidget(controlsBar);

    timelineTableView_ = new QTableView(timelineContainer);
    timelineTableView_->setObjectName(QStringLiteral("timelineTableView"));
    timelineModel_ = new TimelineTableModel(this);
    timelineTableView_->setModel(timelineModel_);
    timelineTableView_->setAlternatingRowColors(true);
    timelineTableView_->setSelectionBehavior(QAbstractItemView::SelectRows);
    timelineTableView_->setSelectionMode(QAbstractItemView::SingleSelection);
    timelineTableView_->horizontalHeader()->setStretchLastSection(true);
    timelineTableView_->installEventFilter(this);

    connect(timelineTableView_->selectionModel(), &QItemSelectionModel::currentChanged,
            this, [this](const QModelIndex& current, const QModelIndex&) {
                onSampleSelectionChanged(current);
            });
    connect(timelineTableView_, &QTableView::doubleClicked,
            this, &MainWindow::onSampleDoubleClicked);

    timelineLayout->addWidget(timelineTableView_, 1);
    timelineDock_->setWidget(timelineContainer);
    addDockWidget(Qt::BottomDockWidgetArea, timelineDock_);
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (maybeSave()) {
        event->accept();
    } else {
        event->ignore();
    }
}

void MainWindow::updateWindowTitle() {
    QString title;
    if (session_) {
        const QString displayName = currentSessionFilePath_.has_value()
                                        ? QFileInfo(*currentSessionFilePath_).fileName()
                                        : QFileInfo(session_->source().identity()).fileName();
        title = QStringLiteral("%1[*] - StreamView").arg(displayName);
    } else {
        title = QStringLiteral("StreamView");
    }
    setWindowTitle(title);
}

void MainWindow::updateActionStates() {
    const bool hasSession = (session_ != nullptr);
    if (actionSaveSession_ != nullptr) {
        actionSaveSession_->setEnabled(hasSession);
    }
    if (actionSaveSessionAs_ != nullptr) {
        actionSaveSessionAs_->setEnabled(hasSession);
    }
    if (actionOverrideFormat_ != nullptr) {
        actionOverrideFormat_->setEnabled(hasSession);
    }
}

void MainWindow::addBookmark(SessionBookmark bookmark) {
    bookmarks_.push_back(std::move(bookmark));
    setWindowModified(true);
}

void MainWindow::removeBookmark(std::size_t index) {
    if (index < bookmarks_.size()) {
        bookmarks_.erase(bookmarks_.begin() + static_cast<std::ptrdiff_t>(index));
        setWindowModified(true);
    }
}

void MainWindow::clearBookmarks() {
    if (!bookmarks_.empty()) {
        bookmarks_.clear();
        setWindowModified(true);
    }
}

void MainWindow::addAnnotation(SessionAnnotation annotation) {
    annotations_.push_back(std::move(annotation));
    setWindowModified(true);
}

void MainWindow::removeAnnotation(std::size_t index) {
    if (index < annotations_.size()) {
        annotations_.erase(annotations_.begin() + static_cast<std::ptrdiff_t>(index));
        setWindowModified(true);
    }
}

void MainWindow::clearAnnotations() {
    if (!annotations_.empty()) {
        annotations_.clear();
        setWindowModified(true);
    }
}

QModelIndex MainWindow::findIndexByPath(const QString& path) const {
    if (path.isEmpty() || !analysisModel_) {
        return {};
    }
    const QStringList parts = path.split(u'/', Qt::SkipEmptyParts);
    if (parts.isEmpty()) {
        return {};
    }
    QModelIndex currentParent;
    for (const QString& part : parts) {
        bool found = false;
        const int rows = analysisModel_->rowCount(currentParent);
        for (int r = 0; r < rows; ++r) {
            const QModelIndex child = analysisModel_->index(r, AnalysisTreeModel::Name, currentParent);
            if (analysisModel_->data(child, Qt::DisplayRole).toString() == part) {
                currentParent = child;
                found = true;
                break;
            }
        }
        if (!found) {
            return {};
        }
    }
    return currentParent;
}

void MainWindow::expandNodeByPath(const QString& path) {
    const QModelIndex idx = findIndexByPath(path);
    if (idx.isValid()) {
        analysisTreeView_->setExpanded(idx, true);
    }
}

SessionUserState MainWindow::currentUserState() const {
    SessionUserState state;
    state.bookmarks = bookmarks_;
    state.annotations = annotations_;

    if (analysisTreeView_ != nullptr && analysisModel_ != nullptr) {
        std::function<void(const QModelIndex&, const QString&)> collectExpanded;
        collectExpanded = [&](const QModelIndex& parentIndex, const QString& parentPath) {
            const int rows = analysisModel_->rowCount(parentIndex);
            for (int r = 0; r < rows; ++r) {
                const QModelIndex idx = analysisModel_->index(r, AnalysisTreeModel::Name, parentIndex);
                if (!idx.isValid()) {
                    continue;
                }
                const QString nodeName = analysisModel_->data(idx, Qt::DisplayRole).toString();
                const QString currentPath = parentPath.isEmpty() ? nodeName : parentPath + u'/' + nodeName;
                if (analysisTreeView_->isExpanded(idx)) {
                    state.expandedPaths.append(currentPath);
                }
                collectExpanded(idx, currentPath);
            }
        };
        collectExpanded(QModelIndex{}, QString{});
    }

    if (rawDataView_ != nullptr && rawDataView_->model() != nullptr) {
        state.view.rawPageIndex = rawDataView_->model()->pageIndex();
        state.view.rawDisplayMode = rawDataView_->model()->displayMode();
    }
    if (!sourceSelection_.sourceSpans.empty()) {
        state.view.selectedSourceBitOffset = sourceSelection_.sourceSpans.front().start().absoluteBitOffset();
    }
    if (analysisTreeView_ != nullptr && analysisModel_ != nullptr) {
        const QModelIndex currentIdx = analysisTreeView_->currentIndex();
        if (currentIdx.isValid()) {
            QStringList pathComponents;
            for (QModelIndex it = currentIdx; it.isValid(); it = it.parent()) {
                const QModelIndex nameIdx = analysisModel_->index(it.row(), AnalysisTreeModel::Name, it.parent());
                pathComponents.prepend(analysisModel_->data(nameIdx, Qt::DisplayRole).toString());
            }
            state.view.selectedAnalysisPath = pathComponents.join(u'/');
        }
    }
    return state;
}

bool MainWindow::maybeSave() {
    if (!session_ || !isWindowModified()) {
        return true;
    }

    QMessageBox::StandardButton ret = QMessageBox::Cancel;
    if (savePromptHandler_) {
        ret = savePromptHandler_(this);
    } else {
        ret = QMessageBox::warning(
            this, tr("Unsaved Changes"),
            tr("The current session has unsaved changes. Do you want to save your changes before proceeding?"),
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
            QMessageBox::Save);
    }

    if (ret == QMessageBox::Save) {
        return saveSession();
    }
    if (ret == QMessageBox::Discard) {
        return true;
    }
    return false;
}

bool MainWindow::saveSession() {
    if (!session_) {
        return false;
    }
    if (currentSessionFilePath_.has_value() && !currentSessionFilePath_->isEmpty()) {
        return saveSessionToPath(*currentSessionFilePath_);
    }
    return saveSessionAs();
}

bool MainWindow::saveSessionAs() {
    if (!session_) {
        return false;
    }
    QString path;
    if (saveFileDialogHandler_) {
        path = saveFileDialogHandler_(this, tr("Save Session"),
                                      tr("StreamView Session (*.svsession);;All Files (*)"));
    } else {
        path = QFileDialog::getSaveFileName(
            this, tr("Save Session"), QString(),
            tr("StreamView Session (*.svsession);;All Files (*)"));
    }
    if (path.isEmpty()) {
        return false;
    }
    return saveSessionToPath(path);
}

bool MainWindow::saveSessionToPath(const QString& path) {
    if (!session_) {
        return false;
    }
    const auto userState = currentUserState();
    const auto result = session_->saveSession(path, userState);
    if (!result.succeeded()) {
        if (messageDialogHandler_) {
            messageDialogHandler_(this, tr("Save Session Failed"),
                                  tr("Could not save session to %1:\n%2").arg(path, result.errorMessage));
        } else {
            QMessageBox::critical(this, tr("Save Session Failed"),
                                  tr("Could not save session to %1:\n%2").arg(path, result.errorMessage));
        }
        return false;
    }

    currentSessionFilePath_ = path;
    setWindowModified(false);
    updateWindowTitle();
    statusBar()->showMessage(tr("Session saved: %1").arg(QFileInfo(path).fileName()), 3000);
    return true;
}

void MainWindow::openFile() {
    if (!maybeSave()) {
        return;
    }
    QString path;
    if (openFileDialogHandler_) {
        path = openFileDialogHandler_(this, tr("Open Media File"),
                                      tr("H.264 Annex B (*.264 *.h264 *.bin);;All Files (*)"));
    } else {
        path = QFileDialog::getOpenFileName(
            this, tr("Open Media File"), QString(),
            tr("H.264 Annex B (*.264 *.h264 *.bin);;All Files (*)"));
    }

    if (path.isEmpty()) {
        return;
    }

    QString errorMessage;
    if (!openMediaSource(path, &errorMessage)) {
        if (messageDialogHandler_) {
            messageDialogHandler_(this, tr("Cannot Open File"),
                                  tr("Could not open %1:\n%2").arg(path, errorMessage));
        } else {
            QMessageBox::warning(this, tr("Cannot Open File"),
                                 tr("Could not open %1:\n%2").arg(path, errorMessage));
        }
    }
}

void MainWindow::openSession() {
    if (!maybeSave()) {
        return;
    }

    QString path;
    if (openFileDialogHandler_) {
        path = openFileDialogHandler_(this, tr("Open Session"),
                                      tr("StreamView Session (*.svsession);;All Files (*)"));
    } else {
        path = QFileDialog::getOpenFileName(
            this, tr("Open Session"), QString(),
            tr("StreamView Session (*.svsession);;All Files (*)"));
    }

    if (path.isEmpty()) {
        return;
    }

    QString errorMessage;
    if (!openSessionFile(path, &errorMessage)) {
        if (messageDialogHandler_) {
            messageDialogHandler_(this, tr("Cannot Open Session"),
                                  tr("Could not open %1:\n%2").arg(path, errorMessage));
        } else {
            QMessageBox::warning(this, tr("Cannot Open Session"),
                                 tr("Could not open %1:\n%2").arg(path, errorMessage));
        }
    }
}

void MainWindow::overrideFormat() {
    if (!session_) {
        return;
    }

    std::optional<rules::RuleEntryPointIdentity> targetRule;
    if (formatOverrideDialogHandler_) {
        targetRule = formatOverrideDialogHandler_(
            this, catalog_, session_->formatSelection(), session_->ruleIdentity());
    } else {
        FormatOverrideDialog dlg(
            this, catalog_, session_->formatSelection(), session_->ruleIdentity());
        if (dlg.exec() == QDialog::Accepted) {
            targetRule = dlg.selectedRuleEntryPoint();
        }
    }

    if (!targetRule.has_value()) {
        return;
    }

    if (*targetRule == session_->ruleIdentity() && !session_->formatSelection().ambiguous()) {
        return;
    }

    QString errorMessage;
    if (!session_->overrideFormat(catalog_, *targetRule, &errorMessage)) {
        if (messageDialogHandler_) {
            messageDialogHandler_(this, tr("Format Override Failed"),
                                  tr("Failed to override format:\n%1").arg(errorMessage));
        } else {
            QMessageBox::warning(this, tr("Format Override Failed"),
                                 tr("Failed to override format:\n%1").arg(errorMessage));
        }
        return;
    }

    const quint64 generation = ++analysisGeneration_;
    clearSourceSelection();
    {
        const QSignalBlocker blocker(analysisTreeView_->selectionModel());
        analysisTreeView_->selectionModel()->clear();
    }
    fieldInspector_->clear();
    analysisModel_->clear();
    timelineModel_->clear();
    timelineTrackComboBox_->clear();
    navigationBreadcrumbFormats_.clear();

    analysisModel_->resetFromTree(session_->tree());

    advanceAnalysis(generation);

    updateAmbiguityUI();
    loadTracks();

    analysisTreeView_->expandToDepth(1);
    for (int i = 0; i < AnalysisTreeModel::ColumnCount; ++i) {
        analysisTreeView_->resizeColumnToContents(i);
    }

    updateNavigationUI();
    setWindowModified(true);
    updateWindowTitle();
    updateActionStates();
}

void MainWindow::openRuleManager() {
    if (ruleManagerDialogHandler_) {
        ruleManagerDialogHandler_(this, catalog_, ruleStorePath_, bundledPackageIds_);
        return;
    }
    RuleManagerDialog dlg(this, catalog_, ruleStorePath_, bundledPackageIds_);
    dlg.exec();
}

bool MainWindow::openSessionFile(const QString& sessionPath, QString* errorMessage) {
    if (!maybeSave()) {
        if (errorMessage != nullptr) {
            *errorMessage = tr("Operation cancelled by user");
        }
        return false;
    }

    auto restored = AnalysisSession::restoreSession(sessionPath, catalog_, cacheOptions_);
    if (!restored.succeeded()) {
        if (errorMessage != nullptr) {
            *errorMessage = restored.errorMessage.isEmpty()
                                ? tr("Failed to restore session")
                                : restored.errorMessage;
        }
        return false;
    }

    auto candidate = std::move(restored.session);
    const auto savedUserState = candidate->userState();

    const quint64 generation = ++analysisGeneration_;
    clearSourceSelection();
    {
        const QSignalBlocker blocker(analysisTreeView_->selectionModel());
        analysisTreeView_->selectionModel()->clear();
    }
    fieldInspector_->clear();
    rawDataView_->clear();
    analysisModel_->clear();
    timelineModel_->clear();
    timelineTrackComboBox_->clear();
    session_.reset();
    navigationBreadcrumbFormats_.clear();

    session_ = std::move(candidate);
    bookmarks_ = savedUserState.bookmarks;
    annotations_ = savedUserState.annotations;
    currentSessionFilePath_ = sessionPath;

    rawError_.clear();
    rawLoaded_ = rawDataView_->setSource(
        &session_->source(), session_->initialPage(), &rawError_);
    analysisModel_->resetFromTree(session_->tree());

    advanceAnalysis(generation);

    updateAmbiguityUI();
    loadTracks();

    if (!savedUserState.expandedPaths.isEmpty()) {
        for (const QString& p : savedUserState.expandedPaths) {
            expandNodeByPath(p);
        }
    } else {
        analysisTreeView_->expandToDepth(1);
    }

    for (int i = 0; i < AnalysisTreeModel::ColumnCount; ++i) {
        analysisTreeView_->resizeColumnToContents(i);
    }

    if (savedUserState.view.rawPageIndex > 0 && rawDataView_->model() != nullptr) {
        static_cast<void>(rawDataView_->model()->loadPage(savedUserState.view.rawPageIndex));
    }
    if (rawDataView_->model() != nullptr) {
        rawDataView_->model()->setDisplayMode(savedUserState.view.rawDisplayMode);
    }
    if (savedUserState.view.selectedSourceBitOffset.has_value()) {
        selectSourceBit(*savedUserState.view.selectedSourceBitOffset);
    }
    if (savedUserState.view.selectedAnalysisPath.has_value()) {
        const QModelIndex idx = findIndexByPath(*savedUserState.view.selectedAnalysisPath);
        if (idx.isValid()) {
            analysisTreeView_->setCurrentIndex(idx);
            selectAnalysisNode(idx);
        }
    }

    updateNavigationUI();
    setWindowModified(false);
    updateWindowTitle();
    updateActionStates();

    if (errorMessage != nullptr) {
        errorMessage->clear();
    }
    return true;
}

bool MainWindow::openMediaSource(const QString& path, QString* errorMessage) {
    if (!maybeSave()) {
        if (errorMessage != nullptr) {
            *errorMessage = tr("Operation cancelled by user");
        }
        return false;
    }

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
    timelineModel_->clear();
    timelineTrackComboBox_->clear();
    session_.reset();
    navigationBreadcrumbFormats_.clear();
    candidate->enableCache(cacheOptions_);
    session_ = std::move(candidate);
    bookmarks_.clear();
    annotations_.clear();
    currentSessionFilePath_.reset();

    rawError_.clear();
    rawLoaded_ = rawDataView_->setSource(
        &session_->source(), session_->initialPage(), &rawError_);
    analysisModel_->resetFromTree(session_->tree());

    // Publish the first batch before returning so the new session is immediately useful.
    advanceAnalysis(generation);

    updateAmbiguityUI();
    loadTracks();

    // Auto-expand the first two levels for visibility.
    analysisTreeView_->expandToDepth(1);

    // Resize columns to content.
    for (int i = 0; i < AnalysisTreeModel::ColumnCount; ++i) {
        analysisTreeView_->resizeColumnToContents(i);
    }

    updateNavigationUI();
    setWindowModified(false);
    updateWindowTitle();
    updateActionStates();

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
        if (rootTreeIsActive) {
            loadTracks();
        }
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
    if (rootTreeIsActive) {
        loadTracks();
    }
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
    if (watched == timelineTableView_ && event->type() == QEvent::KeyPress) {
        auto* keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter) {
            return enterSampleOnCurrentRow();
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

        if (retResult.restoredSample.has_value()) {
            const auto& restored = *retResult.restoredSample;
            for (int i = 0; i < timelineTrackComboBox_->count(); ++i) {
                if (timelineTrackComboBox_->itemData(i).toUInt() == restored.trackId) {
                    const QSignalBlocker blocker(timelineTrackComboBox_);
                    timelineTrackComboBox_->setCurrentIndex(i);
                    break;
                }
            }
            currentTrackId_ = restored.trackId;
            const quint64 targetPage = restored.sampleIndex / kSamplePageSize;
            loadSamplePage(currentTrackId_, targetPage);

            const int row = static_cast<int>(restored.sampleIndex % kSamplePageSize);
            const QModelIndex sampleIndex = timelineModel_->index(row, 0);
            if (sampleIndex.isValid()) {
                const QSignalBlocker blocker(timelineTableView_->selectionModel());
                timelineTableView_->selectRow(row);
                timelineTableView_->scrollTo(sampleIndex, QAbstractItemView::PositionAtCenter);
            }
            if (!restored.sample.sourceSpans.empty()) {
                SourceSelection selection;
                selection.sourceIdentity = session_->identity();
                selection.sourceSpans = restored.sample.sourceSpans;
                setSourceSelection(std::move(selection));
            }
            fieldInspector_->clear();
        } else if (retResult.restoredParentTargetNodeId.has_value()) {
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

    const auto* sampleFrame = session_->currentSampleFrame();
    if (sampleFrame != nullptr) {
        const QString trackDesc = sampleFrame->targetFormat.isEmpty()
                                      ? tr("Track %1").arg(sampleFrame->trackId)
                                      : tr("Track %1 (%2)").arg(sampleFrame->trackId).arg(sampleFrame->targetFormat);
        const QString sampleDesc = tr("Sample #%1%2")
                                       .arg(sampleFrame->sampleIndex)
                                       .arg(sampleFrame->sample.isSyncSample ? QStringLiteral(" [Sync]") : QString());
        breadcrumbParts.append(trackDesc);
        breadcrumbParts.append(sampleDesc);
    }

    breadcrumbParts.append(navigationBreadcrumbFormats_);
    navigationBreadcrumbLabel_->setText(breadcrumbParts.join(QStringLiteral(" > ")));
}

void MainWindow::onTrackSelectionChanged(int comboIndex) {
    if (comboIndex < 0 || !session_) {
        timelineModel_->clear();
        currentTrackId_ = 0;
        currentTotalSamples_ = 0;
        currentPageIndex_ = 0;
        updateTimelinePageControls();
        return;
    }

    const quint32 trackId = timelineTrackComboBox_->itemData(comboIndex).toUInt();
    currentTrackId_ = trackId;

    const auto tracksResult = session_->tracks();
    if (tracksResult.available()) {
        for (const auto& track : tracksResult.tracks) {
            if (track.trackId == trackId) {
                currentTotalSamples_ = track.sampleCount;
                currentTimescale_ = track.timescale;
                break;
            }
        }
    }

    loadSamplePage(trackId, 0);
}

void MainWindow::onSampleSelectionChanged(const QModelIndex& current) {
    if (!session_ || !current.isValid()) {
        return;
    }
    const auto* sample = timelineModel_->sampleAt(current.row());
    if (sample == nullptr || sample->sourceSpans.empty()) {
        return;
    }

    SourceSelection selection;
    selection.sourceIdentity = session_->identity();
    selection.sourceSpans = sample->sourceSpans;
    setSourceSelection(std::move(selection));
}

void MainWindow::onSampleDoubleClicked(const QModelIndex& index) {
    if (index.isValid()) {
        timelineTableView_->setCurrentIndex(index);
    }
    static_cast<void>(enterSampleOnCurrentRow());
}

void MainWindow::onPrevPageClicked() {
    if (currentPageIndex_ > 0) {
        loadSamplePage(currentTrackId_, currentPageIndex_ - 1);
    }
}

void MainWindow::onNextPageClicked() {
    const quint64 totalPages = (currentTotalSamples_ == 0)
                                   ? 1
                                   : ((currentTotalSamples_ + kSamplePageSize - 1) / kSamplePageSize);
    if (currentPageIndex_ + 1 < totalPages) {
        loadSamplePage(currentTrackId_, currentPageIndex_ + 1);
    }
}

bool MainWindow::enterSampleOnCurrentRow() {
    if (!session_) {
        return false;
    }
    const QModelIndex currentIndex = timelineTableView_->currentIndex();
    if (!currentIndex.isValid()) {
        return false;
    }
    const auto* sample = timelineModel_->sampleAt(currentIndex.row());
    if (sample == nullptr) {
        return false;
    }

    const quint32 trackId = currentTrackId_;
    const quint64 sampleIndex = sample->sampleIndex;
    const auto navResult = session_->enterSample(trackId, sampleIndex, catalog_);
    if (!navResult.entered()) {
        statusBar()->showMessage(tr("Cannot enter sample: %1").arg(navResult.errorMessage));
        return true;
    }

    analysisModel_->resetFromTree(session_->activeTree());
    analysisModel_->updateFromTree(session_->activeTree());

    const QModelIndex childRootIndex = navResult.sampleNodeId.has_value()
                                            ? analysisModel_->indexForNodeId(
                                                  *navResult.sampleNodeId)
                                            : QModelIndex{};
    if (!childRootIndex.isValid()) {
        const auto rollback = session_->returnToParent();
        if (rollback.returned()) {
            analysisModel_->resetFromTree(session_->activeTree());
            analysisModel_->updateFromTree(session_->activeTree());
        }
        updateNavigationUI();
        statusBar()->showMessage(tr("Cannot enter sample: child root is unavailable"));
        return true;
    }

    analysisTreeView_->setCurrentIndex(childRootIndex);
    selectAnalysisNode(childRootIndex);
    analysisTreeView_->expandToDepth(1);
    updateNavigationUI();
    return true;
}

void MainWindow::loadTracks() {
    timelineTrackComboBox_->clear();
    timelineModel_->clear();
    timelineStatusLabel_->clear();
    timelineStatusLabel_->hide();
    currentTrackId_ = 0;
    currentPageIndex_ = 0;
    currentTotalSamples_ = 0;
    currentTimescale_ = 1;
    updateTimelinePageControls();

    if (!session_) {
        return;
    }

    bool isTruncated = false;
    for (quint64 i = 1; i <= session_->tree().nodeCount(); ++i) {
        const auto n = session_->tree().node(core::AnalysisNodeId(i));
        if (n) {
            for (const auto& diag : n->diagnostics()) {
                if (diag.code == core::DiagnosticCode::TruncatedSource) {
                    isTruncated = true;
                    break;
                }
            }
            if (isTruncated) {
                break;
            }
        }
    }

    const auto tracksResult = session_->tracks();

    if (tracksResult.status == AnalysisSessionSampleStatus::UnsupportedSource) {
        timelineStatusLabel_->setText(tr("Container tracks unavailable for elementary stream"));
        timelineStatusLabel_->show();
        updateTimelinePageControls();
        return;
    }

    if (isTruncated) {
        timelineStatusLabel_->setText(tr("File is truncated; track list may be incomplete"));
        timelineStatusLabel_->show();
    }

    if (!tracksResult.available() || tracksResult.tracks.empty()) {
        if (!isTruncated) {
            timelineStatusLabel_->setText(
                tracksResult.errorMessage.isEmpty()
                    ? tr("No tracks found")
                    : tr("No tracks: %1").arg(tracksResult.errorMessage));
            timelineStatusLabel_->show();
        }
        updateTimelinePageControls();
        return;
    }

    for (const auto& track : tracksResult.tracks) {
        const QString formatText =
            track.targetFormat.isEmpty() ? tr("unknown") : track.targetFormat;
        const QString itemText = tr("Track %1 (%2, %3 samples)")
                                     .arg(track.trackId)
                                     .arg(formatText)
                                     .arg(track.sampleCount);
        timelineTrackComboBox_->addItem(itemText, track.trackId);
    }

    if (timelineTrackComboBox_->count() > 0) {
        timelineTrackComboBox_->setCurrentIndex(0);
        onTrackSelectionChanged(0);
    }
}

void MainWindow::loadSamplePage(quint32 trackId, quint64 pageIndex) {
    if (!session_ || trackId == 0) {
        timelineModel_->clear();
        updateTimelinePageControls();
        return;
    }

    AnalysisSessionSamplePageRequest request;
    request.trackId = trackId;
    request.pageIndex = pageIndex * kSamplePageSize;
    request.pageSize = kSamplePageSize;

    const auto pageResult = session_->samplesForTrack(request);
    if (!pageResult.available()) {
        timelineModel_->clear();
        updateTimelinePageControls();
        return;
    }

    currentPageIndex_ = pageIndex;
    timelineModel_->setSamples(pageResult.descriptors, currentTimescale_);
    updateTimelinePageControls();

    for (int i = 0; i < TimelineTableModel::ColumnCount; ++i) {
        timelineTableView_->resizeColumnToContents(i);
    }
}

void MainWindow::updateTimelinePageControls() {
    const quint64 totalPages = (currentTotalSamples_ == 0)
                                   ? 1
                                   : ((currentTotalSamples_ + kSamplePageSize - 1) / kSamplePageSize);
    timelinePrevPageButton_->setEnabled(currentPageIndex_ > 0);
    timelineNextPageButton_->setEnabled(currentPageIndex_ + 1 < totalPages);

    if (currentTotalSamples_ == 0) {
        timelinePageLabel_->setText(tr("No samples"));
    } else {
        const quint64 first = currentPageIndex_ * kSamplePageSize;
        const quint64 last = std::min(first + kSamplePageSize, currentTotalSamples_) - 1;
        timelinePageLabel_->setText(tr("Page %1 of %2 (samples %3-%4 of %5)")
                                        .arg(currentPageIndex_ + 1)
                                        .arg(totalPages)
                                        .arg(first)
                                        .arg(last)
                                        .arg(currentTotalSamples_));
    }
}

void MainWindow::updateAmbiguityUI() {
    if (!session_) {
        formatAmbiguityLabel_->clear();
        formatAmbiguityLabel_->hide();
        if (formatAmbiguityBannerWidget_ != nullptr) {
            formatAmbiguityBannerWidget_->hide();
        }
        return;
    }
    if (session_->formatSelection().ambiguous()) {
        formatAmbiguityLabel_->setText(
            tr("Warning: Ambiguous format (container vs elementary stream detected)"));
        formatAmbiguityLabel_->show();
        if (formatAmbiguityBannerWidget_ != nullptr) {
            formatAmbiguityBannerWidget_->show();
        }
    } else {
        formatAmbiguityLabel_->clear();
        formatAmbiguityLabel_->hide();
        if (formatAmbiguityBannerWidget_ != nullptr) {
            formatAmbiguityBannerWidget_->hide();
        }
    }
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
