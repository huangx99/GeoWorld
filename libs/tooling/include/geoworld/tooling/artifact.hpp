#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace geoworld::tooling {

enum class ArtifactKind { rule, behavior_tree, state_machine };

[[nodiscard]] std::string_view artifact_type(ArtifactKind kind) noexcept;

struct ArtifactHeader {
    std::string type;
    std::uint32_t schema_version{};
    std::string source_hash;
    std::uint32_t compiler_version{1};

    [[nodiscard]] bool valid() const noexcept {
        return !type.empty() && schema_version != 0 && !source_hash.empty();
    }
};

struct ArtifactValidation {
    bool valid{};
    std::string message;
};

[[nodiscard]] std::string source_hash(std::string_view source);
[[nodiscard]] ArtifactHeader make_header(ArtifactKind kind, std::uint32_t schema_version,
                                          std::string_view source);
[[nodiscard]] ArtifactValidation validate(const ArtifactHeader& header,
                                          ArtifactKind expected_kind,
                                          std::uint32_t minimum_schema_version = 1);

} // namespace geoworld::tooling
