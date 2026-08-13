#include "geoworld/gateway/config.hpp"

namespace geoworld::gateway {

namespace {

[[nodiscard]] bool invalid(std::string& diagnostic, std::string reason) {
    diagnostic = std::move(reason);
    return false;
}

} // namespace

[[nodiscard]] bool validate(const GatewayConfig& config, std::string& diagnostic) {
    if (config.max_state_queue_bytes == 0) {
        return invalid(diagnostic, "gateway: max_state_queue_bytes must be positive");
    }
    if (config.max_reliable_queue_bytes == 0) {
        return invalid(diagnostic, "gateway: max_reliable_queue_bytes must be positive");
    }
    if (config.ack_timeout_seconds == 0) {
        return invalid(diagnostic, "gateway: ack_timeout_seconds must be positive");
    }
    if (config.heartbeat_interval_seconds == 0) {
        return invalid(diagnostic, "gateway: heartbeat_interval_seconds must be positive");
    }
    if (config.stream_ticket_ttl_seconds == 0) {
        return invalid(diagnostic, "gateway: stream_ticket_ttl_seconds must be positive");
    }
    if (config.command_rate_per_session == 0) {
        return invalid(diagnostic, "gateway: command_rate_per_session must be positive");
    }
    if (config.command_lead_ticks == 0) {
        return invalid(diagnostic, "gateway: command_lead_ticks must be positive");
    }
    if (config.max_command_lead_ticks < config.command_lead_ticks) {
        return invalid(diagnostic,
            "gateway: max_command_lead_ticks must be at least command_lead_ticks");
    }
    if (config.gateway_io_threads == 0) {
        return invalid(diagnostic, "gateway: gateway_io_threads must be positive");
    }
    if (config.encoding_workers == 0) {
        return invalid(diagnostic, "gateway: encoding_workers must be positive");
    }
    if (config.max_sessions == 0) {
        return invalid(diagnostic, "gateway: max_sessions must be positive");
    }
    if (config.max_aoi_extent_meters <= 0.0) {
        return invalid(diagnostic, "gateway: max_aoi_extent_meters must be positive");
    }
    return true;
}

} // namespace geoworld::gateway
