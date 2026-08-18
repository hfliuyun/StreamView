#pragma once

#include <streamview/core/coordinates.h>
#include <streamview/core/source.h>

#include <QString>
#include <QtGlobal>

#include <span>

namespace streamview::core {

class BoundedSourceView final : public RandomAccessSource {
public:
    BoundedSourceView(const RandomAccessSource& baseSource,
                      SourceMapping mapping,
                      quint64 sizeBytes);

    [[nodiscard]] quint64 sizeBytes() const noexcept override { return sizeBytes_; }
    [[nodiscard]] QString identity() const override { return baseSource_->identity(); }
    [[nodiscard]] const SourceMapping& mapping() const noexcept { return mapping_; }
    [[nodiscard]] const RandomAccessSource& baseSource() const noexcept { return *baseSource_; }

    [[nodiscard]] SourceReadResult
    readAt(quint64 byteOffset, std::span<std::byte> destination) const override;

private:
    const RandomAccessSource* baseSource_ = nullptr;
    SourceMapping mapping_;
    quint64 sizeBytes_ = 0;
};

} // namespace streamview::core
