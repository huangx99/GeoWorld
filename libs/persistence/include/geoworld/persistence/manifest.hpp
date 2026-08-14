#pragma once

#include "geoworld/foundation/ids.hpp"
#include "geoworld/persistence/storage.hpp"
#include "geoworld/persistence/types.hpp"

#include <compare>
#include <cstdint>
#include <filesystem>

namespace geoworld::persistence {

// 目录 manifest 记录布局版本与身份，原子提交；检查点/回放/分支 manifest
// 由 schemas/persistence 下的版本化 schema 定义（checkpoint manifest 在 M5-B 落地）。
inline constexpr std::uint32_t kDirectoryManifestFormatVersion = 1;

struct DirectoryManifest {
    std::uint32_t format_version{kDirectoryManifestFormatVersion};
    geoworld::foundation::WorldId world{};
    BranchId branch{};

    constexpr auto operator<=>(const DirectoryManifest&) const = default;
};

[[nodiscard]] Result<DirectoryManifest> load_directory_manifest(
    FileOps& ops, const std::filesystem::path& manifest_path);

[[nodiscard]] PersistenceError publish_directory_manifest(
    FileOps& ops, const std::filesystem::path& manifest_path, const DirectoryManifest& manifest);

} // namespace geoworld::persistence
