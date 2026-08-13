#include "geoworld/gateway/gateway_core.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace geoworld::gateway {

GatewayCore::ConnectionState::ConnectionState(SessionId session_id,
                                              std::size_t state_bytes,
                                              std::size_t reliable_bytes)
    : session(session_id), state_queue(state_bytes), reliable_queue(reliable_bytes) {}

GatewayCore::GatewayCore(GatewayConfig config, projection::ProjectionEngine& engine,
                         std::shared_ptr<AuthenticationProvider> authentication,
                         std::shared_ptr<AuthorizationPolicy> authorization,
                         SteadyClock clock, TokenGenerator tokens,
                         std::uint32_t control_api_version,
                         std::uint32_t data_schema_version)
    : config_(std::move(config)),
      engine_(engine),
      authentication_(std::move(authentication)),
      authorization_(std::move(authorization)),
      sessions_(SessionManager::Config{config_.stream_ticket_ttl_seconds,
                                       config_.command_rate_per_session,
                                       config_.max_sessions},
                std::move(clock), std::move(tokens)),
      control_api_version_(control_api_version),
      data_schema_version_(data_schema_version) {}

void GatewayCore::set_command_submitter(CommandSubmitter submitter) {
    submitter_ = std::move(submitter);
}

void GatewayCore::set_frame_encoder(FrameEncoder encoder) {
    frame_encoder_ = std::move(encoder);
}

void GatewayCore::set_receipt_encoder(ReceiptEncoder encoder) {
    receipt_encoder_ = std::move(encoder);
}

void GatewayCore::set_heartbeat_encoder(HeartbeatEncoder encoder) {
    heartbeat_encoder_ = std::move(encoder);
}

[[nodiscard]] GatewayCore::OpenSessionResult GatewayCore::open_session(
    std::string_view credential_token, std::uint32_t control_min,
    std::uint32_t control_max, std::uint32_t data_min, std::uint32_t data_max,
    std::uint64_t current_tick) {
    // 版本协商：双方共同最高版本，无交集拒绝。
    OpenSessionResult result;
    if (control_max < control_api_version_ || control_min > control_api_version_
        || data_max < data_schema_version_ || data_min > data_schema_version_) {
        result.error = GatewayError::protocol_incompatible;
        return result;
    }

    const std::optional<Principal> principal = authentication_->authenticate(credential_token);
    if (!principal.has_value()) {
        result.error = GatewayError::auth_failed;
        return result;
    }
    if (!authorization_->can_observe(*principal)) {
        result.error = GatewayError::permission_denied;
        return result;
    }

    const SessionId id = sessions_.open(*principal, control_api_version_,
                                        data_schema_version_, current_tick);
    if (!id.valid()) {
        result.error = GatewayError::limit_exceeded;
        return result;
    }
    result.session.session = *sessions_.find(id);
    result.session.stream_ticket = sessions_.issue_ticket(id);
    result.session.ticket_ttl_seconds = config_.stream_ticket_ttl_seconds;
    return result;
}

[[nodiscard]] bool GatewayCore::close_session(SessionId id) {
    const std::optional<SessionInfo> info = sessions_.find(id);
    if (!info.has_value()) {
        return false;
    }
    ownership_.release_all(id);
    sessions_.close(id);
    for (auto connection = connections_.begin(); connection != connections_.end();) {
        if (connection->second.session == id) {
            static_cast<void>(engine_.remove_connection(connection->first));
            connection = connections_.erase(connection);
        } else {
            ++connection;
        }
    }
    return true;
}

