#pragma once

#include <cstdint>

namespace geoworld::gateway {

// 网关错误分类，映射到 docs/M4.md 冻结的稳定错误码字符串。
enum class GatewayError {
    none,
    protocol_incompatible,
    auth_failed,
    permission_denied,
    rate_limited,
    invalid_request,
    limit_exceeded,
    ticket_invalid,
    epoch_mismatch,
    ack_unknown,
    baseline_unavailable,
    slow_client,
    version_conflict,
    missing_object,
    tick_out_of_window,
    unsupported_operation,
};

[[nodiscard]] const char* error_code(GatewayError error) noexcept;

} // namespace geoworld::gateway
