#include "checkpoint_codec.hpp"

#include "checkpoint_manifest_generated.h"
#include "world_checkpoint_generated.h"

#include <crc32c/crc32c.h>
#include <flatbuffers/flatbuffers.h>
#include <zstd.h>

#include <cstring>
#include <filesystem>

namespace geoworld::persistence::detail {

namespace {

namespace fbs = geoworld::persistence::v1;
inline constexpr std::size_t kMaxCheckpointBytes = 16ULL * 1024ULL * 1024ULL * 1024ULL;
inline constexpr std::size_t kMaxProviderBlocks = 1024;

template <typename FbsRoot>
[[nodiscard]] std::vector<std::byte> finish_to_bytes(
    flatbuffers::FlatBufferBuilder& builder, flatbuffers::Offset<FbsRoot> root,
    void (*finish)(flatbuffers::FlatBufferBuilder&, flatbuffers::Offset<FbsRoot>)) {
    finish(builder, root);
    const std::uint8_t* data = builder.GetBufferPointer();
    const std::size_t size = builder.GetSize();
    std::vector<std::byte> out(size);
    std::memcpy(out.data(), data, size);
    return out;
}

[[nodiscard]] std::vector<std::uint8_t> to_branch_bytes(BranchId branch) {
    return {reinterpret_cast<const std::uint8_t*>(branch.bytes.data()),
            reinterpret_cast<const std::uint8_t*>(branch.bytes.data()) + branch.bytes.size()};
}

[[nodiscard]] bool from_branch_bytes(const flatbuffers::Vector<std::uint8_t>* bytes,
                                     BranchId& branch) {
    if (bytes == nullptr || bytes->size() != branch.bytes.size()) {
        return false;
    }
    std::memcpy(branch.bytes.data(), bytes->data(), branch.bytes.size());
    return true;
}

} // namespace

std::uint64_t authoritative_checkpoint_hash(
    const std::vector<CheckpointBlock>& blocks) noexcept {
    constexpr std::uint64_t offset = 14695981039346656037ULL;
    constexpr std::uint64_t prime = 1099511628211ULL;
    std::uint64_t hash = offset;
    const auto mix = [&hash](std::uint8_t byte) { hash = (hash ^ byte) * prime; };
    for (const auto& block : blocks) {
        for (const char byte : block.schema.provider_id) mix(static_cast<std::uint8_t>(byte));
        for (unsigned shift = 0; shift < 32; shift += 8)
            mix(static_cast<std::uint8_t>(block.schema.schema_version >> shift));
        for (const std::byte byte : block.payload) mix(static_cast<std::uint8_t>(byte));
    }
    return hash;
}

std::vector<std::byte> encode_world_checkpoint(geoworld::foundation::WorldId world,
                                               BranchId branch, const CheckpointAnchor& anchor,
                                               std::uint64_t checkpoint_content_hash,
                                               const std::vector<CheckpointBlock>& blocks) {
    flatbuffers::FlatBufferBuilder builder;
    std::vector<flatbuffers::Offset<fbs::ProviderBlock>> block_offsets;
    block_offsets.reserve(blocks.size());
    for (const CheckpointBlock& block : blocks) {
        const auto provider_id = builder.CreateString(block.schema.provider_id);
        const auto payload = builder.CreateVector(
            reinterpret_cast<const std::uint8_t*>(block.payload.data()), block.payload.size());
        const std::uint32_t payload_crc = crc32c::Crc32c(
            reinterpret_cast<const std::uint8_t*>(block.payload.data()), block.payload.size());
        block_offsets.push_back(fbs::CreateProviderBlock(
            builder, provider_id, block.schema.schema_version, payload, payload_crc));
    }
    const auto block_vector = builder.CreateVector(block_offsets);
    const auto branch_bytes = builder.CreateVector(to_branch_bytes(branch));
    const auto root = fbs::CreateWorldCheckpoint(
        builder, 1, world.value, branch_bytes, anchor.completed_tick, anchor.resume_tick,
        anchor.included_lsn.value, anchor.world_state_hash, block_vector,
        checkpoint_content_hash);
    return finish_to_bytes(builder, root, &fbs::FinishWorldCheckpointBuffer);
}

Result<DecodedCheckpointData> decode_world_checkpoint(std::span<const std::byte> bytes) {
    if (bytes.empty() || bytes.size() > kMaxCheckpointBytes) {
        return {{}, PersistenceError::checkpoint_invalid};
    }
    flatbuffers::Verifier verifier(reinterpret_cast<const std::uint8_t*>(bytes.data()),
                                   bytes.size());
    if (!fbs::VerifyWorldCheckpointBuffer(verifier)) {
        return {{}, PersistenceError::checkpoint_invalid};
    }
    const auto* checkpoint = fbs::GetWorldCheckpoint(bytes.data());
    Result<DecodedCheckpointData> result;
    if (checkpoint->format_version() != 1
        || !from_branch_bytes(checkpoint->branch_id(), result.value.branch)) {
        return {{}, PersistenceError::checkpoint_invalid};
    }
    result.value.world = geoworld::foundation::WorldId{checkpoint->world_id()};
    result.value.anchor.completed_tick = checkpoint->completed_tick();
    result.value.anchor.resume_tick = checkpoint->resume_tick();
    result.value.anchor.included_lsn = Lsn{checkpoint->included_lsn()};
    result.value.anchor.world_state_hash = checkpoint->world_state_hash();
    result.value.checkpoint_content_hash = checkpoint->checkpoint_content_hash();
    if (checkpoint->blocks() == nullptr || checkpoint->blocks()->size() > kMaxProviderBlocks) {
        return {{}, PersistenceError::checkpoint_invalid};
    }
    std::string previous_id;
    for (const auto* block : *checkpoint->blocks()) {
        if (block == nullptr || block->provider_id() == nullptr || block->payload() == nullptr) {
            return {{}, PersistenceError::checkpoint_invalid};
        }
        DecodedBlock decoded;
        decoded.schema = CheckpointSchema{block->provider_id()->str(), block->schema_version()};
        if (decoded.schema.provider_id.empty() || decoded.schema.schema_version == 0
            || (!previous_id.empty() && decoded.schema.provider_id <= previous_id)) {
            return {{}, PersistenceError::checkpoint_invalid};
        }
        previous_id = decoded.schema.provider_id;
        decoded.payload.resize(block->payload()->size());
        std::memcpy(decoded.payload.data(), block->payload()->data(), block->payload()->size());
        const std::uint32_t actual_crc = crc32c::Crc32c(
            reinterpret_cast<const std::uint8_t*>(decoded.payload.data()),
            decoded.payload.size());
        if (actual_crc != block->payload_crc32c()) {
            return {{}, PersistenceError::checksum_mismatch};
        }
        result.value.blocks.push_back(std::move(decoded));
    }
    return result;
}

std::vector<std::byte> encode_checkpoint_manifest(
    geoworld::foundation::WorldId world, BranchId branch, const CheckpointAnchor& anchor,
    std::uint64_t checkpoint_content_hash,
    const std::vector<CheckpointSchema>& provider_schemas, const std::string& data_file,
    std::uint64_t data_length, std::uint32_t data_crc32c,
    std::uint64_t data_uncompressed_length, bool zstd_compressed) {
    flatbuffers::FlatBufferBuilder builder;
    std::vector<flatbuffers::Offset<fbs::ProviderSchemaEntry>> schema_offsets;
    schema_offsets.reserve(provider_schemas.size());
    for (const CheckpointSchema& schema : provider_schemas) {
        schema_offsets.push_back(fbs::CreateProviderSchemaEntry(
            builder, builder.CreateString(schema.provider_id), schema.schema_version));
    }
    const auto root = fbs::CreateCheckpointManifest(
        builder, 1, world.value, builder.CreateVector(to_branch_bytes(branch)),
        anchor.completed_tick, anchor.resume_tick, anchor.included_lsn.value,
        anchor.world_state_hash, builder.CreateVector(schema_offsets),
        builder.CreateVector(std::vector<flatbuffers::Offset<fbs::RandomStreamEntry>>{}),
        builder.CreateVector(std::vector<flatbuffers::Offset<fbs::ArtifactEntry>>{}),
        builder.CreateString(data_file), data_length, data_crc32c, data_uncompressed_length,
        zstd_compressed ? fbs::Compression::zstd : fbs::Compression::none,
        checkpoint_content_hash);
    return finish_to_bytes(builder, root, &fbs::FinishCheckpointManifestBuffer);
}

Result<DecodedManifest> decode_checkpoint_manifest(std::span<const std::byte> bytes) {
    if (bytes.empty() || bytes.size() > kMaxCheckpointBytes) {
        return {{}, PersistenceError::manifest_invalid};
    }
    flatbuffers::Verifier verifier(reinterpret_cast<const std::uint8_t*>(bytes.data()),
                                   bytes.size());
    if (!fbs::VerifyCheckpointManifestBuffer(verifier)) {
        return {{}, PersistenceError::manifest_invalid};
    }
    const auto* manifest = fbs::GetCheckpointManifest(bytes.data());
    Result<DecodedManifest> result;
    if (manifest->format_version() != 1
        || !from_branch_bytes(manifest->branch_id(), result.value.branch)
        || manifest->data_file() == nullptr) {
        return {{}, PersistenceError::manifest_invalid};
    }
    result.value.world = geoworld::foundation::WorldId{manifest->world_id()};
    result.value.anchor.completed_tick = manifest->completed_tick();
    result.value.anchor.resume_tick = manifest->resume_tick();
    result.value.anchor.included_lsn = Lsn{manifest->included_lsn()};
    result.value.anchor.world_state_hash = manifest->world_state_hash();
    result.value.checkpoint_content_hash = manifest->checkpoint_content_hash();
    std::string previous_id;
    if (manifest->provider_schemas() != nullptr) {
        if (manifest->provider_schemas()->size() > kMaxProviderBlocks) {
            return {{}, PersistenceError::manifest_invalid};
        }
        for (const auto* entry : *manifest->provider_schemas()) {
            if (entry == nullptr || entry->provider_id() == nullptr) {
                return {{}, PersistenceError::manifest_invalid};
            }
            CheckpointSchema schema{entry->provider_id()->str(), entry->schema_version()};
            if (schema.provider_id.empty() || schema.schema_version == 0
                || (!previous_id.empty() && schema.provider_id <= previous_id)) {
                return {{}, PersistenceError::manifest_invalid};
            }
            previous_id = schema.provider_id;
            result.value.provider_schemas.push_back(std::move(schema));
        }
    }
    result.value.data_file = manifest->data_file()->str();
    const std::filesystem::path data_file{result.value.data_file};
    if (result.value.data_file.empty() || data_file.has_parent_path()
        || data_file.filename() != data_file) {
        return {{}, PersistenceError::manifest_invalid};
    }
    result.value.data_length = manifest->data_length();
    result.value.data_crc32c = manifest->data_crc32c();
    result.value.data_uncompressed_length = manifest->data_uncompressed_length();
    if (manifest->compression() != fbs::Compression::none
        && manifest->compression() != fbs::Compression::zstd) {
        return {{}, PersistenceError::manifest_invalid};
    }
    result.value.zstd_compressed = manifest->compression() == fbs::Compression::zstd;
    if (result.value.data_uncompressed_length == 0) {
        return {{}, PersistenceError::manifest_invalid};
    }
    return result;
}

} // namespace geoworld::persistence::detail

