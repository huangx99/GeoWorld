#include "geoworld/gateway/auth.hpp"

namespace geoworld::gateway {

void FixtureAuthentication::add_credential(std::string token, Principal principal) {
    credentials_.emplace(std::move(token), std::move(principal));
}

[[nodiscard]] std::optional<Principal> FixtureAuthentication::authenticate(
    std::string_view credential_token) const {
    const auto found = credentials_.find(std::string{credential_token});
    if (found == credentials_.end()) {
        return std::nullopt;
    }
    return found->second;
}

void FixtureAuthorization::allow_writable_property(std::string key) {
    writable_keys_.emplace(std::move(key), true);
}

[[nodiscard]] bool FixtureAuthorization::can_observe(const Principal& principal) const {
    return !principal.id.empty();
}

[[nodiscard]] bool FixtureAuthorization::can_write_property(
    const Principal& principal, foundation::WorldId target, std::string_view key) const {
    static_cast<void>(target);
    if (principal.administrator) {
        return true;
    }
    return writable_keys_.contains(std::string{key});
}

[[nodiscard]] bool FixtureAuthorization::can_manage_objects(
    const Principal& principal) const {
    return principal.administrator;
}

} // namespace geoworld::gateway
