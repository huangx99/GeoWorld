#include "stream_client.hpp"

#include "geoworld/protocol/replica.hpp"
#include "geoworld/protocol/wire.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {

using geoworld::client::StreamClient;
using geoworld::client::StreamClientConfig;
using geoworld::client::SubmitResult;
using geoworld::foundation::WorldId;

// loopback 端到端闭环的默认参数；命令行可覆盖，不写死散落的字面量。
struct CliConfig {
    StreamClientConfig client;
    double subscribe_extent_meters{1000.0};
    std::uint64_t target_wid{7};
    std::string property_key{"speed"};
    double property_value{9.5};
    std::uint64_t expected_object_version{1};
    std::uint64_t ownership_lease_ticks{1000};
    std::uint64_t max_frames{500};
};

constexpr std::string_view kUsage =
    "用法: gw-stream-client [--control host:port] [--stream-host host]"
    " [--stream-port port] [--token token] [--extent meters]"
    " [--target-wid id] [--property-key key] [--property-value value]"
    " [--expected-version version] [--tls] [--root-cert file]";

[[nodiscard]] bool parse_cli(int argc, char** argv, CliConfig& config) {
    for (int index = 1; index < argc; ++index) {
        const std::string_view flag{argv[index]};
        const auto take_value = [&]() -> const char* {
            return index + 1 < argc ? argv[++index] : nullptr;
        };
        if (flag == "--control") {
            const char* value = take_value();
            if (value == nullptr) { return false; }
            config.client.control_address = value;
        } else if (flag == "--stream-host") {
            const char* value = take_value();
            if (value == nullptr) { return false; }
            config.client.stream_host = value;
        } else if (flag == "--stream-port") {
            const char* value = take_value();
            if (value == nullptr) { return false; }
            config.client.stream_port = static_cast<std::uint16_t>(std::stoul(value));
        } else if (flag == "--token") {
            const char* value = take_value();
            if (value == nullptr) { return false; }
            config.client.credential_token = value;
        } else if (flag == "--extent") {
            const char* value = take_value();
            if (value == nullptr) { return false; }
            config.subscribe_extent_meters = std::stod(value);
        } else if (flag == "--target-wid") {
            const char* value = take_value();
            if (value == nullptr) { return false; }
            config.target_wid = std::stoull(value);
        } else if (flag == "--property-key") {
            const char* value = take_value();
            if (value == nullptr) { return false; }
            config.property_key = value;
        } else if (flag == "--property-value") {
            const char* value = take_value();
            if (value == nullptr) { return false; }
            config.property_value = std::stod(value);
        } else if (flag == "--expected-version") {
            const char* value = take_value();
            if (value == nullptr) { return false; }
            config.expected_object_version = std::stoull(value);
        } else if (flag == "--tls") {
            config.client.use_tls = true;
        } else if (flag == "--root-cert") {
            const char* value = take_value();
            if (value == nullptr) { return false; }
            config.client.tls_root_certificate_file = value;
        } else {
            return false;
        }
    }
    return true;
}

constexpr int kExitUsage = 2;
constexpr int kExitOpenSession = 3;
constexpr int kExitSubscribe = 4;
constexpr int kExitOwnership = 5;
constexpr int kExitConnect = 6;
constexpr int kExitStreamFailure = 7;
constexpr int kExitSubmit = 8;
constexpr int kExitReceipt = 9;
constexpr int kExitLoopIncomplete = 10;

} // namespace

