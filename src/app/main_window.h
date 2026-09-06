#pragma once

#include "analysis_session.h"
#include "session_document.h"
#include "source_selection.h"

#include <QMainWindow>
#include <QMessageBox>
#include <QStringList>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <vector>

class QAction;
class QCloseEvent;
class QComboBox;
class QDockWidget;
class QLabel;
class QModelIndex;
class QProgressBar;
class QPushButton;
class QTableView;
class QToolButton;
class QTreeView;

namespace streamview::app {

class AnalysisTreeModel;
class DiagnosticsSummaryDock;
class FieldInspector;
class RawDataView;
class TimelineTableModel;

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    explicit MainWindow(AnalysisSessionCacheOptions cacheOptions,
                        QWidget* parent = nullptr);

    [[nodiscard]] bool openMediaSource(const QString& path,
                                       QString* errorMessage = nullptr);
    [[nodiscard]] bool openSessionFile(const QString& sessionPath,
                                       QString* errorMessage = nullptr);
    [[nodiscard]] QString currentSourceIdentity() const;
    [[nodiscard]] std::optional<QString> currentSessionFilePath() const noexcept {
        return currentSessionFilePath_;
    }

    [[nodiscard]] const std::vector<SessionBookmark>& bookmarks() const noexcept {
        return bookmarks_;
    }
    [[nodiscard]] const std::vector<SessionAnnotation>& annotations() const noexcept {
        return annotations_;
    }
    void addBookmark(SessionBookmark bookmark);
    void removeBookmark(std::size_t index);
    void clearBookmarks();
    void addAnnotation(SessionAnnotation annotation);
    void removeAnnotation(std::size_t index);
    void clearAnnotations();

    [[nodiscard]] SessionUserState currentUserState() const;
    [[nodiscard]] bool maybeSave();

    // Testing hooks
    using SavePromptHandler = std::function<QMessageBox::StandardButton(QWidget*)>;
    using FileDialogHandler = std::function<QString(QWidget*, const QString&, const QString&)>;
    using MessageDialogHandler = std::function<void(QWidget*, const QString&, const QString&)>;

    void setSavePromptHandlerForTesting(SavePromptHandler handler) {
        savePromptHandler_ = std::move(handler);
    }
    void setSaveFileDialogHandlerForTesting(FileDialogHandler handler) {
        saveFileDialogHandler_ = std::move(handler);
    }
    void setOpenFileDialogHandlerForTesting(FileDialogHandler handler) {
        openFileDialogHandler_ = std::move(handler);
    }
    void setMessageDialogHandlerForTesting(MessageDialogHandler handler) {
        messageDialogHandler_ = std::move(handler);
    }
    using FormatOverrideDialogHandler = std::function<std::optional<rules::RuleEntryPointIdentity>(
        QWidget*,
        const rules::RulePackageCatalog&,
        const rules::FormatSelection&,
        const rules::RuleEntryPointIdentity&)>;
    void setFormatOverrideDialogHandlerForTesting(FormatOverrideDialogHandler handler) {
        formatOverrideDialogHandler_ = std::move(handler);
    }
    using RuleManagerDialogHandler = std::function<void(
        QWidget*,
        rules::RulePackageCatalog&,
        const QString&,
        const std::set<QString>&)>;
    void setRuleManagerDialogHandlerForTesting(RuleManagerDialogHandler handler) {
        ruleManagerDialogHandler_ = std::move(handler);
    }
    void setRuleStorePathForTesting(QString path) {
        ruleStorePath_ = std::move(path);
    }
    [[nodiscard]] const QString& ruleStorePath() const noexcept {
        return ruleStorePath_;
    }

public slots:
    void openFile();
    void openSession();
    bool saveSession();
    bool saveSessionAs();
    void overrideFormat();
    void openRuleManager();
    void cancelAnalysis();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private slots:
    void returnToParentFormat();
    void onTreeDoubleClicked(const QModelIndex& index);
    void onTrackSelectionChanged(int comboIndex);
    void onSampleSelectionChanged(const QModelIndex& current);
    void onSampleDoubleClicked(const QModelIndex& index);
    void onPrevPageClicked();
    void onNextPageClicked();
    void onDiagnosticSelected(core::AnalysisNodeId nodeId,
                              std::optional<core::FieldLocation> location);

private:
    void setupMenus();
    void setupDocks();
    void selectAnalysisNode(const QModelIndex& current);
    void selectSourceBit(quint64 absoluteBitOffset);
    void setSourceSelection(SourceSelection selection);
    void clearSourceSelection();
    void advanceAnalysis(quint64 generation);
    void pollAnalysisCache(quint64 generation);
    void publishAnalysisStatus(AnalysisBatchStatus status,
                               const QString& errorMessage);
    [[nodiscard]] bool enterChildFormatOnCurrentNode();
    [[nodiscard]] bool enterSampleOnCurrentRow();
    void updateNavigationUI();
    void loadTracks();
    void loadSamplePage(quint32 trackId, quint64 pageIndex);
    void updateTimelinePageControls();
    void updateAmbiguityUI();
    [[nodiscard]] bool saveSessionToPath(const QString& path);
    void updateWindowTitle();
    void updateActionStates();
    [[nodiscard]] QModelIndex findIndexByPath(const QString& path) const;
    void expandNodeByPath(const QString& path);

