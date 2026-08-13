#pragma once

#include "geoworld/gateway/control_params.hpp"
#include "geoworld/gateway/types.hpp"
#include "geoworld/protocol/replica.hpp"
#include "geoworld/protocol/wire.hpp"
#include "geoworld/projection/connection.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace geoworld::client {

struct StreamClientConfig {
    std::string control_address{"127.0.0.1:50051"};
    std::string stream_host{"127.0.0.1"};
    std::uint16_t stream_port{50052};
    std::string credential_token{"dev-observer-token"};
    std::string stream_path{std::string(gateway::default_stream_path)};
    // 单次阻塞 IO 的上限；超时即失败，不无限等待。
    std::chrono::milliseconds io_timeout{10'000};
    bool use_tls{};
    // 提供根证书文件则校验对端；不校验仅限 loopback 自签测试。
    std::string tls_root_certificate_file;
};

struct ClientSessionInfo {
    std::string session_id;
    std::string stream_ticket;
    std::string data_endpoint;
    std::uint64_t current_tick{};
    std::uint32_t control_api_version{};
    std::uint32_t data_schema_version{};
};

struct SubmitResult {
    gateway::ReceiptStatus status{gateway::ReceiptStatus::rejected};
    std::uint64_t ingress_sequence{};
    std::string error_code;
};

// 参考客户端：gRPC 控制面 + Beast WebSocket 数据面，全部阻塞调用并带超时。
// 世界帧的重建立在调用方（ReplicaAccumulator），本类只负责传输与ack。
class StreamClient {
public:
    explicit StreamClient(StreamClientConfig config);
    ~StreamClient();

    StreamClient(const StreamClient&) = delete;
    StreamClient& operator=(const StreamClient&) = delete;

    // ---- 控制面 ----
    [[nodiscard]] bool open_session(std::string& diagnostic);
    [[nodiscard]] bool update_subscription(const projection::Subscription& subscription,
                                           std::string& diagnostic);
    [[nodiscard]] bool acquire_ownership(foundation::WorldId target,
                                         const std::vector<std::string>& keys,
                                         std::uint64_t lease_until_tick,
                                         std::string& diagnostic);
    [[nodiscard]] bool release_ownership(foundation::WorldId target,
                                         const std::vector<std::string>& keys,
                                         std::string& diagnostic);
    [[nodiscard]] std::optional<SubmitResult> submit_set_property(
        foundation::WorldId target, const std::string& key,
        const world::PropertyValue& value, std::uint64_t client_sequence,
        std::uint64_t expected_object_version, std::string& diagnostic);
    [[nodiscard]] bool request_keyframe(std::string& diagnostic);
    [[nodiscard]] bool close_session(std::string& diagnostic);

    // ---- 数据面 ----
    // 完成 WebSocket 升级握手；ticket 为空时使用 open_session 签发的 ticket，
    // 重连时传入会话新签发的 ticket。
    [[nodiscard]] bool connect_stream(std::string& diagnostic,
                                      std::string_view ticket = {});
    // 读取下一服务端帧；连接关闭或超时返回空并填充诊断。
    [[nodiscard]] std::optional<protocol::WireFrame> read_frame(std::string& diagnostic);
    [[nodiscard]] bool send_ack(std::uint64_t stream_epoch, std::uint64_t snapshot_id,
                                std::string& diagnostic);
    [[nodiscard]] bool send_keyframe_request(std::string_view reason,
                                             std::string& diagnostic);
    // 测试与诊断支持：发送未经协议编码的原始二进制帧（畸形输入、超大帧场景）。
    [[nodiscard]] bool send_raw_bytes(std::span<const std::uint8_t> bytes,
                                      std::string& diagnostic);
    void disconnect_stream();

    [[nodiscard]] const ClientSessionInfo& session() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace geoworld::client
