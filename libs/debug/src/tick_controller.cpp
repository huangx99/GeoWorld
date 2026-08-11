#include "geoworld/debug/tick_controller.hpp"

namespace geoworld::debug {

void TickController::pause() noexcept { paused_ = true; }

void TickController::resume() noexcept {
    paused_ = false;
    pending_steps_ = 0;
    target_tick_.reset();
}

void TickController::step(std::uint64_t count) noexcept {
    paused_ = true;
    pending_steps_ += count;
}

void TickController::run_to(std::uint64_t tick) noexcept {
    paused_ = true;
    target_tick_ = tick;
}

bool TickController::should_advance(std::uint64_t current_tick) noexcept {
    if (!paused_) {
        return true;
    }
    if (pending_steps_ != 0) {
        --pending_steps_;
        return true;
    }
    if (target_tick_.has_value() && current_tick < *target_tick_) {
        return true;
    }
    target_tick_.reset();
    return false;
}

} // namespace geoworld::debug
