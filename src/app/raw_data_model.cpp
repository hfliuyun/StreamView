#include "raw_data_model.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <utility>

namespace streamview::app {

RawDataModel::RawDataModel(QObject* parent) : QAbstractTableModel(parent) {
    page_.status = core::SourcePageStatus::EndOfSource;
}

bool RawDataModel::setSource(const core::RandomAccessSource* source, QString* errorMessage) {
    if (source == nullptr) {
        clear();
        if (errorMessage != nullptr) {
            errorMessage->clear();
        }
        return true;
    }

    const core::SourcePager pager(*source);
    core::SourcePage firstPage;
    firstPage.status = core::SourcePageStatus::EndOfSource;
    if (pager.pageCount() > 0) {
        firstPage = pager.loadPage(0);
    }
    return setSource(source, std::move(firstPage), errorMessage);
}

bool RawDataModel::setSource(const core::RandomAccessSource* source,
                             core::SourcePage preparedPage,
                             QString* errorMessage) {
    if (source == nullptr) {
        const QString message = QStringLiteral("A prepared page requires a media source");
        if (errorMessage != nullptr) {
            *errorMessage = message;
        }
        lastError_ = message;
        return false;
    }

    core::SourcePager newPager(*source);
    const quint64 pageCount = newPager.pageCount();
    const bool emptySourcePage = pageCount == 0 && preparedPage.pageIndex == 0 &&
                                 preparedPage.byteOffset == 0 && preparedPage.bytes.empty();
    const bool validPageIndex = pageCount > 0 && preparedPage.pageIndex < pageCount;
    if (!emptySourcePage && !validPageIndex) {
        const QString message = QStringLiteral("Prepared page is outside the media source");
        if (errorMessage != nullptr) {
            *errorMessage = message;
        }
        lastError_ = message;
        return false;
    }

    if (validPageIndex) {
        const quint64 expectedOffset =
            preparedPage.pageIndex * core::SourcePager::pageSizeBytes();
        const quint64 available = source->sizeBytes() - expectedOffset;
        const quint64 maximumBytes =
            std::min(core::SourcePager::pageSizeBytes(), available);
        if (preparedPage.byteOffset != expectedOffset ||
            preparedPage.bytes.size() > static_cast<std::size_t>(maximumBytes)) {
            const QString message = QStringLiteral("Prepared page does not match the media source");
            if (errorMessage != nullptr) {
                *errorMessage = message;
            }
            lastError_ = message;
            return false;
        }
        if (preparedPage.succeeded() &&
            preparedPage.bytes.size() != static_cast<std::size_t>(maximumBytes)) {
            const QString message =
                QStringLiteral("Prepared page does not cover its declared source range");
            if (errorMessage != nullptr) {
                *errorMessage = message;
            }
            lastError_ = message;
            return false;
        }
        const bool reachesEnd = expectedOffset + maximumBytes >= source->sizeBytes();
        const bool statusMatches =
            (reachesEnd && preparedPage.status == core::SourcePageStatus::EndOfSource) ||
            (!reachesEnd && preparedPage.status == core::SourcePageStatus::Ready) ||
            preparedPage.status == core::SourcePageStatus::Error;
        if (!statusMatches) {
            const QString message =
                QStringLiteral("Prepared page status does not match the media source");
            if (errorMessage != nullptr) {
                *errorMessage = message;
            }
            lastError_ = message;
            return false;
        }
    }

    beginResetModel();
    source_ = source;
    pager_ = newPager;
    page_ = std::move(preparedPage);
    highlightedSourceSpans_.clear();
    lastError_ = page_.errorMessage;
    endResetModel();

    if (errorMessage != nullptr) {
        *errorMessage = lastError_;
    }
    return page_.succeeded();
}

void RawDataModel::clear() {
    beginResetModel();
    source_ = nullptr;
    pager_.reset();
    page_ = {};
    page_.status = core::SourcePageStatus::EndOfSource;
    highlightedSourceSpans_.clear();
    lastError_.clear();
    endResetModel();
}

bool RawDataModel::loadPage(quint64 pageIndex, QString* errorMessage) {
    if (!pager_ || pageIndex >= pager_->pageCount()) {
        const QString message = QStringLiteral("Page index is out of range");
        if (errorMessage != nullptr) {
            *errorMessage = message;
        }
        lastError_ = message;
        return false;
    }

    return applyPage(pager_->loadPage(pageIndex), errorMessage);
}

quint64 RawDataModel::pageCount() const noexcept {
    return pager_ ? pager_->pageCount() : 0;
}

void RawDataModel::setDisplayMode(RawDisplayMode mode) {
    if (displayMode_ == mode) {
        return;
    }
    displayMode_ = mode;
    if (rowCount() > 0) {
        emit dataChanged(index(0, FirstByte),
                         index(rowCount() - 1, ColumnCount - 1),
                         {Qt::DisplayRole, Qt::ToolTipRole});
    }
}

void RawDataModel::setHighlightedSourceSpans(std::vector<core::SourceSpan> sourceSpans) {
    highlightedSourceSpans_ = std::move(sourceSpans);
    if (rowCount() > 0) {
        emit dataChanged(index(0, FirstByte),
                         index(rowCount() - 1, ColumnCount - 1),
                         {SelectedBitsRole});
    }
}

int RawDataModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) {
        return 0;
    }
    const auto rows = (page_.bytes.size() + ByteColumnCount - 1U) / ByteColumnCount;
    return rows > static_cast<std::size_t>(std::numeric_limits<int>::max())
               ? std::numeric_limits<int>::max()
               : static_cast<int>(rows);
}

int RawDataModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant RawDataModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount() ||
        index.column() < 0 || index.column() >= ColumnCount) {
        return {};
    }

    const quint64 byteOffset = page_.byteOffset +
                               static_cast<quint64>(index.row()) * ByteColumnCount;
    if (index.column() == Offset) {
        if (role == Qt::DisplayRole) {
            return QStringLiteral("0x%1").arg(byteOffset, 16, 16, QLatin1Char('0')).toUpper();
        }
        if (role == ByteOffsetRole) {
            return QVariant::fromValue<qulonglong>(byteOffset);
        }
        if (role == Qt::TextAlignmentRole) {
            return QVariant::fromValue(Qt::AlignRight | Qt::AlignVCenter);
        }
        return {};
    }

    const auto byteIndex = static_cast<std::size_t>(index.row()) * ByteColumnCount +
                           static_cast<std::size_t>(index.column() - FirstByte);
    if (byteIndex >= page_.bytes.size()) {
        return {};
    }

    const quint8 value = std::to_integer<quint8>(page_.bytes[byteIndex]);
    const quint64 cellOffset =
        byteOffset + static_cast<quint64>(index.column() - FirstByte);
    if (role == Qt::DisplayRole) {
        return formatByte(value);
    }
    if (role == ByteOffsetRole) {
        return QVariant::fromValue<qulonglong>(cellOffset);
    }
    if (role == ByteValueRole) {
        return value;
    }
    if (role == SelectedBitsRole) {
        return static_cast<unsigned int>(selectedBitsAt(cellOffset));
    }
    if (role == Qt::ToolTipRole) {
        return QStringLiteral("Byte 0x%1").arg(cellOffset, 16, 16, QLatin1Char('0')).toUpper();
    }
    if (role == Qt::TextAlignmentRole) {
        return QVariant::fromValue(Qt::AlignCenter);
    }
    return {};
}

QVariant RawDataModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole || section < 0 ||
        section >= ColumnCount) {
        return {};
    }
    if (section == Offset) {
        return tr("Offset");
    }
    return QStringLiteral("+%1").arg(section - FirstByte, 1, 16, QLatin1Char('0')).toUpper();
}

QString RawDataModel::formatHex(quint8 value) {
    return QStringLiteral("%1").arg(value, 2, 16, QLatin1Char('0')).toUpper();
}

QString RawDataModel::formatBinary(quint8 value) {
    QString result;
    result.reserve(8);
    for (int bit = 7; bit >= 0; --bit) {
        result.append((value & (quint8{1} << bit)) != 0U ? QLatin1Char('1') : QLatin1Char('0'));
    }
    return result;
}

QString RawDataModel::formatByte(quint8 value) const {
    const QString hex = formatHex(value);
    if (displayMode_ == RawDisplayMode::Hex) {
        return hex;
    }
    const QString binary = formatBinary(value);
    if (displayMode_ == RawDisplayMode::Binary) {
        return binary;
    }
    return hex + QLatin1Char('\n') + binary;
}

quint8 RawDataModel::selectedBitsAt(quint64 byteOffset) const noexcept {
    quint8 mask = 0;
    for (quint8 bitOffset = 0; bitOffset < 8U; ++bitOffset) {
        const auto sourceBit = core::SourceBitAddress::fromByteAndBit(byteOffset, bitOffset);
        if (!sourceBit) {
            continue;
        }
        for (const core::SourceSpan& span : highlightedSourceSpans_) {
            if (span.start() <= *sourceBit && *sourceBit < span.endExclusive()) {
                mask = static_cast<quint8>(mask | (quint8{0x80} >> bitOffset));
                break;
            }
        }
    }
    return mask;
}

bool RawDataModel::applyPage(core::SourcePage page, QString* errorMessage) {
    const bool succeeded = page.succeeded();
    const QString pageError = page.errorMessage;
    beginResetModel();
    page_ = std::move(page);
    lastError_ = pageError;
    endResetModel();
    if (errorMessage != nullptr) {
        *errorMessage = pageError;
    }
    return succeeded;
}

} // namespace streamview::app
