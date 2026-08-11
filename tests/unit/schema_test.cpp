#include "geoworld/schema/schema_registry.hpp"
#include "geoworld/schema/migration.hpp"

int main() {
    geoworld::schema::SchemaRegistry registry;
    const geoworld::schema::SchemaDescriptor descriptor{
        "core.transform", 1, geoworld::schema::SchemaKind::component
    };

    using geoworld::schema::RegistrationResult;
    if (registry.register_schema(descriptor) != RegistrationResult::registered
        || registry.register_schema(descriptor) != RegistrationResult::already_registered) {
        return 1;
    }
    if (registry.register_schema({"core.transform", 2,
                                  geoworld::schema::SchemaKind::component})
        != RegistrationResult::incompatible_version) {
        return 1;
    }
    const auto found = registry.find("core.transform");
    if (!found.has_value() || found->version != 1 || registry.size() != 1) {
        return 1;
    }

    geoworld::schema::MigrationRegistry migrations;
    if (!migrations.register_upcast("core.transform", 1, [](auto& values) {
            values["height_m"] = values.at("height");
            values.erase("height");
            return true;
        })) {
        return 1;
    }
    geoworld::schema::PropertyBag values{{"height", 12.0}};
    if (!migrations.upcast("core.transform", 1, 2, values)) {
        return 1;
    }
    return values.contains("height_m") && !values.contains("height") ? 0 : 1;
}