int main(int argc, char** argv) {
    CliConfig config;
    if (!parse_cli(argc, argv, config)) {
        std::cerr << kUsage << '\n';
        return kExitUsage;
    }

    StreamClient client{config.client};
    std::string diagnostic;
    if (!client.open_session(diagnostic)) {
        std::cerr << "open_session: " << diagnostic << '\n';
        return kExitOpenSession;
    }
    std::cout << "session=" << client.session().session_id
              << " tick=" << client.session().current_tick
              << " endpoint=" << client.session().data_endpoint << '\n';

    geoworld::projection::Subscription subscription;
    subscription.area = geoworld::spatial::Aabb{
        geoworld::spatial::Enu{-config.subscribe_extent_meters,
                               -config.subscribe_extent_meters,
                               -config.subscribe_extent_meters},
        geoworld::spatial::Enu{config.subscribe_extent_meters,
                               config.subscribe_extent_meters,
                               config.subscribe_extent_meters},
    };
    if (!client.update_subscription(subscription, diagnostic)) {
        std::cerr << "update_subscription: " << diagnostic << '\n';
        return kExitSubscribe;
    }

    const WorldId target{config.target_wid};
    if (!client.acquire_ownership(
            target, {config.property_key},
            client.session().current_tick + config.ownership_lease_ticks, diagnostic)) {
        std::cerr << "acquire_ownership: " << diagnostic << '\n';
        return kExitOwnership;
    }

    if (!client.connect_stream(diagnostic)) {
        std::cerr << "connect_stream: " << diagnostic << '\n';
        return kExitConnect;
    }
    std::cout << "stream connected\n";

    geoworld::protocol::ReplicaAccumulator replica;
    bool got_keyframe = false;
    bool submitted = false;
    bool receipt_applied = false;
    bool saw_update_after_command = false;

    for (std::uint64_t frames = 0; frames < config.max_frames; ++frames) {
        const std::optional<geoworld::protocol::WireFrame> frame =
            client.read_frame(diagnostic);
        if (!frame.has_value()) {
            std::cerr << "read_frame: " << diagnostic << '\n';
            return kExitStreamFailure;
        }

        if (const auto* keyframe =
                std::get_if<geoworld::protocol::WireKeyframe>(&*frame)) {
            replica.apply(*frame);
            got_keyframe = true;
            std::cout << "keyframe snapshot=" << keyframe->snapshot_id
                      << " epoch=" << keyframe->stream_epoch
                      << " entities=" << keyframe->entities.size()
                      << " replica_hash=" << replica.hash() << '\n';
            if (!client.send_ack(keyframe->stream_epoch, keyframe->snapshot_id,
                                 diagnostic)) {
                std::cerr << "ack: " << diagnostic << '\n';
                return kExitStreamFailure;
            }
        } else if (const auto* delta =
                       std::get_if<geoworld::protocol::WireDelta>(&*frame)) {
            replica.apply(*frame);
            for (const auto& update : delta->updates) {
                if (update.wid == target) {
                    saw_update_after_command = true;
                }
            }
            std::cout << "delta snapshot=" << delta->snapshot_id
                      << " enters=" << delta->enters.size()
                      << " updates=" << delta->updates.size()
                      << " leaves=" << delta->leaves.size()
                      << " replica_hash=" << replica.hash() << '\n';
            if (!client.send_ack(delta->stream_epoch, delta->snapshot_id, diagnostic)) {
                std::cerr << "ack: " << diagnostic << '\n';
                return kExitStreamFailure;
            }
        } else if (const auto* reliable =
                       std::get_if<geoworld::protocol::WireReliable>(&*frame)) {
            if (reliable->kind == geoworld::protocol::ReliableKind::command_receipt) {
                std::cout << "receipt sequence=" << reliable->receipt.client_sequence
                          << " status="
                          << static_cast<unsigned>(reliable->receipt.status)
                          << " error=" << reliable->receipt.error_code << '\n';
                if (reliable->receipt.status
                    == geoworld::protocol::ReceiptStatus::applied) {
                    receipt_applied = true;
                } else if (reliable->receipt.status
                           == geoworld::protocol::ReceiptStatus::rejected) {
                    std::cerr << "命令被拒绝: " << reliable->receipt.error_code << '\n';
                    return kExitReceipt;
                }
            } else if (reliable->kind
                       == geoworld::protocol::ReliableKind::protocol_error) {
                std::cerr << "协议错误: " << reliable->error.code << '\n';
                return kExitStreamFailure;
            }
        }

        if (got_keyframe && !submitted && replica.size() > 0) {
            const std::optional<SubmitResult> result = client.submit_set_property(
                target, config.property_key, config.property_value,
                1, config.expected_object_version, diagnostic);
            if (!result.has_value()
                || (result->status != geoworld::gateway::ReceiptStatus::accepted
                    && result->status != geoworld::gateway::ReceiptStatus::duplicate)) {
                std::cerr << "submit_set_property: " << diagnostic << " "
                          << (result.has_value() ? result->error_code : "") << '\n';
                return kExitSubmit;
            }
            submitted = true;
            std::cout << "command submitted ingress=" << result->ingress_sequence << '\n';
        }

        if (got_keyframe && receipt_applied && saw_update_after_command) {
            std::cout << "closed loop complete replica_hash=" << replica.hash() << '\n';
            client.disconnect_stream();
            return 0;
        }
    }

    std::cerr << "闭环未完成: keyframe=" << got_keyframe
              << " receipt_applied=" << receipt_applied
              << " saw_update=" << saw_update_after_command << '\n';
    return kExitLoopIncomplete;
}
