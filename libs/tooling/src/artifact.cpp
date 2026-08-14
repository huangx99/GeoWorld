#include "geoworld/tooling/artifact.hpp"

#include <iomanip>
#include <sstream>
#include <utility>

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

bool ArtifactManifest::activate(ActiveArtifact artifact) {
    if (artifact.logical_name.empty() || !artifact.header.valid()) {
        return false;
    }
    artifacts_.insert_or_assign(std::move(artifact.logical_name),
                                std::move(artifact.header));
    return true;
}

bool ArtifactManifest::remove(std::string_view logical_name) {
    const auto found = artifacts_.find(logical_name);
    if (found == artifacts_.end()) {
        return false;
    }
    artifacts_.erase(found);
    return true;
}

std::vector<ActiveArtifact> ArtifactManifest::snapshot() const {
    std::vector<ActiveArtifact> result;
    result.reserve(artifacts_.size());
    for (const auto& [name, header] : artifacts_) {
        result.push_back({name, header});
    }
    return result;
}

bool ArtifactManifest::restore(std::vector<ActiveArtifact> artifacts) {
    std::map<std::string, ArtifactHeader, std::less<>> restored;
    for (auto& artifact : artifacts) {
        if (artifact.logical_name.empty() || !artifact.header.valid()
            || !restored.emplace(std::move(artifact.logical_name),
                                 std::move(artifact.header)).second) {
            return false;
        }
    }
    artifacts_ = std::move(restored);
    return true;
}

} // namespace geoworld::tooling
