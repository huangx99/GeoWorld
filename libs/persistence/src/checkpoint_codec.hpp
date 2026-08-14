#pragma once

// 内部检查点 FlatBuffers 编解码：生成类型不进公共 API。
// 确定性依据：相同字段顺序、相同排序输入经同一 builder 构造产生逐字节一致输出。

#include "geoworld/persistence/checkpoint.hpp"
#include "geoworld/persistence/types.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace geoworld::persistence::detail {

struct DecodedManifest {
    CheckpointAnchor anchor{};
    std::uint64_t checkpoint_content_hash{};
    geoworld::foundation::WorldId world{};
    BranchId branch{};
    std::vector<CheckpointSchema> provider_schemas;
    std::string data_file;
    std::uint64_t data_length{};
    std::uint32_t data_crc32c{};
    std::uint64_t data_uncompressed_length{};
    bool zstd_compressed{};
};

struct DecodedBlock {
    CheckpointSchema schema;
    std::vector<std::byte> payload;
};

struct DecodedCheckpointData {
    CheckpointAnchor anchor{};
    std::uint64_t checkpoint_content_hash{};
    geoworld::foundation::WorldId world{};
    BranchId branch{};
    std::vector<DecodedBlock> blocks;
};

[[nodiscard]] std::uint64_t authoritative_checkpoint_hash(
    const std::vector<CheckpointBlock>& blocks) noexcept;

[[nodiscard]] std::vector<std::byte> encode_world_checkpoint(
    geoworld::foundation::WorldId world, BranchId branch, const CheckpointAnchor& anchor,
    std::uint64_t checkpoint_content_hash, const std::vector<CheckpointBlock>& blocks);

[[nodiscard]] Result<DecodedCheckpointData> decode_world_checkpoint(
    std::span<const std::byte> bytes);

[[nodiscard]] std::vector<std::byte> encode_checkpoint_manifest(
    geoworld::foundation::WorldId world, BranchId branch, const CheckpointAnchor& anchor,
    std::uint64_t checkpoint_content_hash,
    const std::vector<CheckpointSchema>& provider_schemas, const std::string& data_file,
    std::uint64_t data_length, std::uint32_t data_crc32c,
    std::uint64_t data_uncompressed_length, bool zstd_compressed);

[[nodiscard]] Result<DecodedManifest> decode_checkpoint_manifest(
    std::span<const std::byte> bytes);

// 已发布检查点对的完整校验：manifest 解码、数据文件长度与 CRC32C、
// 数据解码（含块 payload CRC）、锚点/schema 列表/身份一致性。
struct VerifiedCheckpoint {
    DecodedManifest manifest;
    DecodedCheckpointData data;
};

[[nodiscard]] Result<VerifiedCheckpoint> verify_checkpoint_pair(
    FileOps& ops, const std::filesystem::path& checkpoint_dir,
    const std::filesystem::path& manifest_path, geoworld::foundation::WorldId world,
    BranchId branch);

} // namespace geoworld::persistence::detail
