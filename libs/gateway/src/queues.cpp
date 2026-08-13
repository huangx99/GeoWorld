#include "geoworld/gateway/queues.hpp"

namespace geoworld::gateway {

StateQueue::StateQueue(std::size_t max_bytes) noexcept : max_bytes_(max_bytes) {}

void StateQueue::push(QueuedFrame frame) {
    const std::size_t frame_bytes = frame.bytes.size();
    if (frame.is_keyframe) {
        // keyframe 替换全部历史视图，既有状态帧全部失效。
        frames_.clear();
        bytes_ = 0;
    } else {
        // 同一已确认基线的较新 delta 覆盖较旧 delta（update 携带完整实体）。
        for (auto existing = frames_.begin(); existing != frames_.end();) {
            if (!existing->is_keyframe
                && existing->baseline_snapshot_id == frame.baseline_snapshot_id) {
                bytes_ -= existing->bytes.size();
                existing = frames_.erase(existing);
            } else {
                ++existing;
            }
        }
    }
    bytes_ += frame_bytes;
    frames_.push_back(std::move(frame));

    while (bytes_ > max_bytes_ && frames_.size() > 1) {
        bytes_ -= frames_.front().bytes.size();
        frames_.pop_front();
        resync_required_ = true;
    }
    // 单帧即超限时保留该帧，由上层决定是否断开；帧硬上限由协议层 max_frame_bytes 拦截。
}

[[nodiscard]] std::optional<QueuedFrame> StateQueue::pop() {
    if (frames_.empty()) {
        return std::nullopt;
    }
    QueuedFrame frame = std::move(frames_.front());
    bytes_ -= frame.bytes.size();
    frames_.pop_front();
    return frame;
}

[[nodiscard]] bool StateQueue::empty() const noexcept {
    return frames_.empty();
}

[[nodiscard]] std::size_t StateQueue::size() const noexcept {
    return frames_.size();
}

[[nodiscard]] std::size_t StateQueue::bytes() const noexcept {
    return bytes_;
}

[[nodiscard]] bool StateQueue::resync_required() const noexcept {
    return resync_required_;
}

void StateQueue::clear_resync() noexcept {
    resync_required_ = false;
}

ReliableQueue::ReliableQueue(std::size_t max_bytes) noexcept : max_bytes_(max_bytes) {}

[[nodiscard]] bool ReliableQueue::push(FrameBytes bytes) {
    if (bytes_ + bytes.size() > max_bytes_) {
        return false;
    }
    bytes_ += bytes.size();
    frames_.push_back(std::move(bytes));
    return true;
}

[[nodiscard]] std::optional<FrameBytes> ReliableQueue::pop() {
    if (frames_.empty()) {
        return std::nullopt;
    }
    FrameBytes frame = std::move(frames_.front());
    bytes_ -= frame.size();
    frames_.pop_front();
    return frame;
}

[[nodiscard]] bool ReliableQueue::empty() const noexcept {
    return frames_.empty();
}

[[nodiscard]] std::size_t ReliableQueue::size() const noexcept {
    return frames_.size();
}

[[nodiscard]] std::size_t ReliableQueue::bytes() const noexcept {
    return bytes_;
}

} // namespace geoworld::gateway
