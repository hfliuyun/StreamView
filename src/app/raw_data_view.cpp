#include "raw_data_view.h"

#include <QButtonGroup>
#include <QFontDatabase>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTableView>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <optional>

namespace streamview::app {

namespace {

class RawBitHighlightDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter,
               const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        QStyledItemDelegate::paint(painter, option, index);
        if (index.column() < RawDataModel::FirstByte) {
            return;
        }

        const auto selectedBits = static_cast<quint8>(
            index.data(RawDataModel::SelectedBitsRole).toUInt());
        if (selectedBits == 0U) {
            return;
        }

        painter->save();
        QColor highlight = option.palette.color(QPalette::Highlight);
        highlight.setAlpha(96);
        const QRectF cell = QRectF(option.rect).adjusted(1.0, 1.0, -1.0, -1.0);
        const qreal bitWidth = cell.width() / 8.0;
        for (quint8 bitOffset = 0; bitOffset < 8U; ++bitOffset) {
            if ((selectedBits & (quint8{0x80} >> bitOffset)) == 0U) {
                continue;
            }
            const QRectF bitRect(cell.left() + bitWidth * bitOffset,
                                 cell.top(), bitWidth, cell.height());
            painter->fillRect(bitRect, highlight);
        }
        painter->restore();
    }
};

} // namespace

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
    table_->setSelectionMode(QAbstractItemView::NoSelection);
    table_->setWordWrap(true);
    table_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    table_->setItemDelegate(new RawBitHighlightDelegate(table_));
    table_->verticalHeader()->setVisible(false);
    table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    table_->viewport()->installEventFilter(this);
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

void RawDataView::setSourceSelection(std::vector<core::SourceSpan> sourceSpans) {
    const auto firstNonEmpty = std::find_if(
        sourceSpans.cbegin(), sourceSpans.cend(),
        [](const core::SourceSpan& span) { return span.bitLength() != 0U; });
    const std::optional<quint64> targetByteOffset =
        firstNonEmpty == sourceSpans.cend()
            ? std::nullopt
            : std::optional<quint64>(firstNonEmpty->start().byteOffset());
    model_->setHighlightedSourceSpans(std::move(sourceSpans));
    if (!targetByteOffset) {
        return;
    }

    const quint64 targetPage = *targetByteOffset / core::SourcePager::pageSizeBytes();
    if (targetPage >= model_->pageCount()) {
        return;
    }
    if (targetPage != model_->pageIndex()) {
        QString errorMessage;
        if (!model_->loadPage(targetPage, &errorMessage)) {
            emit sourceReadFailed(errorMessage);
            return;
        }
        updateControls();
    }

    const quint64 byteIndex = *targetByteOffset - model_->pageByteOffset();
    const int row = static_cast<int>(byteIndex / RawDataModel::ByteColumnCount);
    const int column = RawDataModel::FirstByte +
                       static_cast<int>(byteIndex % RawDataModel::ByteColumnCount);
    table_->scrollTo(model_->index(row, column), QAbstractItemView::PositionAtCenter);
}

void RawDataView::clear() {
    model_->clear();
    updateControls();
}

bool RawDataView::eventFilter(QObject* watched, QEvent* event) {
    if (watched == table_->viewport() && event->type() == QEvent::MouseButtonPress) {
        const auto* mouseEvent = static_cast<QMouseEvent*>(event);
        if (mouseEvent->button() == Qt::LeftButton) {
            const QPoint position = mouseEvent->position().toPoint();
            const QModelIndex cell = table_->indexAt(position);
            if (cell.isValid() && cell.column() >= RawDataModel::FirstByte) {
                const QVariant byteOffsetValue = model_->data(cell, RawDataModel::ByteOffsetRole);
                const QRect cellRect = table_->visualRect(cell);
                if (byteOffsetValue.isValid() && cellRect.width() > 0) {
                    const int relativeX =
                        std::clamp(position.x() - cellRect.left(), 0, cellRect.width() - 1);
                    const auto bitOffset = static_cast<quint8>(
                        (relativeX * 8) / cellRect.width());
                    const auto sourceBit = core::SourceBitAddress::fromByteAndBit(
                        byteOffsetValue.toULongLong(), bitOffset);
                    if (sourceBit) {
                        emit sourceBitSelected(sourceBit->absoluteBitOffset());
                        table_->setCurrentIndex(cell);
                        return true;
                    }
                }
            }
        }
    }
    return QWidget::eventFilter(watched, event);
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
