#pragma once

#include "geoworld/persistence/storage.hpp"
#include "geoworld/persistence/types.hpp"

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>

namespace geoworld::persistence {

inline constexpr std::uint32_t kAuthoritativeHashAlgorithmVersion = 1;

struct ReplayManifestData {
    geoworld::foundation::WorldId world{};
    BranchId branch{};
    std::filesystem::path base_checkpoint;
    Lsn first_lsn{};
    Lsn last_lsn{};
    std::int64_t dt_microseconds{};
    std::string schema_registry_fingerprint;
    std::string build_compatibility;
    std::uint32_t hash_algorithm_version{kAuthoritativeHashAlgorithmVersion};
    bool side_effects_enabled{};
};

[[nodiscard]] Result<std::filesystem::path> publish_replay_manifest(
    const std::filesystem::path& output, const ReplayManifestData& manifest,
    std::shared_ptr<FileOps> file_ops = {});
[[nodiscard]] Result<ReplayManifestData> load_replay_manifest(
    const std::filesystem::path& path, std::shared_ptr<FileOps> file_ops = {});

struct HashDivergence {
    std::uint64_t tick{};
    std::optional<std::uint64_t> left;
    std::optional<std::uint64_t> right;
};

[[nodiscard]] std::optional<HashDivergence> first_hash_divergence(
    const std::map<std::uint64_t, std::uint64_t>& left,
    const std::map<std::uint64_t, std::uint64_t>& right) noexcept;

} // namespace geoworld::persistence
