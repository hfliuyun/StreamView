#include "main_window.h"

#include <QDockWidget>
#include <QLabel>
#include <QStatusBar>
#include <QTreeView>

namespace streamview::app {

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(tr("StreamView"));
    resize(1280, 800);

    auto* rawDataPlaceholder = new QLabel(tr("Open a media file to inspect its bits."), this);
    rawDataPlaceholder->setAlignment(Qt::AlignCenter);
    setCentralWidget(rawDataPlaceholder);

    auto* analysisDock = new QDockWidget(tr("Analysis Tree"), this);
    analysisDock->setObjectName(QStringLiteral("analysisTreeDock"));
    analysisDock->setWidget(new QTreeView(analysisDock));
    addDockWidget(Qt::LeftDockWidgetArea, analysisDock);

    auto* inspectorDock = new QDockWidget(tr("Field Inspector"), this);
    inspectorDock->setObjectName(QStringLiteral("fieldInspectorDock"));
    auto* inspectorPlaceholder = new QLabel(tr("No field selected."), inspectorDock);
    inspectorPlaceholder->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    inspectorDock->setWidget(inspectorPlaceholder);
    addDockWidget(Qt::RightDockWidgetArea, inspectorDock);

    statusBar()->showMessage(tr("Ready"));
}

} // namespace streamview::app
