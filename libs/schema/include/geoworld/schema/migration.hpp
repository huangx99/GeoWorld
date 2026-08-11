#pragma once

#include "geoworld/schema/value.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace geoworld::schema {

class MigrationRegistry {
public:
    using Upcast = std::function<bool(PropertyBag&)>;

    [[nodiscard]] bool register_upcast(std::string schema_name,
                                       std::uint32_t from_version,
                                       Upcast upcast);
    [[nodiscard]] bool upcast(std::string_view schema_name,
                              std::uint32_t from_version,
                              std::uint32_t target_version,
                              PropertyBag& values) const;

private:
    struct Key {
        std::string schema_name;
        std::uint32_t from_version;

        bool operator==(const Key&) const = default;
    };

    struct KeyHash {
        std::size_t operator()(const Key& key) const noexcept;
    };

    std::unordered_map<Key, Upcast, KeyHash> upcasts_;
};

} // namespace geoworld::schema
