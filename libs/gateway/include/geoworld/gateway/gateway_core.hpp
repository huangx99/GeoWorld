#pragma once

#include "geoworld/gateway/auth.hpp"
#include "geoworld/gateway/config.hpp"
#include "geoworld/gateway/ownership.hpp"
#include "geoworld/gateway/queues.hpp"
#include "geoworld/gateway/session.hpp"
#include "geoworld/gateway/types.hpp"
#include "geoworld/projection/engine.hpp"
#include "geoworld/simulation/command_buffer.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace geoworld::gateway {

// 传输无关的 Gateway 核心：会话、虚拟连接、背压队列、命令接入与回执。
// 帧编码经注入的编码器完成（由组合方绑定 protocol codec），本类不出现协议类型。
class GatewayCore {
public:
    using CommandSubmitter = std::function<std::uint64_t(
        std::uint64_t, simulation::CommandPayload, simulation::CommandMeta)>;
    using FrameEncoder = std::function<FrameBytes(const projection::StateFrame&)>;
    using ReceiptEncoder = std::function<FrameBytes(const CommandReceipt&)>;
    using HeartbeatEncoder = std::function<FrameBytes(std::uint64_t, std::uint64_t)>;

    struct OpenSessionResult {
        GatewayError error{GatewayError::none};
        OpenedSession session{};
    };

    GatewayCore(GatewayConfig config, projection::ProjectionEngine& engine,
                std::shared_ptr<AuthenticationProvider> authentication,
                std::shared_ptr<AuthorizationPolicy> authorization,
                SteadyClock clock, TokenGenerator tokens,
                std::uint32_t control_api_version, std::uint32_t data_schema_version);

    void set_command_submitter(CommandSubmitter submitter);
    void set_frame_encoder(FrameEncoder encoder);
    void set_receipt_encoder(ReceiptEncoder encoder);
    void set_heartbeat_encoder(HeartbeatEncoder encoder);
    // 注入共享线程池后，pump 的帧编码按连接并行；输出与串行逐字节一致。
    // 并发边界：注入线程池后编码器会被多线程并发调用，必须线程安全
    // （内置 make_frame_encoder 满足）；engine_.next_frame 与队列写入保持单线程。
    void set_thread_pool(std::shared_ptr<foundation::ThreadPool> pool);

    // ---- 控制面 ----
    [[nodiscard]] OpenSessionResult open_session(std::string_view credential_token,
                                                 std::uint32_t control_min,
                                                 std::uint32_t control_max,
                                                 std::uint32_t data_min,
                                                 std::uint32_t data_max,
                                                 std::uint64_t current_tick);
    [[nodiscard]] bool close_session(SessionId id);
    [[nodiscard]] GatewayError update_subscription(
        SessionId id, const projection::Subscription& subscription);
    [[nodiscard]] GatewayError acquire_ownership(
        SessionId id, foundation::WorldId target, const std::vector<std::string>& keys,
        std::uint64_t lease_until_tick, std::uint64_t current_tick);
    [[nodiscard]] GatewayError release_ownership(
        SessionId id, foundation::WorldId target, const std::vector<std::string>& keys);
    [[nodiscard]] std::pair<GatewayError, CommandReceipt> submit_command(
        SessionId id, const ExternalCommand& command, std::uint64_t current_tick);
    [[nodiscard]] GatewayError request_keyframe(SessionId id);

    // ---- 数据面 ----
    // 为既有会话签发新的一次性 ticket（重连数据链路必须换新 ticket）。
    [[nodiscard]] std::optional<std::string> issue_stream_ticket(SessionId id);
    // ticket 校验即消费；成功后建立虚拟连接并绑定会话订阅。
    [[nodiscard]] std::optional<projection::ConnectionId> attach_stream(
        projection::ConnectionId connection, std::string_view ticket);
    void detach_stream(projection::ConnectionId connection);
    [[nodiscard]] bool inbound_ack(projection::ConnectionId connection,
                                   std::uint64_t stream_epoch,
                                   std::uint64_t snapshot_id);
    [[nodiscard]] bool inbound_keyframe_request(projection::ConnectionId connection);

    // 每个 tick 在投影观察之后调用：拉取帧、编码、入队、心跳与 ack 超时检查。
    void pump(std::uint64_t tick);
    // 命令缓冲 apply 结果回执：产生终态回执并写入可靠队列。
    void on_commands_applied(const simulation::ApplyReport& report);

    // IO 侧取待发字节：可靠队列优先于状态队列。
    [[nodiscard]] std::optional<FrameBytes> next_outbound(projection::ConnectionId connection);
    // 可靠队列溢出等必须断开的情况查询。
    [[nodiscard]] bool must_disconnect(projection::ConnectionId connection) const;
    [[nodiscard]] GatewayError disconnect_reason(projection::ConnectionId connection) const;

    [[nodiscard]] std::size_t connection_count() const noexcept;
    // 上一次 pump 入队的 keyframe 帧数：供基准按 tick 类型分解延迟。
    [[nodiscard]] std::uint64_t last_pump_keyframe_count() const noexcept {
        return last_pump_keyframe_count_;
    }
    [[nodiscard]] const SessionManager& sessions() const noexcept;

private:
    struct ConnectionState {
        SessionId session;
        StateQueue state_queue;
        ReliableQueue reliable_queue;
        std::uint64_t last_ack_tick{};
        std::uint64_t last_heartbeat_tick{};
        bool must_disconnect{};
        GatewayError disconnect_reason{GatewayError::none};

        ConnectionState(SessionId session_id, std::size_t state_bytes,
                        std::size_t reliable_bytes);
    };

    struct PendingCommand {
        SessionId session;
        std::uint64_t client_sequence{};
    };

    [[nodiscard]] ConnectionState* find_connection(projection::ConnectionId connection) noexcept;
    void mark_disconnect(ConnectionState& state, GatewayError reason);

    GatewayConfig config_;
    projection::ProjectionEngine& engine_;
    std::shared_ptr<AuthenticationProvider> authentication_;
    std::shared_ptr<AuthorizationPolicy> authorization_;
    SessionManager sessions_;
    OwnershipRegistry ownership_;
    std::uint32_t control_api_version_{};
    std::uint32_t data_schema_version_{};
    std::uint64_t ingress_sequence_{};
    CommandSubmitter submitter_;
    FrameEncoder frame_encoder_;
    ReceiptEncoder receipt_encoder_;
    HeartbeatEncoder heartbeat_encoder_;
    std::unordered_map<projection::ConnectionId, ConnectionState,
                       projection::ConnectionIdHash> connections_;
    std::unordered_map<std::uint64_t, PendingCommand> pending_commands_;
    std::unordered_map<SessionId, projection::Subscription, SessionIdHash>
        pending_subscriptions_;
    std::uint64_t last_pump_tick_{};
    std::uint64_t last_pump_keyframe_count_{};
    std::shared_ptr<foundation::ThreadPool> thread_pool_;
    std::vector<projection::ConnectionId> pump_order_;
    std::vector<std::optional<projection::StateFrame>> pump_frames_;
    std::vector<FrameBytes> pump_encoded_;
    // 并行编码段提取的帧元数据（0=无帧, 1=keyframe, 2=delta）与 delta 基线快照号。
    std::vector<char> pump_frame_kind_;
    std::vector<std::uint64_t> pump_frame_baseline_;
};

} // namespace geoworld::gateway
