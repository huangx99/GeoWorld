#include "geoworld/gateway/ownership.hpp"

namespace geoworld::gateway {

[[nodiscard]] std::size_t OwnershipRegistry::OwnershipKeyHash::operator()(
    const OwnershipKey& key) const noexcept {
    const std::size_t wid = static_cast<std::size_t>(
        key.target.value ^ (key.target.value >> 32U));
    return wid ^ (std::hash<std::string>{}(key.key) * 1099511628211ULL);
}

[[nodiscard]] bool OwnershipRegistry::acquire(SessionId session,
                                              foundation::WorldId target,
                                              const std::string& key,
                                              std::uint64_t lease_until_tick,
                                              std::uint64_t current_tick) {
    const OwnershipKey ownership_key{target, key};
    const auto found = grants_.find(ownership_key);
    if (found != grants_.end()) {
        const Grant& grant = found->second;
        // 租约未到期且属主不同，冲突拒绝；到期租约自动失效。
        if (grant.owner != session && grant.lease_until_tick > current_tick) {
            return false;
        }
    }
    grants_[ownership_key] = Grant{session, lease_until_tick};
    return true;
}

[[nodiscard]] bool OwnershipRegistry::release(SessionId session,
                                              foundation::WorldId target,
                                              const std::string& key) {
    const auto found = grants_.find(OwnershipKey{target, key});
    // 幂等释放：不存在的授权视为成功。
    if (found == grants_.end()) {
        return true;
    }
    if (found->second.owner != session) {
        return false;
    }
    grants_.erase(found);
    return true;
}

[[nodiscard]] bool OwnershipRegistry::holds(SessionId session,
                                            foundation::WorldId target,
                                            const std::string& key,
                                            std::uint64_t current_tick) const {
    const auto found = grants_.find(OwnershipKey{target, key});
    if (found == grants_.end()) {
        return false;
    }
    return found->second.owner == session && found->second.lease_until_tick > current_tick;
}

void OwnershipRegistry::release_all(SessionId session) {
    for (auto grant = grants_.begin(); grant != grants_.end();) {
        if (grant->second.owner == session) {
            grant = grants_.erase(grant);
        } else {
            ++grant;
        }
    }
}

[[nodiscard]] std::size_t OwnershipRegistry::size() const noexcept {
    return grants_.size();
}

} // namespace geoworld::gateway
