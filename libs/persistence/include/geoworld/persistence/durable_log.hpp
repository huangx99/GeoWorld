#pragma once

#include "geoworld/persistence/types.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace geoworld::persistence {

// WAL 记录类型，与 schemas/persistence/wal_record.fbs 的 WalRecordKind 保持一致。
enum class WalRecordKind : std::uint16_t {
    external_command = 0,
    normalized_input = 1,
    version_switch = 2,
    random_control = 3,
    command_outcome = 4,
    state_hash = 5,
    checkpoint_marker = 6,
};

// relaxed 只允许基准与显式测试配置；响应状态必须与 durable 区分。
enum class Durability : std::uint8_t {
    durable,
    relaxed,
};

// writer 只接收自有、不可变的规范化记录：不持有世界对象、迭代器或原始指针。
struct WalRecord {
    WalRecordKind kind{WalRecordKind::external_command};
    std::uint64_t target_tick{};
    std::vector<std::byte> payload;
    Durability durability{Durability::durable};
};

enum class AdmissionStatus : std::uint8_t {
    none,
    durable_accepted,
    relaxed_accepted,
};

struct AppendOutcome {
    PersistenceError error{PersistenceError::none};
    Lsn lsn{};
    AdmissionStatus status{AdmissionStatus::none};

    [[nodiscard]] bool ok() const noexcept { return error == PersistenceError::none; }
};

// 一次 append 的异步完成句柄；可复制，多处等待得到同一结果。
class AppendTicket {
public:
    // 实现细节的前置声明：仅 persistence 内部（writer 完成 promise）使用。
    struct State;

    AppendTicket() = default;

    [[nodiscard]] AppendOutcome wait() const;
    [[nodiscard]] std::optional<AppendOutcome> wait_for(std::chrono::milliseconds timeout) const;
    [[nodiscard]] bool pending() const;

private:
    explicit AppendTicket(std::shared_ptr<State> state) : state_(std::move(state)) {}

    std::shared_ptr<State> state_;

    friend class WalWriter;
};

// durable append 抽象：Gateway durable admission 只依赖该接口，不接触 WAL 文件。
class DurableLog {
public:
    virtual ~DurableLog() = default;

    // LSN 由 writer 唯一分配；队列满、故障态、关闭中均以稳定错误码立即拒绝。
    [[nodiscard]] virtual Result<AppendTicket> append(WalRecord record) = 0;
    [[nodiscard]] virtual Lsn last_durable_lsn() const noexcept = 0;
    [[nodiscard]] virtual bool faulted() const noexcept = 0;
    // 取消全部异步操作并 flush 当前承诺边界；返回后所有已接受 ticket 均有终态。
    virtual void shutdown() = 0;
};

} // namespace geoworld::persistence
