#pragma once

#include <QtGlobal>

#include <compare>
#include <optional>
#include <utility>
#include <vector>

namespace streamview::core {

class SourceBitAddress final {
public:
    constexpr explicit SourceBitAddress(quint64 absoluteBitOffset = 0) noexcept
        : absoluteBitOffset_(absoluteBitOffset) {}

    [[nodiscard]] static std::optional<SourceBitAddress>
    fromByteAndBit(quint64 byteOffset, quint8 bitOffsetInByte) noexcept;

    [[nodiscard]] constexpr quint64 absoluteBitOffset() const noexcept {
        return absoluteBitOffset_;
    }

    [[nodiscard]] constexpr quint64 byteOffset() const noexcept {
        return absoluteBitOffset_ / 8;
    }

    [[nodiscard]] constexpr quint8 bitOffsetInByte() const noexcept {
        return static_cast<quint8>(absoluteBitOffset_ % 8);
    }

    friend constexpr auto operator<=>(const SourceBitAddress&, const SourceBitAddress&) = default;

private:
    quint64 absoluteBitOffset_ = 0;
};

class SourceSpan final {
public:
    [[nodiscard]] static std::optional<SourceSpan>
    create(SourceBitAddress start, quint64 bitLength) noexcept;

    [[nodiscard]] constexpr SourceBitAddress start() const noexcept { return start_; }
    [[nodiscard]] constexpr quint64 bitLength() const noexcept { return bitLength_; }
    [[nodiscard]] constexpr SourceBitAddress endExclusive() const noexcept {
        return SourceBitAddress(start_.absoluteBitOffset() + bitLength_);
    }

private:
    constexpr SourceSpan(SourceBitAddress start, quint64 bitLength) noexcept
        : start_(start), bitLength_(bitLength) {}

    SourceBitAddress start_;
    quint64 bitLength_ = 0;
};

class LogicalViewId final {
public:
    constexpr explicit LogicalViewId(quint64 value = 0) noexcept : value_(value) {}

    [[nodiscard]] constexpr quint64 value() const noexcept { return value_; }

    friend constexpr auto operator<=>(const LogicalViewId&, const LogicalViewId&) = default;

private:
    quint64 value_ = 0;
};

class LogicalBitAddress final {
public:
    constexpr LogicalBitAddress(LogicalViewId viewId, quint64 bitOffset) noexcept
        : viewId_(viewId), bitOffset_(bitOffset) {}

    [[nodiscard]] constexpr LogicalViewId viewId() const noexcept { return viewId_; }
    [[nodiscard]] constexpr quint64 bitOffset() const noexcept { return bitOffset_; }

    friend constexpr auto operator<=>(const LogicalBitAddress&, const LogicalBitAddress&) = default;

private:
    LogicalViewId viewId_;
    quint64 bitOffset_ = 0;
};

class LogicalRange final {
public:
    [[nodiscard]] static std::optional<LogicalRange>
    create(LogicalBitAddress start, quint64 bitLength) noexcept;

    [[nodiscard]] constexpr LogicalBitAddress start() const noexcept { return start_; }
    [[nodiscard]] constexpr quint64 bitLength() const noexcept { return bitLength_; }
    [[nodiscard]] constexpr quint64 endOffsetExclusive() const noexcept {
        return start_.bitOffset() + bitLength_;
    }

private:
    constexpr LogicalRange(LogicalBitAddress start, quint64 bitLength) noexcept
        : start_(start), bitLength_(bitLength) {}

    LogicalBitAddress start_;
    quint64 bitLength_ = 0;
};

class FieldLocation final {
public:
    [[nodiscard]] const LogicalRange& logicalRange() const noexcept { return logicalRange_; }
    [[nodiscard]] const std::vector<SourceSpan>& sourceSpans() const noexcept {
        return sourceSpans_;
    }

private:
    friend class SourceMapping;

    FieldLocation(LogicalRange logicalRange, std::vector<SourceSpan> sourceSpans)
        : logicalRange_(logicalRange), sourceSpans_(std::move(sourceSpans)) {}

    LogicalRange logicalRange_;
    std::vector<SourceSpan> sourceSpans_;
};

class SourceMapping final {
public:
    [[nodiscard]] static std::optional<SourceMapping>
    create(LogicalViewId viewId, std::vector<SourceSpan> sourceSpans);

    [[nodiscard]] LogicalViewId viewId() const noexcept { return viewId_; }
    [[nodiscard]] quint64 logicalBitLength() const noexcept { return logicalBitLength_; }
    [[nodiscard]] const std::vector<SourceSpan>& sourceSpans() const noexcept {
        return sourceSpans_;
    }

    [[nodiscard]] std::optional<FieldLocation> locate(const LogicalRange& range) const;

private:
    SourceMapping(LogicalViewId viewId,
                  quint64 logicalBitLength,
                  std::vector<SourceSpan> sourceSpans)
        : viewId_(viewId), logicalBitLength_(logicalBitLength),
          sourceSpans_(std::move(sourceSpans)) {}

    LogicalViewId viewId_;
    quint64 logicalBitLength_ = 0;
    std::vector<SourceSpan> sourceSpans_;
};

} // namespace streamview::core
