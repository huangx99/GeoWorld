#include "geoworld/persistence/types.hpp"

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

#include <algorithm>
#include <cstring>

namespace geoworld::persistence {

BranchId generate_branch_id() {
    // Boost.UUID 只存在于实现层；公共线格式固定为 16 字节。
    const boost::uuids::uuid id = boost::uuids::random_generator()();
    BranchId branch;
    std::memcpy(branch.bytes.data(), id.data, branch.bytes.size());
    return branch;
}

std::optional<BranchId> parse_branch_id(std::string_view text) noexcept {
    try {
        const boost::uuids::uuid id = boost::uuids::string_generator()(std::string{text});
        BranchId branch;
        std::memcpy(branch.bytes.data(), id.data, branch.bytes.size());
        return branch;
    } catch (...) {
        return std::nullopt;
    }
}

std::string format_branch_id(BranchId id) {
    boost::uuids::uuid uuid{};
    std::memcpy(uuid.data, id.bytes.data(), id.bytes.size());
    return boost::uuids::to_string(uuid);
}

const char* error_code(PersistenceError error) noexcept {
    switch (error) {
    case PersistenceError::none:
        return "";
    case PersistenceError::record_invalid:
        return error_record_invalid.data();
    case PersistenceError::lsn_discontinuity:
        return error_lsn_discontinuity.data();
    case PersistenceError::checksum_mismatch:
        return error_checksum_mismatch.data();
    case PersistenceError::segment_corrupted:
        return error_segment_corrupted.data();
    case PersistenceError::io_failure:
        return error_io_failure.data();
    case PersistenceError::sync_failed:
        return error_sync_failed.data();
    case PersistenceError::no_space_or_permission:
        return error_no_space_or_permission.data();
    case PersistenceError::queue_full:
        return error_queue_full.data();
    case PersistenceError::fault_read_only:
        return error_fault_read_only.data();
    case PersistenceError::config_invalid:
        return error_config_invalid.data();
    case PersistenceError::lsn_overflow:
        return error_lsn_overflow.data();
    case PersistenceError::manifest_invalid:
        return error_manifest_invalid.data();
    case PersistenceError::not_found:
        return error_not_found.data();
    case PersistenceError::shutting_down:
        return error_shutting_down.data();
    case PersistenceError::relaxed_not_allowed:
        return error_relaxed_not_allowed.data();
    case PersistenceError::provider_missing:
        return error_provider_missing.data();
    case PersistenceError::provider_unknown:
        return error_provider_unknown.data();
    case PersistenceError::provider_version_mismatch:
        return error_provider_version_mismatch.data();
    case PersistenceError::checkpoint_invalid:
        return error_checkpoint_invalid.data();
    case PersistenceError::checkpoint_incomplete:
        return error_checkpoint_incomplete.data();
    }
    return "";
}

} // namespace geoworld::persistence
