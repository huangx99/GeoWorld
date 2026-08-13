#pragma once

#include "geoworld/gateway/control_params.hpp"
#include "geoworld/protocol/limits.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace geoworld::client {

// thin 客户端每连接统计：全部为原子，主线程可在 join 后或运行中读取。
struct ThinConnectionStats {
    std::atomic<std::uint64_t> frames{};
    std::atomic<std::uint64_t> keyframes{};
    std::atomic<std::uint64_t> deltas{};
    std::atomic<std::uint64_t> heartbeats{};
    std::atomic<std::uint64_t> reliable_events{};
    std::atomic<std::uint64_t> acks_sent{};
    // 帧类型/标量字段提取失败次数；>0 说明链路出现了非预期帧。
    std::atomic<std::uint64_t> parse_failures{};
    std::atomic<bool> established{false};
    std::atomic<bool> failed{false};
    // 对端关闭或本地错误后置位（含慢连接被服务器 GWG105 断开）。
    std::atomic<bool> closed{false};
};

struct ThinClientPoolConfig {
    std::string host{"127.0.0.1"};
    std::uint16_t port{};
    std::string path{std::string(gateway::default_stream_path)};
    std::size_t io_threads{4};
    std::chrono::milliseconds connect_timeout{10'000};
    std::size_t max_frame_bytes{protocol::ProtocolLimits{}.max_frame_bytes};
};

// thin 扇出客户端池：真实 Beast WebSocket 连接（ticket 握手、subprotocol
// 校验、二进制帧，走与阻塞参考客户端相同的 StreamTransport 路径），但用少量
// 线程各自驱动独立 io_context，以异步方式承载全部连接；收到状态帧只做最便宜
// 的 FlatBuffer 标量提取（union 类型 + stream_epoch/snapshot_id，不做 Verifier
// 全遍历、不构造 wire 结构、不做 replica 重建）并立即回 ack，ack 流语义与
// 完整客户端一致。用途：loopback 基准中剥离客户端侧 decode/replica 的 CPU
// 开销，隔离服务端扇出能力。
class ThinClientPool {
public:
    ThinClientPool(ThinClientPoolConfig config, std::size_t connection_count);
    ~ThinClientPool();

    ThinClientPool(const ThinClientPool&) = delete;
    ThinClientPool& operator=(const ThinClientPool&) = delete;

    // 启动 io 线程；之后才能 async_connect。
    void start();
    // 主线程逐个调用：异步发起 index 连接的 resolve+connect+WebSocket 握手，
    // 完成或失败经 stats(index).established/failed 观察。逐个发起可让调用方
    // 把服务器分配的 ConnectionId 与连接下标对齐。
    void async_connect(std::size_t index, std::string ticket);
    // 慢连接语义：停止/恢复发起下一次 async_read（ack 写不暂停）。
    void pause_reads(std::size_t index);
    void resume_reads(std::size_t index);
    [[nodiscard]] const ThinConnectionStats& stats(std::size_t index) const;
    // 告知池进入收尾：之后连接被关闭视为正常 teardown，不再计入 closed。
    // 在服务器侧 shutdown 之前调用，语义与阻塞客户端的 stopping 标志一致。
    void set_stopping();
    // 关闭全部连接并停 io 线程；幂等。
    void shutdown();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace geoworld::client
