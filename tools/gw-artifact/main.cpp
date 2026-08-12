#include "geoworld/tooling/artifact.hpp"
#include "geoworld/tooling/behavior_document.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <string>
#include <vector>

namespace {

using geoworld::tooling::ArtifactKind;

ArtifactKind kind() {
#if defined(GW_ARTIFACT_KIND_RULE)
    return ArtifactKind::rule;
#elif defined(GW_ARTIFACT_KIND_BT)
    return ArtifactKind::behavior_tree;
#else
    return ArtifactKind::state_machine;
#endif
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return input ? std::string{std::istreambuf_iterator<char>{input},
                               std::istreambuf_iterator<char>{}} : std::string{};
}

std::vector<std::uint8_t> read_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

bool write_text(const std::filesystem::path& path, std::string_view text) {
    std::ofstream output(path, std::ios::binary);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    return output.good();
}

bool write_bytes(const std::filesystem::path& path, std::span<const std::uint8_t> bytes) {
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return output.good();
}

void print_diagnostics(const geoworld::tooling::DocumentValidation& validation) {
    for (const auto& diagnostic : validation.diagnostics) {
        std::cerr << (diagnostic.severity == geoworld::tooling::DiagnosticSeverity::error ? "错误" : "警告")
                  << ' ' << diagnostic.code;
        if (!diagnostic.object_id.empty()) std::cerr << " [" << diagnostic.object_id << ']';
        std::cerr << ": " << diagnostic.message << '\n';
    }
}

std::filesystem::path schema_path(ArtifactKind artifact_kind) {
    const auto filename = artifact_kind == ArtifactKind::behavior_tree
        ? "behavior_tree.schema.json" : "hfsm.schema.json";
    return std::filesystem::path{GW_SOURCE_DIR} / "schemas" / "tooling" / filename;
}

int legacy_header(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "用法: " << argv[0] << " <源文件>\n";
        return 2;
    }
    const auto source = read_text(argv[1]);
    if (source.empty()) {
        std::cerr << "无法读取源文件\n";
        return 2;
    }
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

int behavior_command(int argc, char** argv) {
#if !GW_HAS_BEHAVIOR_TOOLING
    static_cast<void>(argc);
    static_cast<void>(argv);
    std::cerr << "当前构建未启用 M3 行为工具链依赖，请使用 vcpkg-m3 preset\n";
    return 3;
#else
    if (argc < 3) {
        std::cerr << "用法: " << argv[0]
                  << " validate <文档.json> | dot <文档.json> <输出.dot> | layout <文档.json> <输出.json> | compile <文档.json> <输出制品> | inspect <制品>\n";
        return 2;
    }
    const std::string command = argv[1];
    if (command == "inspect") {
        const auto bytes = read_bytes(argv[2]);
        geoworld::tooling::CompiledBehaviorArtifact artifact;
        const auto validation = geoworld::tooling::load_behavior_artifact(bytes, kind(), artifact);
        print_diagnostics(validation);
        if (!validation.valid()) return 1;
        std::cout << "类型: " << geoworld::tooling::artifact_type(artifact.kind) << '\n'
                  << "文档 ID: " << artifact.document_id << '\n'
                  << "入口 ID: " << artifact.entry_id << '\n'
                  << "Schema 版本: " << artifact.header.schema_version << '\n'
                  << "编译器版本: " << artifact.header.compiler_version << '\n'
                  << "源 Hash: " << artifact.header.source_hash << '\n';
        return 0;
    }
    const auto source = read_text(argv[2]);
    const auto schema = read_text(schema_path(kind()));
    if (source.empty() || schema.empty()) {
        std::cerr << "无法读取编辑文档或 JSON Schema\n";
        return 2;
    }
    auto parsed = kind() == ArtifactKind::behavior_tree
        ? geoworld::tooling::parse_behavior_tree_json(source, schema)
        : geoworld::tooling::parse_hfsm_json(source, schema);
    print_diagnostics(parsed.validation);
    if (!parsed.validation.valid()) return 1;
    if (command == "validate") {
        std::cout << "文档校验通过\n";
        return 0;
    }
    if ((command != "dot" && command != "layout" && command != "compile") || argc != 4) {
        std::cerr << "命令或参数数量错误\n";
        return 2;
    }
    if (command == "dot") {
        const auto dot = kind() == ArtifactKind::behavior_tree
            ? geoworld::tooling::export_dot(parsed.behavior_tree)
            : geoworld::tooling::export_dot(parsed.hfsm);
        if (!write_text(argv[3], dot)) {
            std::cerr << "无法写入 DOT 文件\n";
            return 2;
        }
        std::cout << "DOT 已生成: " << argv[3] << '\n';
        return 0;
    }
    if (command == "layout") {
        const auto validation = kind() == ArtifactKind::behavior_tree
            ? geoworld::tooling::apply_graphviz_layout(parsed.behavior_tree)
            : geoworld::tooling::apply_graphviz_layout(parsed.hfsm);
        print_diagnostics(validation);
        if (!validation.valid()) return 1;
        const auto output = kind() == ArtifactKind::behavior_tree
            ? geoworld::tooling::canonical_json(parsed.behavior_tree)
            : geoworld::tooling::canonical_json(parsed.hfsm);
        if (!write_text(argv[3], output)) {
            std::cerr << "无法写入布局后的编辑文档\n";
            return 2;
        }
        std::cout << "Graphviz 自动布局已写入: " << argv[3] << '\n';
        return 0;
    }
    const auto artifact = kind() == ArtifactKind::behavior_tree
        ? geoworld::tooling::compile(parsed.behavior_tree)
        : geoworld::tooling::compile(parsed.hfsm);
    if (artifact.empty() || !write_bytes(argv[3], artifact)) {
        std::cerr << "制品编译或写入失败\n";
        return 1;
    }
    std::cout << "制品已生成: " << argv[3] << " (" << artifact.size() << " 字节)\n";
    return 0;
#endif
}

} // namespace

int main(int argc, char** argv) {
    if (kind() == ArtifactKind::rule) {
        return legacy_header(argc, argv);
    }
    return behavior_command(argc, argv);
}
