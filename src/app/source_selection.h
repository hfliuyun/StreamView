#pragma once

#include <streamview/core/coordinates.h>

#include <QString>

#include <vector>

namespace streamview::app {

struct SourceSelection final {
    QString sourceIdentity;
    std::vector<core::SourceSpan> sourceSpans;

    [[nodiscard]] bool isEmpty() const noexcept {
        return sourceIdentity.isEmpty() || sourceSpans.empty();
    }
};

} // namespace streamview::app
