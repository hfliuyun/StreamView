#pragma once

#include <QtGlobal>

#include <optional>

namespace streamview::rules::detail {

[[nodiscard]] constexpr quint8
h264StartCodePrefixLength(quint8 first,
                          quint8 second,
                          std::optional<quint8> third,
                          std::optional<quint8> fourth) noexcept {
    if (first != 0U || second != 0U || !third) {
        return 0;
    }
    if (*third == 1U) {
        return 3;
    }
    if (*third != 0U || !fourth || *fourth != 1U) {
        return 0;
    }
    return 4;
}

} // namespace streamview::rules::detail