    QTreeView* analysisTreeView_ = nullptr;
    AnalysisTreeModel* analysisModel_ = nullptr;
    QToolButton* navigationBackButton_ = nullptr;
    QLabel* navigationBreadcrumbLabel_ = nullptr;
    FieldInspector* fieldInspector_ = nullptr;
    RawDataView* rawDataView_ = nullptr;

    QDockWidget* timelineDock_ = nullptr;
    QComboBox* timelineTrackComboBox_ = nullptr;
    QLabel* timelineStatusLabel_ = nullptr;
    QToolButton* timelinePrevPageButton_ = nullptr;
    QToolButton* timelineNextPageButton_ = nullptr;
    QLabel* timelinePageLabel_ = nullptr;
    QTableView* timelineTableView_ = nullptr;
    TimelineTableModel* timelineModel_ = nullptr;
    QLabel* formatAmbiguityLabel_ = nullptr;
    QWidget* formatAmbiguityBannerWidget_ = nullptr;
    QPushButton* resolveAmbiguityButton_ = nullptr;
    QProgressBar* analysisProgressBar_ = nullptr;
    QPushButton* cancelAnalysisButton_ = nullptr;
    DiagnosticsSummaryDock* diagnosticsSummaryDock_ = nullptr;

    SourceSelection sourceSelection_;

    std::unique_ptr<AnalysisSession> session_;
    rules::RulePackageCatalog catalog_;
    AnalysisSessionCacheOptions cacheOptions_;
    QStringList navigationBreadcrumbFormats_;
    quint64 analysisGeneration_ = 0;
    bool rawLoaded_ = true;
    QString rawError_;

    quint32 currentTrackId_ = 0;
    quint64 currentPageIndex_ = 0;
    quint64 currentTotalSamples_ = 0;
    quint32 currentTimescale_ = 1;

    std::optional<QString> currentSessionFilePath_;
    std::vector<SessionBookmark> bookmarks_;
    std::vector<SessionAnnotation> annotations_;
    QString ruleStorePath_;
    std::set<QString> bundledPackageIds_;

    QAction* actionOpen_ = nullptr;
    QAction* actionOpenSession_ = nullptr;
    QAction* actionSaveSession_ = nullptr;
    QAction* actionSaveSessionAs_ = nullptr;
    QAction* actionOverrideFormat_ = nullptr;
    QAction* actionManageRules_ = nullptr;
    QAction* actionExit_ = nullptr;
    QAction* actionToggleDiagnosticsDock_ = nullptr;
    QAction* actionToggleTimelineDock_ = nullptr;

    SavePromptHandler savePromptHandler_;
    FileDialogHandler saveFileDialogHandler_;
    FileDialogHandler openFileDialogHandler_;
    MessageDialogHandler messageDialogHandler_;
    FormatOverrideDialogHandler formatOverrideDialogHandler_;
    RuleManagerDialogHandler ruleManagerDialogHandler_;
};

} // namespace streamview::app
