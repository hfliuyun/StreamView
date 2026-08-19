#pragma once

#include "analysis_session.h"
#include "source_selection.h"

#include <QMainWindow>
#include <QStringList>

#include <memory>
#include <cstdint>
class QTreeView;
class QModelIndex;
class QToolButton;
class QLabel;

namespace streamview::app {

class AnalysisTreeModel;
class FieldInspector;
class RawDataView;

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    explicit MainWindow(AnalysisSessionCacheOptions cacheOptions,
                        QWidget* parent = nullptr);

    [[nodiscard]] bool openMediaSource(const QString& path,
                                       QString* errorMessage = nullptr);
    [[nodiscard]] QString currentSourceIdentity() const;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private slots:
    void openFile();
    void returnToParentFormat();
    void onTreeDoubleClicked(const QModelIndex& index);

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
    void updateNavigationUI();

    QTreeView* analysisTreeView_ = nullptr;
    AnalysisTreeModel* analysisModel_ = nullptr;
    QToolButton* navigationBackButton_ = nullptr;
    QLabel* navigationBreadcrumbLabel_ = nullptr;
    FieldInspector* fieldInspector_ = nullptr;
    RawDataView* rawDataView_ = nullptr;
    SourceSelection sourceSelection_;

    std::unique_ptr<AnalysisSession> session_;
    rules::RulePackageCatalog catalog_;
    AnalysisSessionCacheOptions cacheOptions_;
    QStringList navigationBreadcrumbFormats_;
    quint64 analysisGeneration_ = 0;
    bool rawLoaded_ = true;
    QString rawError_;
};

} // namespace streamview::app
