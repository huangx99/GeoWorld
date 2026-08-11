#include "geoworld/schema/migration.hpp"

#include <utility>

namespace geoworld::schema {

std::size_t MigrationRegistry::KeyHash::operator()(const Key& key) const noexcept {
    return std::hash<std::string>{}(key.schema_name)
        ^ (static_cast<std::size_t>(key.from_version) << 1U);
}

bool MigrationRegistry::register_upcast(std::string schema_name,
                                        std::uint32_t from_version,
                                        Upcast upcast) {
    if (schema_name.empty() || from_version == 0 || !upcast) {
        return false;
    }
    return upcasts_.emplace(Key{std::move(schema_name), from_version},
                            std::move(upcast)).second;
}

bool MigrationRegistry::upcast(std::string_view schema_name,
                               std::uint32_t from_version,
                               std::uint32_t target_version,
                               PropertyBag& values) const {
    if (schema_name.empty() || from_version == 0 || from_version > target_version) {
        return false;
    }

    auto migrated = values;
    for (auto version = from_version; version < target_version; ++version) {
        const auto iterator = upcasts_.find(Key{std::string{schema_name}, version});
        if (iterator == upcasts_.end() || !iterator->second(migrated)) {
            return false;
        }
    }
    values = std::move(migrated);
    return true;
}

} // namespace geoworld::schema
