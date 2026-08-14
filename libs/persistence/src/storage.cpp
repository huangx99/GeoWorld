#include "geoworld/persistence/storage.hpp"

#include "geoworld/persistence/wal.hpp"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <string>
#include <system_error>
#include <unistd.h>

namespace geoworld::persistence {

namespace {

[[nodiscard]] PersistenceError map_errno(int error_number) noexcept {
    if (error_number == ENOSPC || error_number == EACCES || error_number == EPERM
        || error_number == EDQUOT) {
        return PersistenceError::no_space_or_permission;
    }
    return PersistenceError::io_failure;
}

class PosixWritableFile final : public WritableFile {
public:
    explicit PosixWritableFile(int fd, std::uint64_t size) : fd_(fd), size_(size) {}

    ~PosixWritableFile() override {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    [[nodiscard]] PersistenceError write(std::span<const std::byte> data) noexcept override {
        std::size_t written = 0;
        while (written < data.size()) {
            const ssize_t count =
                ::write(fd_, data.data() + written, data.size() - written);
            if (count < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return map_errno(errno);
            }
            written += static_cast<std::size_t>(count);
        }
        size_ += data.size();
        return PersistenceError::none;
    }

    [[nodiscard]] PersistenceError sync() noexcept override {
        while (::fdatasync(fd_) != 0) {
            if (errno == EINTR) {
                continue;
            }
            const int error_number = errno;
            const PersistenceError mapped = map_errno(error_number);
            return mapped == PersistenceError::io_failure ? PersistenceError::sync_failed
                                                          : mapped;
        }
        return PersistenceError::none;
    }

    [[nodiscard]] std::uint64_t size() const noexcept override { return size_; }

private:
    int fd_{-1};
    std::uint64_t size_{};
};

class PosixFileOps final : public FileOps {
public:
    Result<std::unique_ptr<WritableFile>> open_append(
        const std::filesystem::path& path) override {
        const int fd = ::open(path.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC);
        if (fd < 0) {
            return {nullptr, map_errno(errno)};
        }
        const off_t end = ::lseek(fd, 0, SEEK_END);
        if (end < 0) {
            const int error_number = errno;
            ::close(fd);
            return {nullptr, map_errno(error_number)};
        }
        return {std::make_unique<PosixWritableFile>(fd, static_cast<std::uint64_t>(end)),
                PersistenceError::none};
    }

    Result<std::unique_ptr<WritableFile>> create_exclusive(
        const std::filesystem::path& path) override {
        const int fd =
            ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, kFileMode);
        if (fd < 0) {
            return {nullptr, map_errno(errno)};
        }
        return {std::make_unique<PosixWritableFile>(fd, 0), PersistenceError::none};
    }

    [[nodiscard]] PersistenceError rename_file(
        const std::filesystem::path& from, const std::filesystem::path& to) override {
        if (::rename(from.c_str(), to.c_str()) != 0) {
            return map_errno(errno);
        }
        return PersistenceError::none;
    }

    [[nodiscard]] PersistenceError sync_directory(const std::filesystem::path& dir) override {
        const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd < 0) {
            return map_errno(errno);
        }
        PersistenceError result = PersistenceError::none;
        while (::fsync(fd) != 0) {
            if (errno == EINTR) {
                continue;
            }
            result = PersistenceError::sync_failed;
            break;
        }
        ::close(fd);
        return result;
    }

    [[nodiscard]] PersistenceError truncate_file(const std::filesystem::path& path,
                                                 std::uint64_t size) override {
        if (::truncate(path.c_str(), static_cast<off_t>(size)) != 0) {
            return map_errno(errno);
        }
        return PersistenceError::none;
    }

