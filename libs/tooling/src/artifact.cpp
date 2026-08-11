#include "geoworld/tooling/artifact.hpp"

#include <iomanip>
#include <sstream>

namespace geoworld::tooling {

std::string_view artifact_type(ArtifactKind kind) noexcept {
    switch (kind) {
    case ArtifactKind::rule:
        return "geoworld.rule";
    case ArtifactKind::behavior_tree:
        return "geoworld.behavior_tree";
    case ArtifactKind::state_machine:
        return "geoworld.state_machine";
    }
    return {};
}

std::string source_hash(std::string_view source) {
    constexpr std::uint64_t offset = 14695981039346656037ULL;
    constexpr std::uint64_t prime = 1099511628211ULL;
    auto hash = offset;
    for (const auto byte : source) {
        hash ^= static_cast<std::uint8_t>(byte);
        hash *= prime;
    }
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(16) << hash;
    return output.str();
}

ArtifactHeader make_header(ArtifactKind kind, std::uint32_t schema_version,
                           std::string_view source) {
    return {std::string{artifact_type(kind)}, schema_version, source_hash(source), 1};
}

ArtifactValidation validate(const ArtifactHeader& header, ArtifactKind expected_kind,
                            std::uint32_t minimum_schema_version) {
    if (!header.valid()) {
        return {false, "artifact header is incomplete"};
    }
    if (header.type != artifact_type(expected_kind)) {
        return {false, "artifact type is incompatible"};
    }
    if (header.schema_version < minimum_schema_version) {
        return {false, "artifact schema version is too old"};
    }
    if (header.compiler_version == 0) {
        return {false, "artifact compiler version is invalid"};
    }
    return {true, {}};
}

} // namespace geoworld::tooling
