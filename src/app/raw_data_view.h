#pragma once

#include "raw_data_model.h"

#include <streamview/core/coordinates.h>
#include <streamview/core/source.h>

#include <QWidget>

#include <vector>

class QLabel;
class QEvent;
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
    void setSourceSelection(std::vector<core::SourceSpan> sourceSpans);
    void clear();
    void retranslateUi();
    void setDisplayMode(RawDisplayMode mode);
    [[nodiscard]] bool loadPage(quint64 pageIndex, QString* errorMessage = nullptr);
    [[nodiscard]] RawDataModel* model() const noexcept { return model_; }

signals:
    void sourceReadFailed(const QString& errorMessage);
    void sourceBitSelected(quint64 absoluteBitOffset);

protected:
    [[nodiscard]] bool eventFilter(QObject* watched, QEvent* event) override;
    void changeEvent(QEvent* event) override;

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
    QToolButton* hexButton_ = nullptr;
    QToolButton* binaryButton_ = nullptr;
    QToolButton* combinedButton_ = nullptr;
};

} // namespace streamview::app
