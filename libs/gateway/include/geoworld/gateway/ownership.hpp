#pragma once

#include "geoworld/gateway/types.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

namespace geoworld::gateway {

// 属性级所有权：任一 (WID, 属性键) 任意时刻至多一个会话拥有写权，租约以 tick 表达。
class OwnershipRegistry {
public:
    [[nodiscard]] bool acquire(SessionId session, foundation::WorldId target,
                               const std::string& key, std::uint64_t lease_until_tick,
                               std::uint64_t current_tick);
    [[nodiscard]] bool release(SessionId session, foundation::WorldId target,
                               const std::string& key);
    [[nodiscard]] bool holds(SessionId session, foundation::WorldId target,
                             const std::string& key, std::uint64_t current_tick) const;
    void release_all(SessionId session);

    [[nodiscard]] std::size_t size() const noexcept;

private:
    struct OwnershipKey {
        foundation::WorldId target;
        std::string key;

        bool operator==(const OwnershipKey&) const = default;
    };

    struct OwnershipKeyHash {
        std::size_t operator()(const OwnershipKey& key) const noexcept;
    };

    struct Grant {
        SessionId owner;
        std::uint64_t lease_until_tick{};
    };

    std::unordered_map<OwnershipKey, Grant, OwnershipKeyHash> grants_;
};

} // namespace geoworld::gateway
