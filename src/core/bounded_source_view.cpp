#include <streamview/core/bounded_source_view.h>

#include <algorithm>
#include <limits>

namespace streamview::core {

BoundedSourceView::BoundedSourceView(const RandomAccessSource& baseSource,
                                     SourceMapping mapping,
                                     quint64 sizeBytes)
    : baseSource_(&baseSource), mapping_(std::move(mapping)), sizeBytes_(sizeBytes) {}

SourceReadResult
BoundedSourceView::readAt(quint64 byteOffset, std::span<std::byte> destination) const {
    if (destination.empty()) {
        return {SourceReadStatus::Complete, 0, {}};
    }
    if (byteOffset >= sizeBytes_) {
        return {SourceReadStatus::EndOfSource, 0, {}};
    }
    const quint64 available = sizeBytes_ - byteOffset;
    const std::size_t count = static_cast<std::size_t>(
        std::min(static_cast<quint64>(destination.size()), available));

    constexpr quint64 maxByteCoordinate = std::numeric_limits<quint64>::max() / 8U;
    if (byteOffset > maxByteCoordinate ||
        static_cast<quint64>(count) > maxByteCoordinate ||
        byteOffset + static_cast<quint64>(count) > maxByteCoordinate) {
        return {SourceReadStatus::Error, 0,
                QStringLiteral("Bounded source view coordinate overflow")};
    }

    const auto range = LogicalRange::create(
        LogicalBitAddress(mapping_.viewId(), byteOffset * 8U),
        static_cast<quint64>(count) * 8U);
    if (!range) {
        return {SourceReadStatus::Error, 0,
                QStringLiteral("Invalid range in bounded source view")};
    }

    const auto locateRes = mapping_.locate(*range);
    if (!locateRes.has_value() || locateRes->sourceSpans().empty()) {
        return {SourceReadStatus::Error, 0,
                QStringLiteral("Failed to locate spans in bounded source view")};
    }

    std::size_t bytesFilled = 0;
    for (const auto& span : locateRes->sourceSpans()) {
        if (span.start().bitOffsetInByte() != 0 || (span.bitLength() % 8U) != 0) {
            return {SourceReadStatus::Error, bytesFilled,
                    QStringLiteral("Bounded source view span is not byte-aligned")};
        }
        const quint64 spanStartByte = span.start().byteOffset();
        const std::size_t spanLength = static_cast<std::size_t>(span.bitLength() / 8U);
        const std::size_t toRead = std::min(spanLength, count - bytesFilled);
        const auto readRes = baseSource_->readAt(
            spanStartByte, destination.subspan(bytesFilled, toRead));
        if (readRes.bytesRead > toRead) {
            return {SourceReadStatus::Error,
                    bytesFilled,
                    QStringLiteral("Bounded source view received an oversized read")};
        }
        bytesFilled += readRes.bytesRead;
        if (!readRes.complete()) {
            return {readRes.status, bytesFilled, readRes.errorMessage};
        }
        if (readRes.bytesRead != toRead) {
            return {SourceReadStatus::Error,
                    bytesFilled,
                    QStringLiteral("Bounded source view received an incomplete successful read")};
        }
        if (bytesFilled >= count) {
            break;
        }
    }

    return {bytesFilled == count && count == destination.size()
                ? SourceReadStatus::Complete
                : SourceReadStatus::EndOfSource,
            bytesFilled,
            {}};
}

} // namespace streamview::core
