#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

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

struct ActiveArtifact {
    std::string logical_name;
    ArtifactHeader header;

    auto operator<=>(const ActiveArtifact&) const = default;
};

class ArtifactManifest {
public:
    [[nodiscard]] bool activate(ActiveArtifact artifact);
    [[nodiscard]] bool remove(std::string_view logical_name);
    [[nodiscard]] std::vector<ActiveArtifact> snapshot() const;
    [[nodiscard]] bool restore(std::vector<ActiveArtifact> artifacts);

private:
    std::map<std::string, ArtifactHeader, std::less<>> artifacts_;
};

[[nodiscard]] std::string source_hash(std::string_view source);
[[nodiscard]] ArtifactHeader make_header(ArtifactKind kind, std::uint32_t schema_version,
                                          std::string_view source);
[[nodiscard]] ArtifactValidation validate(const ArtifactHeader& header,
                                          ArtifactKind expected_kind,
                                          std::uint32_t minimum_schema_version = 1);

} // namespace geoworld::tooling
