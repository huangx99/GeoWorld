#include "geoworld/persistence/replay.hpp"

#include <filesystem>
#include <map>

#include <unistd.h>

int main() {
    using namespace geoworld::persistence;
    const auto root = std::filesystem::temp_directory_path()
                      / ("gw-replay-test-" + std::to_string(::getpid()));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);

    ReplayManifestData source;
    source.world = geoworld::foundation::WorldId{8};
    source.branch = parse_branch_id("00000000-0000-0000-0000-000000000008").value();
    source.base_checkpoint = "checkpoints/ckpt-8.gwckm";
    source.first_lsn = Lsn{9};
    source.last_lsn = Lsn{19};
    source.dt_microseconds = 50'000;
    source.schema_registry_fingerprint = "schema-v1";
    source.build_compatibility = "geoworld-0.1";
    const auto path = root / "replay.gwrm";
    auto published = publish_replay_manifest(path, source);
    auto loaded = load_replay_manifest(path);
    if (!published.ok() || !loaded.ok() || loaded.value.world != source.world
        || loaded.value.branch != source.branch
        || loaded.value.base_checkpoint != source.base_checkpoint
        || loaded.value.first_lsn != source.first_lsn
        || loaded.value.last_lsn != source.last_lsn
        || loaded.value.dt_microseconds != source.dt_microseconds
        || loaded.value.schema_registry_fingerprint != source.schema_registry_fingerprint
        || loaded.value.build_compatibility != source.build_compatibility
        || loaded.value.side_effects_enabled) {
        return 1;
    }

    ReplayManifestData invalid = source;
    invalid.first_lsn = Lsn{20};
    if (publish_replay_manifest(root / "invalid.gwrm", invalid).ok()) return 2;

    const std::map<std::uint64_t, std::uint64_t> left{{1, 11}, {2, 22}, {3, 33}};
    const std::map<std::uint64_t, std::uint64_t> equal = left;
    const std::map<std::uint64_t, std::uint64_t> changed{{1, 11}, {2, 99}, {3, 33}};
    const std::map<std::uint64_t, std::uint64_t> missing{{1, 11}, {3, 33}};
    if (first_hash_divergence(left, equal).has_value()) return 3;
    const auto value_divergence = first_hash_divergence(left, changed);
    if (!value_divergence.has_value() || value_divergence->tick != 2
        || value_divergence->left != 22 || value_divergence->right != 99) return 4;
    const auto missing_divergence = first_hash_divergence(left, missing);
    if (!missing_divergence.has_value() || missing_divergence->tick != 2
        || missing_divergence->left != 22 || missing_divergence->right.has_value()) return 5;

    std::filesystem::remove_all(root, ec);
    return 0;
}
