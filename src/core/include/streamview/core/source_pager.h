#pragma once

#include <streamview/core/source.h>

#include <QString>
#include <QtGlobal>

#include <cstddef>
#include <vector>

namespace streamview::core {

enum class SourcePageStatus : quint8 {
    Ready,
    EndOfSource,
    Error,
};

struct SourcePage final {
    SourcePageStatus status = SourcePageStatus::Error;
    quint64 pageIndex = 0;
    quint64 byteOffset = 0;
    std::vector<std::byte> bytes;
    QString errorMessage;

    [[nodiscard]] bool succeeded() const noexcept { return status != SourcePageStatus::Error; }
};

class SourcePager final {
public:
    explicit SourcePager(const RandomAccessSource& source) noexcept : source_(&source) {}

    [[nodiscard]] static constexpr quint64 pageSizeBytes() noexcept { return 64U * 1024U; }
    [[nodiscard]] quint64 pageCount() const noexcept;
    [[nodiscard]] SourcePage loadPage(quint64 pageIndex) const;

private:
    const RandomAccessSource* source_ = nullptr;
};

} // namespace streamview::core
