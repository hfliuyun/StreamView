#pragma once

#include "analysis_session.h"
#include "source_selection.h"

#include <QMainWindow>

#include <memory>
class QTreeView;
class QModelIndex;

namespace streamview::app {

class AnalysisTreeModel;
class RawDataView;

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);

    [[nodiscard]] bool openMediaSource(const QString& path,
                                       QString* errorMessage = nullptr);
    [[nodiscard]] QString currentSourceIdentity() const;

private slots:
    void openFile();

private:
    void setupMenus();
    void setupDocks();
    void selectAnalysisNode(const QModelIndex& current);
    void selectSourceBit(quint64 absoluteBitOffset);
    void setSourceSelection(SourceSelection selection);
    void clearSourceSelection();
    QTreeView* analysisTreeView_ = nullptr;
    AnalysisTreeModel* analysisModel_ = nullptr;
    RawDataView* rawDataView_ = nullptr;
    SourceSelection sourceSelection_;

    std::unique_ptr<AnalysisSession> session_;
};

} // namespace streamview::app
