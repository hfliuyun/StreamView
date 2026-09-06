#include "timeline_table_model.h"

namespace streamview::app {

TimelineTableModel::TimelineTableModel(QObject* parent)
    : QAbstractTableModel(parent) {}

int TimelineTableModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) {
        return 0;
    }
    return static_cast<int>(samples_.size());
}

int TimelineTableModel::columnCount(const QModelIndex& parent) const {
    if (parent.isValid()) {
        return 0;
    }
    return ColumnCount;
}

QVariant TimelineTableModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 ||
        static_cast<std::size_t>(index.row()) >= samples_.size()) {
        return {};
    }

    const auto& sample = samples_[static_cast<std::size_t>(index.row())];

    if (role == Qt::TextAlignmentRole) {
        if (index.column() == SyncType) {
            return static_cast<int>(Qt::AlignCenter);
        }
        return static_cast<int>(Qt::AlignRight | Qt::AlignVCenter);
    }

    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case SampleIndex:
            return QString::number(sample.sampleIndex);
        case SyncType:
            return sample.isSyncSample ? QStringLiteral("[Sync]") : QStringLiteral("-");
        case Dts:
            return QString::number(sample.dts);
        case Pts:
            return QString::number(sample.pts);
        case Duration:
            return QString::number(sample.duration);
        case SizeBytes: {
            quint64 totalBytes = 0;
            for (const auto& span : sample.sourceSpans) {
                totalBytes += span.bitLength() / 8U;
            }
            return QString::number(totalBytes);
        }
        case BitOffset: {
            if (sample.sourceSpans.empty()) {
                return QStringLiteral("-");
            }
            return QString::number(sample.sourceSpans.front().start().absoluteBitOffset());
        }
        case DescriptionIndex:
            return QString::number(sample.sampleDescriptionIndex);
        default:
            return {};
        }
    }

    if (role == Qt::ToolTipRole) {
        if (index.column() == SyncType) {
            return sample.isSyncSample ? tr("Sync sample / Keyframe (random access point)")
                                       : tr("Non-sync sample / Delta frame");
        }
        if (index.column() == Pts && timescale_ > 0) {
            const double seconds = static_cast<double>(sample.pts) / static_cast<double>(timescale_);
            return tr("PTS: %1 s").arg(seconds, 0, 'f', 3);
        }
        if (index.column() == Dts && timescale_ > 0) {
            const double seconds = static_cast<double>(sample.dts) / static_cast<double>(timescale_);
            return tr("DTS: %1 s").arg(seconds, 0, 'f', 3);
        }
    }

    return {};
}

QVariant TimelineTableModel::headerData(int section,
                                        Qt::Orientation orientation,
                                        int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
        return {};
    }

    switch (section) {
    case SampleIndex:
        return tr("Sample #");
    case SyncType:
        return tr("Type");
    case Dts:
        return tr("DTS");
    case Pts:
        return tr("PTS");
    case Duration:
        return tr("Duration");
    case SizeBytes:
        return tr("Size (B)");
    case BitOffset:
        return tr("Bit Offset");
    case DescriptionIndex:
        return tr("Desc Idx");
    default:
        return {};
    }
}

void TimelineTableModel::setSamples(std::vector<core::SampleDescriptor> samples,
                                    quint32 timescale) {
    beginResetModel();
    samples_ = std::move(samples);
    timescale_ = timescale == 0 ? 1 : timescale;
    endResetModel();
}

void TimelineTableModel::clear() {
    beginResetModel();
    samples_.clear();
    timescale_ = 1;
    endResetModel();
}

const core::SampleDescriptor* TimelineTableModel::sampleAt(int row) const noexcept {
    if (row < 0 || static_cast<std::size_t>(row) >= samples_.size()) {
        return nullptr;
    }
    return &samples_[static_cast<std::size_t>(row)];
}

} // namespace streamview::app
