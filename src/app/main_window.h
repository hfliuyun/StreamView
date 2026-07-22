#pragma once

#include "analysis_session.h"

#include <QMainWindow>

#include <memory>
class QTreeView;

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
    QTreeView* analysisTreeView_ = nullptr;
    AnalysisTreeModel* analysisModel_ = nullptr;
    RawDataView* rawDataView_ = nullptr;

    std::unique_ptr<AnalysisSession> session_;
};

} // namespace streamview::app