[[nodiscard]] GatewayError GatewayCore::update_subscription(
    SessionId id, const projection::Subscription& subscription) {
    if (!sessions_.find(id).has_value()) {
        return GatewayError::invalid_request;
    }
    if (subscription.area.has_value()) {
        const spatial::Aabb& area = *subscription.area;
        const double east = std::abs(area.maximum.east - area.minimum.east);
        const double north = std::abs(area.maximum.north - area.minimum.north);
        const double up = std::abs(area.maximum.up - area.minimum.up);
        if (east > config_.max_aoi_extent_meters
            || north > config_.max_aoi_extent_meters
            || up > config_.max_aoi_extent_meters) {
            return GatewayError::limit_exceeded;
        }
    }
    for (auto& [connection, state] : connections_) {
        if (state.session == id) {
            static_cast<void>(engine_.update_subscription(connection, subscription));
        }
    }
    pending_subscriptions_[id] = subscription;
    return GatewayError::none;
}

[[nodiscard]] GatewayError GatewayCore::acquire_ownership(
    SessionId id, foundation::WorldId target, const std::vector<std::string>& keys,
    std::uint64_t lease_until_tick, std::uint64_t current_tick) {
    const std::optional<SessionInfo> info = sessions_.find(id);
    if (!info.has_value()) {
        return GatewayError::invalid_request;
    }
    if (!target.valid() || keys.empty() || lease_until_tick <= current_tick) {
        return GatewayError::invalid_request;
    }
    for (const std::string& key : keys) {
        if (!authorization_->can_write_property(info->principal, target, key)) {
            return GatewayError::permission_denied;
        }
        if (!ownership_.acquire(id, target, key, lease_until_tick, current_tick)) {
            return GatewayError::permission_denied;
        }
    }
    return GatewayError::none;
}

[[nodiscard]] GatewayError GatewayCore::release_ownership(
    SessionId id, foundation::WorldId target, const std::vector<std::string>& keys) {
    if (!sessions_.find(id).has_value()) {
        return GatewayError::invalid_request;
    }
    for (const std::string& key : keys) {
        static_cast<void>(ownership_.release(id, target, key));
    }
    return GatewayError::none;
}

[[nodiscard]] std::pair<GatewayError, CommandReceipt> GatewayCore::submit_command(
    SessionId id, const ExternalCommand& command, std::uint64_t current_tick) {
    const auto rejected = [&command](GatewayError error) {
        return std::make_pair(error, CommandReceipt{ReceiptStatus::rejected, error,
                                                    command.client_sequence, 0});
    };

    const std::optional<SessionInfo> info = sessions_.find(id);
    if (!info.has_value() || id != command.session) {
        return rejected(GatewayError::invalid_request);
    }

    // 命令去重：同一 (session, client_sequence) 重试返回缓存结果，不重复入队。
    const std::optional<CommandReceipt> cached =
        sessions_.find_receipt(id, command.client_sequence);
    if (cached.has_value()) {
        CommandReceipt duplicate = *cached;
        duplicate.status = ReceiptStatus::duplicate;
        return std::make_pair(GatewayError::none, duplicate);
    }

    if (!sessions_.consume_command_budget(id)) {
        return rejected(GatewayError::rate_limited);
    }

    // 目标 tick 窗口：[current + 1, current + max_lead]；hint 为 0 时由 Gateway 安排。
    std::uint64_t target_tick = current_tick + config_.command_lead_ticks;
    if (command.target_tick_hint != 0) {
        target_tick = command.target_tick_hint;
    }
    if (target_tick < current_tick + 1
        || target_tick > current_tick + config_.max_command_lead_ticks) {
        return rejected(GatewayError::tick_out_of_window);
    }

    simulation::CommandPayload payload;
    if (const auto* set_property = std::get_if<SetPropertyParams>(&command.params)) {
        if (!command.target_wid.valid()) {
            return rejected(GatewayError::invalid_request);
        }
        if (!authorization_->can_write_property(info->principal, command.target_wid,
                                                set_property->key)) {
            return rejected(GatewayError::permission_denied);
        }
        if (!ownership_.holds(id, command.target_wid, set_property->key, current_tick)) {
            return rejected(GatewayError::permission_denied);
        }
        payload = simulation::SetPropertyCommand{
            command.target_wid, set_property->key, set_property->value};
    } else if (const auto* create = std::get_if<CreateObjectParams>(&command.params)) {
        if (!authorization_->can_manage_objects(info->principal)) {
            return rejected(GatewayError::permission_denied);
        }
        if (!create->requested_id.valid()
            || create->properties.size() > config_.max_command_parameters) {
            return rejected(GatewayError::invalid_request);
        }
        world::WorldObject object;
        object.id = create->requested_id;
        object.semantic_type = create->semantic_type;
        object.geometry_ref = create->geometry_ref;
        object.position = create->position;
        object.properties = create->properties;
        payload = simulation::CreateObjectCommand{std::move(object)};
    } else {
        if (!authorization_->can_manage_objects(info->principal)) {
            return rejected(GatewayError::permission_denied);
        }
        if (!command.target_wid.valid()) {
            return rejected(GatewayError::invalid_request);
        }
        payload = simulation::DestroyObjectCommand{command.target_wid};
    }

    if (!submitter_) {
        return rejected(GatewayError::invalid_request);
    }
    const std::uint64_t ingress = ++ingress_sequence_;
    simulation::CommandMeta meta;
    meta.ingress_sequence = ingress;
    meta.expected_object_version = command.expected_object_version;
    static_cast<void>(submitter_(target_tick, std::move(payload), meta));

    pending_commands_.emplace(ingress, PendingCommand{id, command.client_sequence});
    CommandReceipt receipt{ReceiptStatus::accepted, GatewayError::none,
                           command.client_sequence, ingress};
    sessions_.store_receipt(id, receipt);
    return std::make_pair(GatewayError::none, receipt);
}

