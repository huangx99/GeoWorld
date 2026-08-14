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
    // M5 追加：同一幂等键携带不同命令内容。
    idempotency_conflict,
    // M5 追加：durable 接纳不可用（未配置 WAL、队列满或故障态）。
    durability_unavailable,
};

[[nodiscard]] const char* error_code(GatewayError error) noexcept;

} // namespace geoworld::gateway
