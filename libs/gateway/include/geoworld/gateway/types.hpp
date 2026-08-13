#pragma once

#include "geoworld/gateway/errors.hpp"
#include "geoworld/projection/connection.hpp"
#include "geoworld/world/world.hpp"

#include <cstdint>
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

struct ExternalCommand {
    SessionId session{};
    std::uint64_t client_sequence{};
    foundation::WorldId target_wid;
    CommandParams params;
    std::uint64_t expected_object_version{};
    std::uint64_t target_tick_hint{};
};

enum class ReceiptStatus { accepted, applied, rejected, duplicate };

// 命令终态回执。同一 (session, client_sequence) 重试返回缓存结果。
struct CommandReceipt {
    ReceiptStatus status{ReceiptStatus::accepted};
    GatewayError error{GatewayError::none};
    std::uint64_t client_sequence{};
    std::uint64_t ingress_sequence{};
};

} // namespace geoworld::gateway