[[nodiscard]] GatewayError GatewayCore::request_keyframe(SessionId id) {
    // 幂等操作：对会话的全部连接安排 keyframe。
    if (!sessions_.find(id).has_value()) {
        return GatewayError::invalid_request;
    }
    for (auto& [connection, state] : connections_) {
        if (state.session == id) {
            static_cast<void>(engine_.schedule_keyframe(connection));
        }
    }
    return GatewayError::none;
}

[[nodiscard]] std::optional<std::string> GatewayCore::issue_stream_ticket(SessionId id) {
    if (!sessions_.find(id).has_value()) {
        return std::nullopt;
    }
    return sessions_.issue_ticket(id);
}

[[nodiscard]] std::optional<projection::ConnectionId> GatewayCore::attach_stream(
    projection::ConnectionId connection, std::string_view ticket) {
    const std::optional<SessionInfo> info = sessions_.redeem_ticket(ticket);
    if (!info.has_value() || !connection.valid()) {
        return std::nullopt;
    }
    projection::Subscription subscription;
    const auto pending = pending_subscriptions_.find(info->id);
    if (pending != pending_subscriptions_.end()) {
        subscription = pending->second;
    }
    if (!engine_.add_connection(connection, subscription)) {
        return std::nullopt;
    }
    auto [state, inserted] = connections_.emplace(connection, ConnectionState{
        info->id, config_.max_state_queue_bytes, config_.max_reliable_queue_bytes});
    state->second.last_ack_tick = last_pump_tick_;
    return connection;
}

void GatewayCore::detach_stream(projection::ConnectionId connection) {
    static_cast<void>(engine_.remove_connection(connection));
    connections_.erase(connection);
}

[[nodiscard]] bool GatewayCore::inbound_ack(projection::ConnectionId connection,
                                            std::uint64_t stream_epoch,
                                            std::uint64_t snapshot_id) {
    ConnectionState* state = find_connection(connection);
    if (state == nullptr) {
        return false;
    }
    const projection::ConnectionProjection* projection = engine_.connection(connection);
    if (projection == nullptr || projection->stream_epoch() != stream_epoch) {
        mark_disconnect(*state, GatewayError::epoch_mismatch);
        return false;
    }
    const projection::AckResult result = engine_.acknowledge(connection, snapshot_id);
    if (result == projection::AckResult::error_epoch_mismatch) {
        mark_disconnect(*state, GatewayError::epoch_mismatch);
        return false;
    }
    if (result == projection::AckResult::error_unknown_snapshot) {
        mark_disconnect(*state, GatewayError::ack_unknown);
        return false;
    }
    if (result == projection::AckResult::accepted) {
        state->last_ack_tick = last_pump_tick_;
    }
    return true;
}

