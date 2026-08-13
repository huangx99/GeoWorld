#include "geoworld/projection/config.hpp"

namespace geoworld::projection {

namespace {

[[nodiscard]] bool invalid(std::string& diagnostic, std::string reason) {
    diagnostic = std::move(reason);
    return false;
}

} // namespace

[[nodiscard]] bool validate(const ProjectionConfig& config, std::string& diagnostic) {
    if (config.data_frequency_hz == 0) {
        return invalid(diagnostic, "projection: data_frequency_hz must be positive");
    }
    if (config.slow_frequency_hz == 0) {
        return invalid(diagnostic, "projection: slow_frequency_hz must be positive");
    }
    if (config.slow_frequency_hz > config.data_frequency_hz) {
        return invalid(diagnostic,
            "projection: slow_frequency_hz must not exceed data_frequency_hz");
    }
    if (config.keyframe_interval_seconds == 0) {
        return invalid(diagnostic, "projection: keyframe_interval_seconds must be positive");
    }
    if (config.snapshot_history_frames == 0) {
        return invalid(diagnostic, "projection: snapshot_history_frames must be positive");
    }
    if (config.max_unacked_frames == 0) {
        return invalid(diagnostic, "projection: max_unacked_frames must be positive");
    }
    if (config.max_unacked_bytes == 0) {
        return invalid(diagnostic, "projection: max_unacked_bytes must be positive");
    }
    if (config.max_feature_ids_per_epoch == 0) {
        return invalid(diagnostic, "projection: max_feature_ids_per_epoch must be positive");
    }
    if (config.tick_dt_microseconds <= 0) {
        return invalid(diagnostic, "projection: tick_dt_microseconds must be positive");
    }
    if (config.enu_origin.x == 0.0 && config.enu_origin.y == 0.0
        && config.enu_origin.z == 0.0) {
        return invalid(diagnostic, "projection: enu_origin is not configured");
    }
    return true;
}

} // namespace geoworld::projection
