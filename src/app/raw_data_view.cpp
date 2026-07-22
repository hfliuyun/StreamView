#include "raw_data_view.h"

#include <QButtonGroup>
#include <QFontDatabase>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QStyle>
#include <QTableView>
#include <QToolButton>
#include <QVBoxLayout>

namespace streamview::app {

RawDataView::RawDataView(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("rawDataView"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* toolbar = new QWidget(this);
    auto* toolbarLayout = new QHBoxLayout(toolbar);
    toolbarLayout->setContentsMargins(6, 4, 6, 4);
    toolbarLayout->setSpacing(4);

    previousPageButton_ = new QToolButton(toolbar);
    previousPageButton_->setObjectName(QStringLiteral("previousPageButton"));
    previousPageButton_->setIcon(style()->standardIcon(QStyle::SP_ArrowLeft));
    previousPageButton_->setToolTip(tr("Previous source page"));
    toolbarLayout->addWidget(previousPageButton_);

    nextPageButton_ = new QToolButton(toolbar);
    nextPageButton_->setObjectName(QStringLiteral("nextPageButton"));
    nextPageButton_->setIcon(style()->standardIcon(QStyle::SP_ArrowRight));
    nextPageButton_->setToolTip(tr("Next source page"));
    toolbarLayout->addWidget(nextPageButton_);

    pageLabel_ = new QLabel(tr("No data"), toolbar);
    pageLabel_->setObjectName(QStringLiteral("rawPageLabel"));
    toolbarLayout->addWidget(pageLabel_);
    toolbarLayout->addStretch(1);

    auto* modeGroup = new QButtonGroup(this);
    modeGroup->setExclusive(true);
    const auto addModeButton = [toolbar, toolbarLayout, modeGroup](
                                   const QString& text,
                                   const QString& objectName,
                                   RawDisplayMode mode) {
        auto* button = new QToolButton(toolbar);
        button->setText(text);
        button->setObjectName(objectName);
        button->setCheckable(true);
        button->setAutoRaise(true);
        modeGroup->addButton(button, static_cast<int>(mode));
        toolbarLayout->addWidget(button);
        return button;
    };
    auto* hexButton = addModeButton(tr("Hex"), QStringLiteral("hexModeButton"),
                                    RawDisplayMode::Hex);
    addModeButton(tr("Binary"), QStringLiteral("binaryModeButton"), RawDisplayMode::Binary);
    addModeButton(tr("Combined"), QStringLiteral("combinedModeButton"),
                  RawDisplayMode::Combined);
    hexButton->setChecked(true);
    layout->addWidget(toolbar);

    model_ = new RawDataModel(this);
    table_ = new QTableView(this);
    table_->setObjectName(QStringLiteral("rawDataTable"));
    table_->setModel(model_);
    table_->setAlternatingRowColors(true);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionBehavior(QAbstractItemView::SelectItems);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setWordWrap(true);
    table_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    table_->verticalHeader()->setVisible(false);
    table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    layout->addWidget(table_, 1);

    connect(previousPageButton_, &QToolButton::clicked, this, &RawDataView::loadPreviousPage);
    connect(nextPageButton_, &QToolButton::clicked, this, &RawDataView::loadNextPage);
    connect(modeGroup, &QButtonGroup::idClicked, this, [this](int id) {
        setMode(static_cast<RawDisplayMode>(id));
    });

    updateCellSizes();
    updateControls();
}

bool RawDataView::setSource(const core::RandomAccessSource* source, QString* errorMessage) {
    const bool loaded = model_->setSource(source, errorMessage);
    updateControls();
    if (!loaded) {
        emit sourceReadFailed(model_->lastError());
    }
    return loaded;
}

bool RawDataView::setSource(const core::RandomAccessSource* source,
                            core::SourcePage preparedPage,
                            QString* errorMessage) {
    const bool loaded = model_->setSource(source, std::move(preparedPage), errorMessage);
    updateControls();
    if (!loaded) {
        emit sourceReadFailed(model_->lastError());
    }
    return loaded;
}

void RawDataView::clear() {
    model_->clear();
    updateControls();
}

void RawDataView::loadPreviousPage() {
    if (model_->pageIndex() == 0) {
        return;
    }
    QString errorMessage;
    if (!model_->loadPage(model_->pageIndex() - 1U, &errorMessage)) {
        emit sourceReadFailed(errorMessage);
    }
    updateControls();
}

void RawDataView::loadNextPage() {
    if (model_->pageIndex() + 1U >= model_->pageCount()) {
        return;
    }
    QString errorMessage;
    if (!model_->loadPage(model_->pageIndex() + 1U, &errorMessage)) {
        emit sourceReadFailed(errorMessage);
    }
    updateControls();
}

void RawDataView::setMode(RawDisplayMode mode) {
    model_->setDisplayMode(mode);
    updateCellSizes();
}

void RawDataView::updateControls() {
    const quint64 pageCount = model_->pageCount();
    if (pageCount == 0) {
        pageLabel_->setText(tr("No data"));
        previousPageButton_->setEnabled(false);
        nextPageButton_->setEnabled(false);
        return;
    }

    pageLabel_->setText(tr("Page %1 of %2")
                            .arg(model_->pageIndex() + 1U)
                            .arg(pageCount));
    previousPageButton_->setEnabled(model_->pageIndex() > 0);
    nextPageButton_->setEnabled(model_->pageIndex() + 1U < pageCount);
}

void RawDataView::updateCellSizes() {
    table_->setColumnWidth(RawDataModel::Offset, 152);
    int byteWidth = 40;
    int rowHeight = 24;
    if (model_->displayMode() == RawDisplayMode::Binary) {
        byteWidth = 82;
    } else if (model_->displayMode() == RawDisplayMode::Combined) {
        byteWidth = 82;
        rowHeight = 42;
    }
    for (int column = RawDataModel::FirstByte; column < RawDataModel::ColumnCount; ++column) {
        table_->setColumnWidth(column, byteWidth);
    }
    table_->verticalHeader()->setDefaultSectionSize(rowHeight);
}

} // namespace streamview::app
