#pragma once

#include <atomic>
#include <memory>
#include <utility>

namespace streamview::core {

namespace detail {

struct CancellationState final {
    std::atomic_bool requested{false};
};

} // namespace detail

class CancellationToken final {
public:
    [[nodiscard]] bool isCancellationRequested() const noexcept {
        return state_ && state_->requested.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool canBeCancelled() const noexcept { return state_ != nullptr; }

private:
    friend class CancellationSource;

    explicit CancellationToken(std::shared_ptr<detail::CancellationState> state) noexcept
        : state_(std::move(state)) {}

    std::shared_ptr<detail::CancellationState> state_;
};

class CancellationSource final {
public:
    [[nodiscard]] CancellationToken token() const noexcept {
        return CancellationToken(state_);
    }

    [[nodiscard]] bool requestCancellation() noexcept {
        if (!state_) {
            return false;
        }

        bool expected = false;
        return state_->requested.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel, std::memory_order_acquire);
    }

    [[nodiscard]] bool isCancellationRequested() const noexcept {
        return state_ && state_->requested.load(std::memory_order_acquire);
    }

private:
    std::shared_ptr<detail::CancellationState> state_ =
        std::make_shared<detail::CancellationState>();
};

} // namespace streamview::core
