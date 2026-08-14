#include "geoworld/world/snapshot.hpp"

namespace geoworld::world {

WorldSnapshot capture_snapshot(const World& world) noexcept {
    // snapshot() 已按 id 排序，编码与 hash 的确定性依赖该排序。
    return WorldSnapshot{world.snapshot(), world.next_revision(), world.erase_revision()};
}

void restore_snapshot(World& world, WorldSnapshot snapshot) {
    world.restore(std::move(snapshot.objects), snapshot.next_revision,
                  snapshot.erase_revision);
}

} // namespace geoworld::world
