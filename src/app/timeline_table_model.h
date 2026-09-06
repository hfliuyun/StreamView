#pragma once

#include <streamview/core/sample_descriptor.h>

#include <QAbstractTableModel>
#include <vector>

namespace streamview::app {

class TimelineTableModel final : public QAbstractTableModel {
    Q_OBJECT

public:
    enum Column : int {
        SampleIndex = 0,
        SyncType,
        Dts,
        Pts,
        Duration,
        SizeBytes,
        BitOffset,
        DescriptionIndex,
        ColumnCount,
    };

    explicit TimelineTableModel(QObject* parent = nullptr);
    ~TimelineTableModel() override = default;

    TimelineTableModel(const TimelineTableModel&) = delete;
    TimelineTableModel& operator=(const TimelineTableModel&) = delete;
    TimelineTableModel(TimelineTableModel&&) = delete;
    TimelineTableModel& operator=(TimelineTableModel&&) = delete;

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    [[nodiscard]] QVariant headerData(int section,
                                      Qt::Orientation orientation,
                                      int role = Qt::DisplayRole) const override;

    void setSamples(std::vector<core::SampleDescriptor> samples, quint32 timescale);
    void clear();

    [[nodiscard]] const core::SampleDescriptor* sampleAt(int row) const noexcept;
    [[nodiscard]] const std::vector<core::SampleDescriptor>& samples() const noexcept {
        return samples_;
    }
    [[nodiscard]] quint32 timescale() const noexcept { return timescale_; }

private:
    std::vector<core::SampleDescriptor> samples_;
    quint32 timescale_ = 1;
};

} // namespace streamview::app
