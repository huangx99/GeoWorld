#include "geoworld/persistence/branch.hpp"

#include "geoworld/persistence/recovery.hpp"

#include "branch_manifest_generated.h"

#include <flatbuffers/flatbuffers.h>

#include <algorithm>
#include <cstring>
#include <utility>

namespace geoworld::persistence {
namespace {

namespace fbs = geoworld::persistence::v1;
inline constexpr std::string_view kBranchManifestFile = "branch.gwbm";

std::vector<std::uint8_t> branch_bytes(BranchId id) {
    return {id.bytes.begin(), id.bytes.end()};
}

bool read_branch(const flatbuffers::Vector<std::uint8_t>* bytes, BranchId& id) {
    if (bytes == nullptr || bytes->size() != id.bytes.size()) return false;
    std::memcpy(id.bytes.data(), bytes->data(), id.bytes.size());
    return true;
}

bool same_object(const world::WorldObject& left, const world::WorldObject& right) {
    return left.id == right.id && left.geometry_ref == right.geometry_ref
           && left.position.x == right.position.x && left.position.y == right.position.y
           && left.position.z == right.position.z && left.semantic_type == right.semantic_type
           && left.properties == right.properties && left.state == right.state
           && left.relations.size() == right.relations.size()
           && left.capabilities == right.capabilities && left.lifecycle == right.lifecycle
           && left.version == right.version && left.revision == right.revision
           && std::equal(left.relations.begin(), left.relations.end(), right.relations.begin(),
                         [](const auto& a, const auto& b) {
                             return a.target == b.target && a.type == b.type
                                    && a.attributes == b.attributes;
                         });
}

} // namespace

std::filesystem::path branch_manifest_path(const DurableLayout& layout) {
    return layout.manifest_dir() / kBranchManifestFile;
}

Result<std::filesystem::path> publish_branch_manifest(
    const DurableLayout& layout, const BranchManifestData& manifest,
    std::shared_ptr<FileOps> file_ops) {
    if (!manifest.world.valid() || !manifest.branch.valid() || !manifest.parent_branch.valid()
        || manifest.base_checkpoint.empty() || manifest.overlay_wal.empty()) {
        return {{}, PersistenceError::config_invalid};
    }
    auto ops = file_ops ? std::move(file_ops) : make_posix_file_ops();
    const auto directory = layout.manifest_dir();
    PersistenceError error = ops->create_directories(directory);
    if (error != PersistenceError::none) return {{}, error};
    flatbuffers::FlatBufferBuilder builder;
    const auto root = fbs::CreateBranchManifest(
        builder, 1, manifest.world.value,
        builder.CreateVector(branch_bytes(manifest.branch)),
        builder.CreateVector(branch_bytes(manifest.parent_branch)),
        manifest.fork_tick, manifest.fork_lsn.value,
        builder.CreateString(manifest.base_checkpoint.generic_string()),
        builder.CreateString(manifest.overlay_wal.generic_string()),
        manifest.side_effects_enabled);
    fbs::FinishBranchManifestBuffer(builder, root);
    std::vector<std::byte> bytes(builder.GetSize());
    std::memcpy(bytes.data(), builder.GetBufferPointer(), builder.GetSize());
    const auto path = branch_manifest_path(layout);
    error = atomic_publish(*ops, path, bytes);
    return error == PersistenceError::none
               ? Result<std::filesystem::path>{path, PersistenceError::none}
               : Result<std::filesystem::path>{{}, error};
}

Result<BranchManifestData> load_branch_manifest(
    const std::filesystem::path& path, std::shared_ptr<FileOps> file_ops) {
    auto ops = file_ops ? std::move(file_ops) : make_posix_file_ops();
    auto read = ops->read_file(path);
    if (!read.ok()) return {{}, read.error};
    flatbuffers::Verifier verifier(reinterpret_cast<const std::uint8_t*>(read.value.data()),
                                   read.value.size());
    if (!fbs::VerifyBranchManifestBuffer(verifier))
        return {{}, PersistenceError::manifest_invalid};
    const auto* manifest = fbs::GetBranchManifest(read.value.data());
    Result<BranchManifestData> result;
    if (manifest->format_version() != 1
        || !read_branch(manifest->branch_id(), result.value.branch)
        || !read_branch(manifest->parent_branch_id(), result.value.parent_branch)
        || manifest->base_checkpoint() == nullptr || manifest->overlay_wal() == nullptr) {
        return {{}, PersistenceError::manifest_invalid};
    }
    result.value.world = foundation::WorldId{manifest->world_id()};
    result.value.fork_tick = manifest->fork_tick();
    result.value.fork_lsn = Lsn{manifest->fork_lsn()};
    result.value.base_checkpoint = manifest->base_checkpoint()->str();
    result.value.overlay_wal = manifest->overlay_wal()->str();
    result.value.side_effects_enabled = manifest->side_effects_enabled();
    if (!result.value.world.valid() || !result.value.branch.valid()
        || !result.value.parent_branch.valid() || result.value.base_checkpoint.empty()
        || result.value.overlay_wal.empty()) return {{}, PersistenceError::manifest_invalid};
    return result;
}

Result<BranchManifestData> fork_branch_at_checkpoint(
    const DurableLayout& parent_layout, const DurableLayout& child_layout,
    geoworld::foundation::WorldId world, BranchId parent, BranchId child,
    std::optional<std::uint64_t> checkpoint_tick, std::shared_ptr<FileOps> file_ops) {
    if (!world.valid() || !parent.valid() || !child.valid() || parent == child) {
        return {{}, PersistenceError::config_invalid};
    }
    auto ops = file_ops ? std::move(file_ops) : make_posix_file_ops();
    CheckpointLoader loader{parent_layout, world, parent, ops};
    auto checkpoints = loader.list_valid();
    if (!checkpoints.ok()) {
        return {{}, checkpoints.error};
    }
    const auto selected = checkpoint_tick.has_value()
        ? std::find_if(checkpoints.value.begin(), checkpoints.value.end(),
                       [checkpoint_tick](const CheckpointInfo& candidate) {
                           return candidate.anchor.completed_tick == *checkpoint_tick;
                       })
        : (checkpoints.value.empty() ? checkpoints.value.end()
                                     : std::prev(checkpoints.value.end()));
    if (selected == checkpoints.value.end()) {
        return {{}, PersistenceError::not_found};
    }
    const PersistenceError created = ops->create_directories(child_layout.wal_dir());
    if (created != PersistenceError::none) {
        return {{}, created};
    }
    BranchManifestData manifest;
    manifest.world = world;
    manifest.branch = child;
    manifest.parent_branch = parent;
    manifest.fork_tick = selected->anchor.completed_tick;
    manifest.fork_lsn = selected->anchor.included_lsn;
    manifest.base_checkpoint = selected->manifest_path;
    manifest.overlay_wal = child_layout.wal_dir();
    const auto published = publish_branch_manifest(child_layout, manifest, ops);
    return published.ok() ? Result<BranchManifestData>{std::move(manifest),
                                                       PersistenceError::none}
                          : Result<BranchManifestData>{{}, published.error};
}

WorldOverlay::WorldOverlay(world::FrozenWorldSnapshot baseline)
    : baseline_(std::move(baseline)) {}

const world::WorldObject* WorldOverlay::find(foundation::WorldId id) const noexcept {
    if (tombstones_.contains(id)) return nullptr;
    const auto changed = overlay_.find(id);
    if (changed != overlay_.end()) return &changed->second;
    return baseline_.find(id);
}

bool WorldOverlay::upsert(world::WorldObject object) {
    if (!object.id.valid()) return false;
    tombstones_.erase(object.id);
    overlay_.insert_or_assign(object.id, std::move(object));
    return true;
}

bool WorldOverlay::erase(foundation::WorldId id) {
    if (!id.valid() || find(id) == nullptr) return false;
    overlay_.erase(id);
    tombstones_.insert(id);
    return true;
}

world::WorldSnapshot WorldOverlay::materialize() const {
    world::WorldSnapshot result;
    result.next_revision = baseline_.next_revision();
    result.erase_revision = baseline_.erase_revision();
    std::map<foundation::WorldId, world::WorldObject> merged;
    baseline_.for_each_object([&](const world::WorldObject& object) {
        if (!tombstones_.contains(object.id)) merged.emplace(object.id, object);
    });
    for (const auto& [id, object] : overlay_) merged.insert_or_assign(id, object);
    result.objects.clear(); result.objects.reserve(merged.size());
    for (auto& [id, object] : merged) {
        static_cast<void>(id); result.objects.push_back(std::move(object));
    }
    return result;
}

std::size_t WorldOverlay::overlay_size() const noexcept { return overlay_.size(); }
std::size_t WorldOverlay::tombstone_count() const noexcept { return tombstones_.size(); }

std::vector<ObjectDifference> compare_world_snapshots(
    const world::WorldSnapshot& left, const world::WorldSnapshot& right) {
    std::vector<ObjectDifference> result;
    std::size_t a{}; std::size_t b{};
    while (a < left.objects.size() || b < right.objects.size()) {
        if (b == right.objects.size()
            || (a < left.objects.size() && left.objects[a].id < right.objects[b].id)) {
            result.push_back({left.objects[a++].id, ObjectDifference::Kind::removed});
        } else if (a == left.objects.size() || right.objects[b].id < left.objects[a].id) {
            result.push_back({right.objects[b++].id, ObjectDifference::Kind::added});
        } else {
            if (!same_object(left.objects[a], right.objects[b]))
                result.push_back({left.objects[a].id, ObjectDifference::Kind::changed});
            ++a; ++b;
        }
    }
    return result;
}

} // namespace geoworld::persistence
