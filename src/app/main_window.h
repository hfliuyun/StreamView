#pragma once

#include <QMainWindow>

namespace streamview::app {

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
};

} // namespace streamview::app
