#include "geoworld/persistence/replay.hpp"

#include "replay_manifest_generated.h"

#include <flatbuffers/flatbuffers.h>

#include <cstring>
#include <utility>

namespace geoworld::persistence {
namespace {

namespace fbs = geoworld::persistence::v1;

std::vector<std::uint8_t> branch_bytes(BranchId id) {
    return {id.bytes.begin(), id.bytes.end()};
}

bool read_branch(const flatbuffers::Vector<std::uint8_t>* bytes, BranchId& id) {
    if (bytes == nullptr || bytes->size() != id.bytes.size()) return false;
    std::memcpy(id.bytes.data(), bytes->data(), id.bytes.size());
    return true;
}

} // namespace

Result<std::filesystem::path> publish_replay_manifest(
    const std::filesystem::path& output, const ReplayManifestData& manifest,
    std::shared_ptr<FileOps> file_ops) {
    if (!manifest.world.valid() || !manifest.branch.valid() || manifest.base_checkpoint.empty()
        || !manifest.first_lsn.valid() || !manifest.last_lsn.valid()
        || manifest.first_lsn > manifest.last_lsn || manifest.dt_microseconds <= 0
        || manifest.schema_registry_fingerprint.empty() || manifest.build_compatibility.empty()
        || manifest.hash_algorithm_version == 0) return {{}, PersistenceError::config_invalid};
    auto ops = file_ops ? std::move(file_ops) : make_posix_file_ops();
    const auto parent = output.parent_path().empty() ? std::filesystem::path{"."}
                                                     : output.parent_path();
    auto error = ops->create_directories(parent);
    if (error != PersistenceError::none) return {{}, error};
    flatbuffers::FlatBufferBuilder builder;
    const auto root = fbs::CreateReplayManifest(
        builder, 1, manifest.world.value, builder.CreateVector(branch_bytes(manifest.branch)),
        builder.CreateString(manifest.base_checkpoint.generic_string()),
        manifest.first_lsn.value, manifest.last_lsn.value, manifest.dt_microseconds,
        builder.CreateString(manifest.schema_registry_fingerprint),
        builder.CreateString(manifest.build_compatibility), manifest.hash_algorithm_version,
        manifest.side_effects_enabled);
    fbs::FinishReplayManifestBuffer(builder, root);
    std::vector<std::byte> bytes(builder.GetSize());
    std::memcpy(bytes.data(), builder.GetBufferPointer(), builder.GetSize());
    error = atomic_publish(*ops, output, bytes);
    return error == PersistenceError::none
               ? Result<std::filesystem::path>{output, PersistenceError::none}
               : Result<std::filesystem::path>{{}, error};
}

Result<ReplayManifestData> load_replay_manifest(
    const std::filesystem::path& path, std::shared_ptr<FileOps> file_ops) {
    auto ops = file_ops ? std::move(file_ops) : make_posix_file_ops();
    auto bytes = ops->read_file(path);
    if (!bytes.ok()) return {{}, bytes.error};
    flatbuffers::Verifier verifier(reinterpret_cast<const std::uint8_t*>(bytes.value.data()),
                                   bytes.value.size());
    if (!fbs::VerifyReplayManifestBuffer(verifier))
        return {{}, PersistenceError::manifest_invalid};
    const auto* source = fbs::GetReplayManifest(bytes.value.data());
    Result<ReplayManifestData> result;
    if (source->format_version() != 1 || !read_branch(source->branch_id(), result.value.branch)
        || source->base_checkpoint() == nullptr
        || source->schema_registry_fingerprint() == nullptr
        || source->build_compatibility() == nullptr) return {{}, PersistenceError::manifest_invalid};
    result.value.world = geoworld::foundation::WorldId{source->world_id()};
    result.value.base_checkpoint = source->base_checkpoint()->str();
    result.value.first_lsn = Lsn{source->first_lsn()};
    result.value.last_lsn = Lsn{source->last_lsn()};
    result.value.dt_microseconds = source->dt_microseconds();
    result.value.schema_registry_fingerprint = source->schema_registry_fingerprint()->str();
    result.value.build_compatibility = source->build_compatibility()->str();
    result.value.hash_algorithm_version = source->hash_algorithm_version();
    result.value.side_effects_enabled = source->side_effects_enabled();
    if (!result.value.world.valid() || !result.value.branch.valid()
        || result.value.base_checkpoint.empty() || !result.value.first_lsn.valid()
        || result.value.first_lsn > result.value.last_lsn || result.value.dt_microseconds <= 0
        || result.value.schema_registry_fingerprint.empty()
        || result.value.build_compatibility.empty()
        || result.value.hash_algorithm_version == 0) return {{}, PersistenceError::manifest_invalid};
    return result;
}

std::optional<HashDivergence> first_hash_divergence(
    const std::map<std::uint64_t, std::uint64_t>& left,
    const std::map<std::uint64_t, std::uint64_t>& right) noexcept {
    auto a = left.begin();
    auto b = right.begin();
    while (a != left.end() || b != right.end()) {
        if (b == right.end() || (a != left.end() && a->first < b->first))
            return HashDivergence{a->first, a->second, std::nullopt};
        if (a == left.end() || b->first < a->first)
            return HashDivergence{b->first, std::nullopt, b->second};
        if (a->second != b->second) return HashDivergence{a->first, a->second, b->second};
        ++a; ++b;
    }
    return std::nullopt;
}

} // namespace geoworld::persistence