    Result<std::vector<std::byte>> read_file(const std::filesystem::path& path) override {
        const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            const int error_number = errno;
            if (error_number == ENOENT || error_number == ENOTDIR) {
                return {{}, PersistenceError::not_found};
            }
            return {{}, map_errno(error_number)};
        }
        const off_t end = ::lseek(fd, 0, SEEK_END);
        if (end < 0 || ::lseek(fd, 0, SEEK_SET) < 0) {
            const int error_number = errno;
            ::close(fd);
            return {{}, map_errno(error_number)};
        }
        std::vector<std::byte> contents(static_cast<std::size_t>(end));
        std::size_t read_total = 0;
        while (read_total < contents.size()) {
            const ssize_t count = ::read(fd, contents.data() + read_total,
                                         contents.size() - read_total);
            if (count < 0) {
                if (errno == EINTR) {
                    continue;
                }
                const int error_number = errno;
                ::close(fd);
                return {{}, map_errno(error_number)};
            }
            if (count == 0) {
                break;
            }
            read_total += static_cast<std::size_t>(count);
        }
        ::close(fd);
        contents.resize(read_total);
        return {std::move(contents), PersistenceError::none};
    }

    Result<std::vector<std::filesystem::path>> list_files(
        const std::filesystem::path& dir) override {
        std::error_code ec;
        std::vector<std::filesystem::path> entries;
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (entry.is_regular_file(ec)) {
                entries.push_back(entry.path());
            }
        }
        if (ec) {
            if (ec == std::errc::no_such_file_or_directory) {
                return {{}, PersistenceError::not_found};
            }
            return {{}, PersistenceError::io_failure};
        }
        return {std::move(entries), PersistenceError::none};
    }

    Result<std::vector<std::filesystem::path>> list_directories(
        const std::filesystem::path& dir) override {
        std::error_code ec;
        std::vector<std::filesystem::path> entries;
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (entry.is_directory(ec)) {
                entries.push_back(entry.path());
            }
        }
        if (ec) {
            if (ec == std::errc::no_such_file_or_directory) {
                return {{}, PersistenceError::not_found};
            }
            return {{}, PersistenceError::io_failure};
        }
        return {std::move(entries), PersistenceError::none};
    }

    [[nodiscard]] PersistenceError remove_file(const std::filesystem::path& path) override {
        if (::unlink(path.c_str()) != 0 && errno != ENOENT) {
            return map_errno(errno);
        }
        return PersistenceError::none;
    }

    [[nodiscard]] PersistenceError create_directories(
        const std::filesystem::path& dir) override {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec) {
            return map_errno(ec.value());
        }
        return PersistenceError::none;
    }

private:
    static constexpr mode_t kFileMode = 0644;
};

// 同一进程内临时文件名必须唯一，避免并发发布互相覆盖。
std::atomic<std::uint64_t> g_temp_counter{};

} // namespace

std::shared_ptr<FileOps> make_posix_file_ops() {
    return std::make_shared<PosixFileOps>();
}

PersistenceError atomic_publish(FileOps& ops, const std::filesystem::path& final_path,
                                std::span<const std::byte> contents) {
    const std::filesystem::path parent = final_path.parent_path();
    const std::string temp_name = std::string{kTempFilePrefix} + final_path.filename().string()
                                  + "-" + std::to_string(++g_temp_counter);
    const std::filesystem::path temp_path = parent / temp_name;
    auto created = ops.create_exclusive(temp_path);
    if (!created.ok()) {
        return created.error;
    }
    PersistenceError error = created.value->write(contents);
    if (error == PersistenceError::none) {
        error = created.value->sync();
    }
    created.value.reset();
    if (error == PersistenceError::none) {
        error = ops.rename_file(temp_path, final_path);
    }
    if (error == PersistenceError::none) {
        error = ops.sync_directory(parent);
    }
    if (error != PersistenceError::none) {
        // 失败路径尽力清理：tmp- 前缀文件永远不会被当成恢复候选。
        ops.remove_file(temp_path);
    }
    return error;
}

std::filesystem::path DurableLayout::directory_manifest_path() const {
    return manifest_dir() / "directory.gwmanifest";
}

DurableLayout make_durable_layout(const std::filesystem::path& durable_root,
                                  geoworld::foundation::WorldId world, BranchId branch) {
    std::string branch_text;
    branch_text.reserve(branch.bytes.size() * 2);
    constexpr char hex_digits[] = "0123456789abcdef";
    for (const std::uint8_t byte : branch.bytes) {
        branch_text.push_back(hex_digits[(byte >> 4U) & 0x0FU]);
        branch_text.push_back(hex_digits[byte & 0x0FU]);
    }
    return {durable_root / ("world-" + std::to_string(world.value))
            / ("branch-" + branch_text)};
}

} // namespace geoworld::persistence
