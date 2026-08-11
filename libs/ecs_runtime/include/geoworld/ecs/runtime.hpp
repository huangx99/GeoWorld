#pragma once

#include "geoworld/foundation/ids.hpp"
#include "geoworld/world/world.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

namespace geoworld::ecs {

struct RuntimeEntitySnapshot {
    foundation::WorldId world_id;
    world::PositionEcef position;
    std::string semantic_type;
};

struct RuntimeSnapshot {
    static constexpr std::uint32_t current_format_version = 1;
    static constexpr std::uint32_t current_schema_version = 1;

    std::uint32_t format_version{current_format_version};
    std::uint32_t schema_version{current_schema_version};
    std::vector<RuntimeEntitySnapshot> entities;

    [[nodiscard]] bool valid() const noexcept {
        return format_version == current_format_version
            && schema_version == current_schema_version;
    }
};

class Runtime {
public:
    Runtime();
    ~Runtime();
    Runtime(Runtime&&) noexcept;
    Runtime& operator=(Runtime&&) noexcept;
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    [[nodiscard]] std::optional<foundation::RuntimeId> activate(const world::WorldObject& object);
    [[nodiscard]] bool destroy(foundation::RuntimeId id);
    [[nodiscard]] bool valid(foundation::RuntimeId id) const noexcept;
    [[nodiscard]] std::optional<foundation::RuntimeId> find(foundation::WorldId id) const noexcept;
    [[nodiscard]] std::optional<foundation::WorldId> world_id(foundation::RuntimeId id) const noexcept;
    [[nodiscard]] RuntimeSnapshot snapshot() const;
    [[nodiscard]] bool restore(const RuntimeSnapshot& snapshot);
    [[nodiscard]] std::size_t size() const noexcept;
    void clear();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace geoworld::ecs
