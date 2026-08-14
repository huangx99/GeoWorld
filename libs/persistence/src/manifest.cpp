#include "geoworld/persistence/manifest.hpp"

#include "le_codec.hpp"
#include "wal_codec.hpp"

#include <crc32c/crc32c.h>

#include <array>
#include <cstring>
#include <vector>

namespace geoworld::persistence {

namespace {

// 目录 manifest 线格式（全部 little-endian）：标识 4 字节、格式版本 u32、
// 世界 ID u64、分支 ID 16 字节、CRC32C u32。标识与版本为具名协议常量。
constexpr std::array<char, 4> kManifestMagic{'G', 'W', 'D', 'M'};
constexpr std::size_t manifest_body_bytes =
    4 + sizeof(std::uint32_t) + sizeof(std::uint64_t) + 16;
constexpr std::size_t manifest_total_bytes = manifest_body_bytes + sizeof(std::uint32_t);

[[nodiscard]] std::vector<std::byte> encode_manifest(const DirectoryManifest& manifest) {
    std::vector<std::byte> out(manifest_total_bytes);
    std::memcpy(out.data(), kManifestMagic.data(), kManifestMagic.size());
    detail::write_le32(out.data() + kManifestMagic.size(), manifest.format_version);
    detail::write_le64(out.data() + kManifestMagic.size() + sizeof(std::uint32_t),
                       manifest.world.value);
    std::memcpy(out.data() + kManifestMagic.size() + sizeof(std::uint32_t)
                    + sizeof(std::uint64_t),
                manifest.branch.bytes.data(), manifest.branch.bytes.size());
    const std::uint32_t crc = crc32c::Crc32c(
        reinterpret_cast<const std::uint8_t*>(out.data()), manifest_body_bytes);
    detail::write_le32(out.data() + manifest_body_bytes, crc);
    return out;
}

} // namespace

Result<DirectoryManifest> load_directory_manifest(FileOps& ops,
                                                  const std::filesystem::path& manifest_path) {
    auto read = ops.read_file(manifest_path);
    if (!read.ok()) {
        return {{}, read.error};
    }
    const std::vector<std::byte>& bytes = read.value;
    if (bytes.size() != manifest_total_bytes) {
        return {{}, PersistenceError::manifest_invalid};
    }
    if (std::memcmp(bytes.data(), kManifestMagic.data(), kManifestMagic.size()) != 0) {
        return {{}, PersistenceError::manifest_invalid};
    }
    const std::uint32_t expected_crc = detail::read_le32(bytes.data() + manifest_body_bytes);
    const std::uint32_t actual_crc = crc32c::Crc32c(
        reinterpret_cast<const std::uint8_t*>(bytes.data()), manifest_body_bytes);
    if (expected_crc != actual_crc) {
        return {{}, PersistenceError::checksum_mismatch};
    }
    DirectoryManifest manifest;
    manifest.format_version = detail::read_le32(bytes.data() + kManifestMagic.size());
    if (manifest.format_version != kDirectoryManifestFormatVersion) {
        return {{}, PersistenceError::manifest_invalid};
    }
    manifest.world = geoworld::foundation::WorldId{
        detail::read_le64(bytes.data() + kManifestMagic.size() + sizeof(std::uint32_t))};
    std::memcpy(manifest.branch.bytes.data(),
                bytes.data() + kManifestMagic.size() + sizeof(std::uint32_t)
                    + sizeof(std::uint64_t),
                manifest.branch.bytes.size());
    if (!manifest.world.valid() || !manifest.branch.valid()) {
        return {{}, PersistenceError::manifest_invalid};
    }
    return {manifest, PersistenceError::none};
}

PersistenceError publish_directory_manifest(FileOps& ops,
                                            const std::filesystem::path& manifest_path,
                                            const DirectoryManifest& manifest) {
    const std::vector<std::byte> bytes = encode_manifest(manifest);
    return atomic_publish(ops, manifest_path, bytes);
}

} // namespace geoworld::persistence
