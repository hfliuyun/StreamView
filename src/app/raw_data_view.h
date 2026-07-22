#pragma once

#include "raw_data_model.h"

#include <streamview/core/source.h>

#include <QWidget>

class QLabel;
class QTableView;
class QToolButton;

namespace streamview::app {

class RawDataView final : public QWidget {
    Q_OBJECT

public:
    explicit RawDataView(QWidget* parent = nullptr);

    [[nodiscard]] bool setSource(const core::RandomAccessSource* source,
                                 QString* errorMessage = nullptr);
    [[nodiscard]] bool setSource(const core::RandomAccessSource* source,
                                 core::SourcePage preparedPage,
                                 QString* errorMessage = nullptr);
    void clear();
    [[nodiscard]] RawDataModel* model() const noexcept { return model_; }

signals:
    void sourceReadFailed(const QString& errorMessage);

private:
    void loadPreviousPage();
    void loadNextPage();
    void setMode(RawDisplayMode mode);
    void updateControls();
    void updateCellSizes();

    RawDataModel* model_ = nullptr;
    QTableView* table_ = nullptr;
    QToolButton* previousPageButton_ = nullptr;
    QToolButton* nextPageButton_ = nullptr;
    QLabel* pageLabel_ = nullptr;
};

} // namespace streamview::app
