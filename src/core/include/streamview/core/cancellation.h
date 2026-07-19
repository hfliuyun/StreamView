#pragma once

#include <stop_token>

namespace streamview::core {

class CancellationToken final {
public:
    [[nodiscard]] bool isCancellationRequested() const noexcept {
        return token_.stop_requested();
    }

    [[nodiscard]] bool canBeCancelled() const noexcept { return token_.stop_possible(); }

private:
    friend class CancellationSource;

    explicit CancellationToken(std::stop_token token) noexcept : token_(token) {}

    std::stop_token token_;
};

class CancellationSource final {
public:
    [[nodiscard]] CancellationToken token() const noexcept {
        return CancellationToken(source_.get_token());
    }

    [[nodiscard]] bool requestCancellation() noexcept { return source_.request_stop(); }
    [[nodiscard]] bool isCancellationRequested() const noexcept {
        return source_.stop_requested();
    }

private:
    std::stop_source source_;
};

} // namespace streamview::core