namespace geoworld::persistence::detail {

Result<VerifiedCheckpoint> verify_checkpoint_pair(FileOps& ops,
                                                  const std::filesystem::path& checkpoint_dir,
                                                  const std::filesystem::path& manifest_path,
                                                  geoworld::foundation::WorldId world,
                                                  BranchId branch) {
    auto manifest_read = ops.read_file(manifest_path);
    if (!manifest_read.ok()) {
        return {{}, manifest_read.error};
    }
    auto manifest = decode_checkpoint_manifest(manifest_read.value);
    if (!manifest.ok()) {
        return {{}, manifest.error};
    }
    if (manifest.value.world != world || manifest.value.branch != branch) {
        return {{}, PersistenceError::config_invalid};
    }
    const std::filesystem::path data_path = checkpoint_dir / manifest.value.data_file;
    auto data_read = ops.read_file(data_path);
    if (!data_read.ok()) {
        return {{}, data_read.error};
    }
    if (data_read.value.size() != manifest.value.data_length) {
        return {{}, PersistenceError::checkpoint_invalid};
    }
    const std::uint32_t data_crc = crc32c::Crc32c(
        reinterpret_cast<const std::uint8_t*>(data_read.value.data()), data_read.value.size());
    if (data_crc != manifest.value.data_crc32c) {
        return {{}, PersistenceError::checksum_mismatch};
    }
    std::vector<std::byte> uncompressed;
    std::span<const std::byte> checkpoint_bytes = data_read.value;
    if (manifest.value.zstd_compressed) {
        if (manifest.value.data_uncompressed_length > kMaxCheckpointBytes) {
            return {{}, PersistenceError::checkpoint_invalid};
        }
        uncompressed.resize(manifest.value.data_uncompressed_length);
        const std::size_t decompressed = ZSTD_decompress(
            uncompressed.data(), uncompressed.size(), data_read.value.data(),
            data_read.value.size());
        if (ZSTD_isError(decompressed) || decompressed != uncompressed.size()) {
            return {{}, PersistenceError::checkpoint_invalid};
        }
        checkpoint_bytes = uncompressed;
    } else if (manifest.value.data_uncompressed_length != data_read.value.size()) {
        return {{}, PersistenceError::checkpoint_invalid};
    }
    auto data = decode_world_checkpoint(checkpoint_bytes);
    if (!data.ok()) {
        return {{}, data.error};
    }
    const CheckpointAnchor& manifest_anchor = manifest.value.anchor;
    const CheckpointAnchor& data_anchor = data.value.anchor;
    if (data.value.world != world || data.value.branch != branch
        || data_anchor.completed_tick != manifest_anchor.completed_tick
        || data_anchor.resume_tick != manifest_anchor.resume_tick
        || data_anchor.included_lsn != manifest_anchor.included_lsn
        || data_anchor.world_state_hash != manifest_anchor.world_state_hash
        || data.value.checkpoint_content_hash != manifest.value.checkpoint_content_hash) {
        return {{}, PersistenceError::checkpoint_invalid};
    }
    if (data.value.blocks.size() != manifest.value.provider_schemas.size()) {
        return {{}, PersistenceError::checkpoint_invalid};
    }
    for (std::size_t index = 0; index < data.value.blocks.size(); ++index) {
        if (data.value.blocks[index].schema != manifest.value.provider_schemas[index]) {
            return {{}, PersistenceError::checkpoint_invalid};
        }
    }
    std::vector<CheckpointBlock> hash_blocks;
    hash_blocks.reserve(data.value.blocks.size());
    for (const auto& block : data.value.blocks) {
        hash_blocks.push_back({block.schema, block.payload});
    }
    if (authoritative_checkpoint_hash(hash_blocks)
        != manifest.value.checkpoint_content_hash) {
        return {{}, PersistenceError::checkpoint_invalid};
    }
    return {VerifiedCheckpoint{std::move(manifest.value), std::move(data.value)},
            PersistenceError::none};
}

} // namespace geoworld::persistence::detail
