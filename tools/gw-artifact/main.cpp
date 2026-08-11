#include "geoworld/tooling/artifact.hpp"

#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace {

geoworld::tooling::ArtifactKind kind() {
#if defined(GW_ARTIFACT_KIND_RULE)
    return geoworld::tooling::ArtifactKind::rule;
#elif defined(GW_ARTIFACT_KIND_BT)
    return geoworld::tooling::ArtifactKind::behavior_tree;
#else
    return geoworld::tooling::ArtifactKind::state_machine;
#endif
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: " << argv[0] << " <source-file>\n";
        return 2;
    }
    std::ifstream input(argv[1], std::ios::binary);
    if (!input) {
        std::cerr << "cannot open source file\n";
        return 2;
    }
    const std::string source{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    const auto header = geoworld::tooling::make_header(kind(), 1, source);
    const auto validation = geoworld::tooling::validate(header, kind());
    if (!validation.valid) {
        std::cerr << validation.message << '\n';
        return 1;
    }
    std::cout << "type=" << header.type << '\n'
              << "schema_version=" << header.schema_version << '\n'
              << "compiler_version=" << header.compiler_version << '\n'
              << "source_hash=" << header.source_hash << '\n';
    return 0;
}
