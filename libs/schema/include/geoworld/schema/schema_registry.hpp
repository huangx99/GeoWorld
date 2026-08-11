#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace geoworld::schema {

enum class SchemaKind { component, object_type, relation };
enum class FieldType { int64, float64, boolean, string };

struct FieldDescriptor {
    std::string name;
    FieldType type{FieldType::string};

    bool operator==(const FieldDescriptor&) const = default;
};

struct SchemaDescriptor {
    std::string name;
    std::uint32_t version{};
    SchemaKind kind{SchemaKind::component};
    std::vector<FieldDescriptor> fields;
};

enum class RegistrationResult {
    registered,
    invalid_descriptor,
    already_registered,
    incompatible_version
};

class SchemaRegistry {
public:
    [[nodiscard]] RegistrationResult register_schema(SchemaDescriptor descriptor);
    [[nodiscard]] std::optional<SchemaDescriptor> find(std::string_view name) const;
    [[nodiscard]] std::size_t size() const noexcept;

private:
    std::unordered_map<std::string, SchemaDescriptor> schemas_;
};

} // namespace geoworld::schema
