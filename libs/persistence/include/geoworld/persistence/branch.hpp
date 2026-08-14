#pragma once

#include "geoworld/persistence/storage.hpp"
#include "geoworld/persistence/types.hpp"
#include "geoworld/world/snapshot.hpp"

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace geoworld::persistence {

struct BranchManifestData {
    geoworld::foundation::WorldId world{};
    BranchId branch{};
    BranchId parent_branch{};
    std::uint64_t fork_tick{};
    Lsn fork_lsn{};
    std::filesystem::path base_checkpoint;
    std::filesystem::path overlay_wal;
    bool side_effects_enabled{};
};

[[nodiscard]] std::filesystem::path branch_manifest_path(const DurableLayout& layout);
[[nodiscard]] Result<std::filesystem::path> publish_branch_manifest(
    const DurableLayout& layout, const BranchManifestData& manifest,
    std::shared_ptr<FileOps> file_ops = {});
[[nodiscard]] Result<BranchManifestData> load_branch_manifest(
    const std::filesystem::path& path, std::shared_ptr<FileOps> file_ops = {});
[[nodiscard]] Result<BranchManifestData> fork_branch_at_checkpoint(
    const DurableLayout& parent_layout, const DurableLayout& child_layout,
    geoworld::foundation::WorldId world, BranchId parent, BranchId child,
    std::optional<std::uint64_t> checkpoint_tick = std::nullopt,
    std::shared_ptr<FileOps> file_ops = {});

class WorldOverlay {
public:
    explicit WorldOverlay(world::FrozenWorldSnapshot baseline);
    [[nodiscard]] const world::WorldObject* find(foundation::WorldId id) const noexcept;
    [[nodiscard]] bool upsert(world::WorldObject object);
    [[nodiscard]] bool erase(foundation::WorldId id);
    [[nodiscard]] world::WorldSnapshot materialize() const;
    [[nodiscard]] std::size_t overlay_size() const noexcept;
    [[nodiscard]] std::size_t tombstone_count() const noexcept;

private:
    world::FrozenWorldSnapshot baseline_;
    std::map<foundation::WorldId, world::WorldObject> overlay_;
    std::set<foundation::WorldId> tombstones_;
};

class SideEffectSink {
public:
    virtual ~SideEffectSink() = default;
    [[nodiscard]] virtual bool emit(std::string_view channel,
                                    std::span<const std::byte> payload) = 0;
};

class DisabledSideEffectSink final : public SideEffectSink {
public:
    [[nodiscard]] bool emit(std::string_view, std::span<const std::byte>) override {
        return false;
    }
};

struct ObjectDifference {
    foundation::WorldId id;
    enum class Kind { added, removed, changed } kind{Kind::changed};
};

[[nodiscard]] std::vector<ObjectDifference> compare_world_snapshots(
    const world::WorldSnapshot& left, const world::WorldSnapshot& right);

} // namespace geoworld::persistence
