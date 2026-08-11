#include "geoworld/schema/schema_registry.hpp"

#include <utility>

namespace geoworld::schema {

RegistrationResult SchemaRegistry::register_schema(SchemaDescriptor descriptor) {
    if (descriptor.name.empty() || descriptor.version == 0) {
        return RegistrationResult::invalid_descriptor;
    }
    const auto existing = schemas_.find(descriptor.name);
    if (existing != schemas_.end()) {
        const auto& current = existing->second;
        if (current.version == descriptor.version && current.kind == descriptor.kind
            && current.fields == descriptor.fields) {
            return RegistrationResult::already_registered;
        }
        return RegistrationResult::incompatible_version;
    }
    schemas_.emplace(descriptor.name, std::move(descriptor));
    return RegistrationResult::registered;
}

std::optional<SchemaDescriptor> SchemaRegistry::find(std::string_view name) const {
    const auto iterator = schemas_.find(std::string{name});
    if (iterator == schemas_.end()) {
        return std::nullopt;
    }
    return iterator->second;
}

std::size_t SchemaRegistry::size() const noexcept { return schemas_.size(); }

} // namespace geoworld::schema
