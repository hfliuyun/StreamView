#pragma once

#include <streamview/core/source.h>
#include <streamview/rules/h264_annex_b_analyzer.h>

#include <QMainWindow>

#include <memory>
#include <optional>

class QTreeView;

namespace streamview::app {

class AnalysisTreeModel;

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);

private slots:
    void openFile();

private:
    void setupMenus();
    void setupDocks();
    void analyzeCurrentSource();

    QTreeView* analysisTreeView_ = nullptr;
    AnalysisTreeModel* analysisModel_ = nullptr;

    std::unique_ptr<core::RandomAccessSource> source_;
    std::optional<rules::H264AnnexBAnalyzer> analyzer_;
};

} // namespace streamview::app
