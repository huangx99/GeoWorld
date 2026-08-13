#include "geoworld/gateway/errors.hpp"

namespace geoworld::gateway {

[[nodiscard]] const char* error_code(GatewayError error) noexcept {
    switch (error) {
    case GatewayError::none: return "";
    case GatewayError::protocol_incompatible: return "GWG001";
    case GatewayError::auth_failed: return "GWG002";
    case GatewayError::permission_denied: return "GWG003";
    case GatewayError::rate_limited: return "GWG004";
    case GatewayError::invalid_request: return "GWG005";
    case GatewayError::limit_exceeded: return "GWG006";
    case GatewayError::ticket_invalid: return "GWG101";
    case GatewayError::epoch_mismatch: return "GWG102";
    case GatewayError::ack_unknown: return "GWG103";
    case GatewayError::baseline_unavailable: return "GWG104";
    case GatewayError::slow_client: return "GWG105";
    case GatewayError::version_conflict: return "GWG201";
    case GatewayError::missing_object: return "GWG202";
    case GatewayError::tick_out_of_window: return "GWG203";
    case GatewayError::unsupported_operation: return "GWG204";
    }
    return "GWG005";
}

} // namespace geoworld::gateway
