#pragma once

#include "geoworld/spatial/cell_grid.hpp"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace geoworld::spatial {

enum class CellState { unloaded, loading, loaded, active, sleeping };

struct CellRecord {
    CellState state{CellState::unloaded};
    std::uint64_t state_since_ms{};
    std::uint64_t unload_after_ms{};
};

class CellLifecycle {
public:
    [[nodiscard]] bool request_load(CellKey cell, std::uint64_t now_ms);
    [[nodiscard]] bool mark_loaded(CellKey cell, std::uint64_t now_ms);
    [[nodiscard]] bool activate(CellKey cell, std::uint64_t now_ms);
    [[nodiscard]] bool sleep(CellKey cell, std::uint64_t now_ms,
                              std::uint64_t unload_delay_ms);
    [[nodiscard]] bool unload_if_due(CellKey cell, std::uint64_t now_ms);
    [[nodiscard]] CellState state(CellKey cell) const noexcept;
    [[nodiscard]] std::vector<CellKey> known_cells() const;
    [[nodiscard]] std::vector<CellKey> prefetch(CellKey center, int radius) const;

private:
    std::unordered_map<CellKey, CellRecord, CellKeyHash> cells_;
};

} // namespace geoworld::spatial
