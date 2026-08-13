#pragma once

#include "geoworld/gateway/types.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace geoworld::gateway {

// 可替换鉴权接口：测试可用固定 fixture，生产由部署方实现。M4 不自建用户数据库。
class AuthenticationProvider {
public:
    virtual ~AuthenticationProvider() = default;

    [[nodiscard]] virtual std::optional<Principal> authenticate(
        std::string_view credential_token) const = 0;
};

class AuthorizationPolicy {
public:
    virtual ~AuthorizationPolicy() = default;

    [[nodiscard]] virtual bool can_observe(const Principal& principal) const = 0;
    [[nodiscard]] virtual bool can_write_property(const Principal& principal,
                                                  foundation::WorldId target,
                                                  std::string_view key) const = 0;
    [[nodiscard]] virtual bool can_manage_objects(const Principal& principal) const = 0;
};

// 测试 fixture：token 直接映射主体；管理员可管理对象，写权限按属性白名单。
class FixtureAuthentication final : public AuthenticationProvider {
public:
    void add_credential(std::string token, Principal principal);

    [[nodiscard]] std::optional<Principal> authenticate(
        std::string_view credential_token) const override;

private:
    std::unordered_map<std::string, Principal> credentials_;
};

class FixtureAuthorization final : public AuthorizationPolicy {
public:
    void allow_writable_property(std::string key);

    [[nodiscard]] bool can_observe(const Principal& principal) const override;
    [[nodiscard]] bool can_write_property(const Principal& principal,
                                          foundation::WorldId target,
                                          std::string_view key) const override;
    [[nodiscard]] bool can_manage_objects(const Principal& principal) const override;

private:
    std::unordered_map<std::string, bool> writable_keys_;
};

} // namespace geoworld::gateway
