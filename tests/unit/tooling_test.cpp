#include "geoworld/tooling/artifact.hpp"

int main() {
    using geoworld::tooling::ArtifactKind;
    const auto first = geoworld::tooling::make_header(ArtifactKind::behavior_tree, 1, "<tree/>");
    const auto second = geoworld::tooling::make_header(ArtifactKind::behavior_tree, 1, "<tree/>");
    if (!first.valid() || first.source_hash != second.source_hash
        || geoworld::tooling::validate(first, ArtifactKind::behavior_tree).valid != true
        || geoworld::tooling::validate(first, ArtifactKind::rule).valid
        || geoworld::tooling::validate(first, ArtifactKind::behavior_tree, 2).valid) {
        return 1;
    }
    auto invalid = first;
    invalid.compiler_version = 0;
    return geoworld::tooling::validate(invalid, ArtifactKind::behavior_tree).valid ? 1 : 0;
}
