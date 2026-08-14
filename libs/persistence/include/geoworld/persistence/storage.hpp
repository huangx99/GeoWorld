#pragma once

#include "geoworld/foundation/ids.hpp"
#include "geoworld/persistence/types.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

namespace geoworld::persistence {

// 可写文件窄接口：供 WAL writer 与原子发布使用，也是故障注入点。
// 实现不得抛出异常，全部经 PersistenceError 上报。
class WritableFile {
public:
    virtual ~WritableFile() = default;

    [[nodiscard]] virtual PersistenceError write(std::span<const std::byte> data) noexcept = 0;
    // 语义为 fdatasync：数据落盘成功才允许完成 durable 承诺。
    [[nodiscard]] virtual PersistenceError sync() noexcept = 0;
    [[nodiscard]] virtual std::uint64_t size() const noexcept = 0;
};

// 存储后端窄接口：默认 POSIX 本地文件系统实现；测试可注入失败与延迟。
class FileOps {
public:
    virtual ~FileOps() = default;

    virtual Result<std::unique_ptr<WritableFile>> open_append(
        const std::filesystem::path& path) = 0;
    // 独占创建：目标已存在时返回 io_failure，用于防止覆盖既有 segment。
    virtual Result<std::unique_ptr<WritableFile>> create_exclusive(
        const std::filesystem::path& path) = 0;
    [[nodiscard]] virtual PersistenceError rename_file(
        const std::filesystem::path& from, const std::filesystem::path& to) = 0;
    [[nodiscard]] virtual PersistenceError sync_directory(const std::filesystem::path& dir) = 0;
    [[nodiscard]] virtual PersistenceError truncate_file(
        const std::filesystem::path& path, std::uint64_t size) = 0;
    virtual Result<std::vector<std::byte>> read_file(const std::filesystem::path& path) = 0;
    virtual Result<std::vector<std::filesystem::path>> list_files(
        const std::filesystem::path& dir) = 0;
    virtual Result<std::vector<std::filesystem::path>> list_directories(
        const std::filesystem::path& dir) = 0;
    [[nodiscard]] virtual PersistenceError remove_file(const std::filesystem::path& path) = 0;
    [[nodiscard]] virtual PersistenceError create_directories(const std::filesystem::path& dir) = 0;
};

[[nodiscard]] std::shared_ptr<FileOps> make_posix_file_ops();

// 原子发布顺序冻结：同目录临时文件 -> 写入并 sync -> rename -> 父目录 sync。
// 任何一步失败都不得让临时文件成为恢复候选；调用方负责清理策略。
[[nodiscard]] PersistenceError atomic_publish(FileOps& ops,
                                              const std::filesystem::path& final_path,
                                              std::span<const std::byte> contents);

// 目录布局冻结：<durable_root>/world-<id>/branch-<32hex>/ 下分 wal/manifests/checkpoints。
struct DurableLayout {
    std::filesystem::path root;

    [[nodiscard]] std::filesystem::path wal_dir() const { return root / "wal"; }
    [[nodiscard]] std::filesystem::path manifest_dir() const { return root / "manifests"; }
    [[nodiscard]] std::filesystem::path checkpoint_dir() const { return root / "checkpoints"; }
    [[nodiscard]] std::filesystem::path directory_manifest_path() const;
};

[[nodiscard]] DurableLayout make_durable_layout(const std::filesystem::path& durable_root,
                                                geoworld::foundation::WorldId world,
                                                BranchId branch);

} // namespace geoworld::persistence