[[nodiscard]] bool GatewayCore::inbound_keyframe_request(
    projection::ConnectionId connection) {
    ConnectionState* state = find_connection(connection);
    if (state == nullptr) {
        return false;
    }
    // 幂等：直接安排下一状态帧为 keyframe。
    return engine_.schedule_keyframe(connection);
}

void GatewayCore::set_thread_pool(std::shared_ptr<foundation::ThreadPool> pool) {
    thread_pool_ = std::move(pool);
}

void GatewayCore::pump(std::uint64_t tick) {
    last_pump_tick_ = tick;
    const std::int64_t dt = engine_.config().tick_dt_microseconds;
    const std::uint64_t ack_timeout_ticks =
        (static_cast<std::uint64_t>(config_.ack_timeout_seconds) * 1'000'000ULL)
        / static_cast<std::uint64_t>(dt);
    const std::uint64_t heartbeat_ticks =
        (static_cast<std::uint64_t>(config_.heartbeat_interval_seconds) * 1'000'000ULL)
        / static_cast<std::uint64_t>(dt);

    // 第一段保持单线程：resync 检查与 next_frame 会读写 engine 与连接状态。
    pump_order_.clear();
    pump_frames_.clear();
    pump_order_.reserve(connections_.size());
    pump_frames_.reserve(connections_.size());
    for (auto& [connection, state] : connections_) {
        if (state.must_disconnect) {
            continue;
        }

        // state queue 合并丢弃后不能发送引用已丢弃基线的 delta，安排 keyframe。
        if (state.state_queue.resync_required()) {
            static_cast<void>(engine_.schedule_keyframe(connection));
            state.state_queue.clear_resync();
        }

        pump_order_.push_back(connection);
        pump_frames_.push_back(engine_.next_frame(connection));
    }

    // 第二段按连接并行编码：编码结果只写各自槽位，与串行顺序执行逐字节一致。
    // 帧内实体深拷贝在编码完成后由工作线程就地销毁（析构无观察效应），
    // 第三段只读取本段提取的帧种类与基线快照号。
    const std::size_t count = pump_order_.size();
    pump_encoded_.clear();
    pump_encoded_.resize(count);
    pump_frame_kind_.clear();
    pump_frame_kind_.resize(count, 0);
    pump_frame_baseline_.clear();
    pump_frame_baseline_.resize(count, 0);
    const auto encode_range = [this](std::size_t index) {
        std::optional<projection::StateFrame>& frame = pump_frames_[index];
        if (!frame.has_value()) {
            return;
        }
        const bool is_keyframe = std::holds_alternative<projection::Keyframe>(*frame);
        pump_frame_kind_[index] = is_keyframe ? 1 : 2;
        pump_frame_baseline_[index] = is_keyframe
            ? 0 : std::get<projection::Delta>(*frame).baseline_snapshot_id;
        if (frame_encoder_) {
            pump_encoded_[index] = frame_encoder_(*frame);
        }
        frame.reset();
    };
    if (thread_pool_ != nullptr && count > 1) {
        thread_pool_->run(count, encode_range);
    } else {
        for (std::size_t index = 0; index < count; ++index) {
            encode_range(index);
        }
    }

    // 第三段恢复单线程：入队、心跳与超时检查按原语义顺序执行。
    last_pump_keyframe_count_ = 0;
    for (std::size_t index = 0; index < count; ++index) {
        const projection::ConnectionId connection = pump_order_[index];
        ConnectionState* state = find_connection(connection);
        if (state == nullptr || state->must_disconnect) {
            continue;
        }
        const char frame_kind = pump_frame_kind_[index];
        if (frame_kind != 0 && frame_encoder_) {
            QueuedFrame queued;
            queued.snapshot_id = engine_.latest_snapshot_id();
            queued.is_keyframe = frame_kind == 1;
            if (queued.is_keyframe) {
                ++last_pump_keyframe_count_;
            }
            queued.baseline_snapshot_id = pump_frame_baseline_[index];
            queued.bytes = std::move(pump_encoded_[index]);
            state->state_queue.push(std::move(queued));
        } else if (frame_kind == 0 && heartbeat_encoder_
                   && tick - state->last_heartbeat_tick >= heartbeat_ticks) {
            state->last_heartbeat_tick = tick;
            static_cast<void>(state->reliable_queue.push(
                heartbeat_encoder_(tick, engine_.latest_snapshot_id())));
        }

        if (state->last_ack_tick != 0 && tick - state->last_ack_tick > ack_timeout_ticks) {
            mark_disconnect(*state, GatewayError::slow_client);
        }
    }
}

