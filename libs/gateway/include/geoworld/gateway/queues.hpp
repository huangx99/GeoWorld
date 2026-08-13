#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

namespace geoworld::gateway {

using FrameBytes = std::vector<std::byte>;

struct QueuedFrame {
    std::uint64_t snapshot_id{};
    std::uint64_t baseline_snapshot_id{};
    bool is_keyframe{};
    FrameBytes bytes;
};

// 可合并状态队列：delta 相对同一已确认基线累积，较新的 delta 覆盖较旧的；
// 超限丢弃最旧时置 resync 标志（不能发送引用已丢弃基线的 delta）。
class StateQueue {
public:
    explicit StateQueue(std::size_t max_bytes) noexcept;

    void push(QueuedFrame frame);
    [[nodiscard]] std::optional<QueuedFrame> pop();
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::size_t bytes() const noexcept;
    [[nodiscard]] bool resync_required() const noexcept;
    void clear_resync() noexcept;

private:
    std::size_t max_bytes_;
    std::size_t bytes_{};
    bool resync_required_{};
    std::deque<QueuedFrame> frames_;
};

// 可靠队列：命令终态回执和协议错误不可丢弃；超限返回 false，由上层断开慢客户端。
class ReliableQueue {
public:
    explicit ReliableQueue(std::size_t max_bytes) noexcept;

    [[nodiscard]] bool push(FrameBytes bytes);
    [[nodiscard]] std::optional<FrameBytes> pop();
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::size_t bytes() const noexcept;

private:
    std::size_t max_bytes_;
    std::size_t bytes_{};
    std::deque<FrameBytes> frames_;
};

} // namespace geoworld::gateway
