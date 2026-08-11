#include "geoworld/ecs/schema.hpp"

namespace geoworld::ecs {

const std::vector<schema::SchemaDescriptor>& component_schemas() {
    static const std::vector<schema::SchemaDescriptor> schemas{
        {"ecs.world_identity", 1, schema::SchemaKind::component,
         {{"value", schema::FieldType::int64}}},
        {"ecs.position_ecef", 1, schema::SchemaKind::component,
         {{"x", schema::FieldType::float64},
          {"y", schema::FieldType::float64},
          {"z", schema::FieldType::float64}}},
        {"ecs.semantic_type", 1, schema::SchemaKind::component,
         {{"value", schema::FieldType::string}}}
    };
    return schemas;
}

void register_ecs_schemas(schema::SchemaRegistry& registry) {
    for (const auto& descriptor : component_schemas()) {
        static_cast<void>(registry.register_schema(descriptor));
    }
}

} // namespace geoworld::ecs
