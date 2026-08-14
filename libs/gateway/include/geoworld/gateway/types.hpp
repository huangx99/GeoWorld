#pragma once

#include "geoworld/gateway/errors.hpp"
#include "geoworld/projection/connection.hpp"
#include "geoworld/world/world.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>

namespace geoworld::gateway {

// 控制面会话 ID，不作为世界对象 ID。
struct SessionId {
    std::uint64_t value{};

    constexpr auto operator<=>(const SessionId&) const = default;
    [[nodiscard]] constexpr bool valid() const noexcept { return value != 0; }
};

struct SessionIdHash {
    std::size_t operator()(SessionId id) const noexcept {
        return static_cast<std::size_t>(id.value ^ (id.value >> 32U));
    }
};

// 认证后的主体。权限语义由 AuthorizationPolicy 实现决定。
struct Principal {
    std::string id;
    bool administrator{};
};

// 外部命令的传输无关内部 DTO。客户端提交意图而不是状态。
struct SetPropertyParams {
    std::string key;
    world::PropertyValue value;
};

struct CreateObjectParams {
    foundation::WorldId requested_id;
    std::string semantic_type;
    std::string geometry_ref;
    world::PositionEcef position;
    world::PropertyBag properties;
};

struct DestroyObjectParams {
};

using CommandParams = std::variant<SetPropertyParams, CreateObjectParams, DestroyObjectParams>;

// M5 durable admission 幂等键：128 位，线格式固定 16 原始字节（与 persistence BranchId 同样式）。
inline constexpr std::size_t durable_request_id_bytes = 16;
using DurableRequestId = std::array<std::uint8_t, durable_request_id_bytes>;

struct ExternalCommand {
    SessionId session{};
    std::uint64_t client_sequence{};
    foundation::WorldId target_wid;
    CommandParams params;
    std::uint64_t expected_object_version{};
    std::uint64_t target_tick_hint{};
    // 缺省走 M4 进程内 (session, client_sequence) 去重；携带时走 durable admission。
    std::optional<DurableRequestId> request_id;
};

enum class ReceiptStatus { accepted, applied, rejected, duplicate, durable_accepted };

// 命令终态回执。同一 (session, client_sequence) 重试返回缓存结果。
struct CommandReceipt {
    ReceiptStatus status{ReceiptStatus::accepted};
    GatewayError error{GatewayError::none};
    std::uint64_t client_sequence{};
    std::uint64_t ingress_sequence{};
    // durable_accepted 时携带 WAL LSN；其余状态为 0。
    std::uint64_t durable_lsn{};
};

} // namespace geoworld::gateway
