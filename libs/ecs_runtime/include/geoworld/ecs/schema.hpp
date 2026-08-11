#pragma once

#include "geoworld/schema/schema_registry.hpp"

#include <vector>

namespace geoworld::ecs {

void register_ecs_schemas(schema::SchemaRegistry& registry);
[[nodiscard]] const std::vector<schema::SchemaDescriptor>& component_schemas();

} // namespace geoworld::ecs