void GatewayCore::on_commands_applied(const simulation::ApplyReport& report) {
    for (const simulation::CommandOutcome& outcome : report.outcomes) {
        const auto pending = pending_commands_.find(outcome.ingress_sequence);
        if (pending == pending_commands_.end()) {
            continue;
        }
        const PendingCommand command = pending->second;
        pending_commands_.erase(pending);

        CommandReceipt receipt;
        receipt.client_sequence = command.client_sequence;
        receipt.ingress_sequence = outcome.ingress_sequence;
        if (outcome.applied) {
            receipt.status = ReceiptStatus::applied;
        } else {
            receipt.status = ReceiptStatus::rejected;
            switch (outcome.reason) {
            case simulation::CommandRejectReason::version_conflict:
                receipt.error = GatewayError::version_conflict;
                break;
            case simulation::CommandRejectReason::missing_object:
                receipt.error = GatewayError::missing_object;
                break;
            default:
                receipt.error = GatewayError::invalid_request;
                break;
            }
        }
        sessions_.store_receipt(command.session, receipt);

        if (!receipt_encoder_) {
            continue;
        }
        for (auto& [connection, state] : connections_) {
            if (state.session == command.session) {
                if (!state.reliable_queue.push(receipt_encoder_(receipt))) {
                    mark_disconnect(state, GatewayError::slow_client);
                }
            }
        }
    }
}

[[nodiscard]] std::optional<FrameBytes> GatewayCore::next_outbound(
    projection::ConnectionId connection) {
    ConnectionState* state = find_connection(connection);
    if (state == nullptr) {
        return std::nullopt;
    }
    if (std::optional<FrameBytes> reliable = state->reliable_queue.pop()) {
        return reliable;
    }
    if (std::optional<QueuedFrame> frame = state->state_queue.pop()) {
        return frame->bytes;
    }
    return std::nullopt;
}

[[nodiscard]] bool GatewayCore::must_disconnect(
    projection::ConnectionId connection) const {
    const auto found = connections_.find(connection);
    return found != connections_.end() && found->second.must_disconnect;
}

[[nodiscard]] GatewayError GatewayCore::disconnect_reason(
    projection::ConnectionId connection) const {
    const auto found = connections_.find(connection);
    if (found == connections_.end()) {
        return GatewayError::none;
    }
    return found->second.disconnect_reason;
}

[[nodiscard]] std::size_t GatewayCore::connection_count() const noexcept {
    return connections_.size();
}

[[nodiscard]] const SessionManager& GatewayCore::sessions() const noexcept {
    return sessions_;
}

[[nodiscard]] GatewayCore::ConnectionState* GatewayCore::find_connection(
    projection::ConnectionId connection) noexcept {
    const auto found = connections_.find(connection);
    if (found == connections_.end()) {
        return nullptr;
    }
    return &found->second;
}

void GatewayCore::mark_disconnect(ConnectionState& state, GatewayError reason) {
    state.must_disconnect = true;
    state.disconnect_reason = reason;
}

} // namespace geoworld::gateway
