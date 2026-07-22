#pragma once

#include <streamview/core/source_pager.h>

#include <QAbstractTableModel>
#include <QString>
#include <QVariant>
#include <QtGlobal>

#include <cstddef>
#include <optional>

namespace streamview::app {

enum class RawDisplayMode : quint8 {
    Hex,
    Binary,
    Combined,
};

class RawDataModel final : public QAbstractTableModel {
    Q_OBJECT

public:
    enum Column : int {
        Offset = 0,
        FirstByte = 1,
        ByteColumnCount = 16,
        ColumnCount = FirstByte + ByteColumnCount,
    };

    enum DataRole : int {
        ByteOffsetRole = Qt::UserRole + 1,
        ByteValueRole,
    };

    explicit RawDataModel(QObject* parent = nullptr);

    [[nodiscard]] bool setSource(const core::RandomAccessSource* source,
                                 QString* errorMessage = nullptr);
    [[nodiscard]] bool setSource(const core::RandomAccessSource* source,
                                 core::SourcePage preparedPage,
                                 QString* errorMessage = nullptr);
    void clear();

    [[nodiscard]] bool loadPage(quint64 pageIndex, QString* errorMessage = nullptr);
    [[nodiscard]] quint64 pageCount() const noexcept;
    [[nodiscard]] quint64 pageIndex() const noexcept { return page_.pageIndex; }
    [[nodiscard]] quint64 pageByteOffset() const noexcept { return page_.byteOffset; }
    [[nodiscard]] RawDisplayMode displayMode() const noexcept { return displayMode_; }
    void setDisplayMode(RawDisplayMode mode);
    [[nodiscard]] QString lastError() const { return lastError_; }

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index,
                                int role = Qt::DisplayRole) const override;
    [[nodiscard]] QVariant headerData(int section, Qt::Orientation orientation,
                                      int role = Qt::DisplayRole) const override;

private:
    [[nodiscard]] static QString formatHex(quint8 value);
    [[nodiscard]] static QString formatBinary(quint8 value);
    [[nodiscard]] QString formatByte(quint8 value) const;
    [[nodiscard]] bool applyPage(core::SourcePage page, QString* errorMessage);

    const core::RandomAccessSource* source_ = nullptr;
    std::optional<core::SourcePager> pager_;
    core::SourcePage page_;
    RawDisplayMode displayMode_ = RawDisplayMode::Hex;
    QString lastError_;
};

} // namespace streamview::app
