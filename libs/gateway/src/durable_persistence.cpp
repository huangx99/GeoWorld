#include "geoworld/gateway/durable_persistence.hpp"

#include <chrono>
#include <utility>

namespace geoworld::gateway {
namespace {

// 记录种类数值一致性冻结：gateway 窄接口与 persistence WAL 共享编码空间。
static_assert(static_cast<std::uint16_t>(persistence::WalRecordKind::external_command)
              == static_cast<std::uint8_t>(DurableRecordKind::external_command));
static_assert(static_cast<std::uint16_t>(persistence::WalRecordKind::command_outcome)
              == static_cast<std::uint8_t>(DurableRecordKind::command_outcome));

class PersistenceTicket final : public DurableTicket {
public:
    explicit PersistenceTicket(persistence::AppendTicket ticket)
        : ticket_(std::move(ticket)) {}

    [[nodiscard]] bool try_outcome(DurableAppendOutcome& outcome) override {
        const std::optional<persistence::AppendOutcome> result =
            ticket_.wait_for(std::chrono::milliseconds{0});
        if (!result.has_value()) {
            return false;
        }
        outcome.lsn = result->lsn.value;
        outcome.ok = result->ok();
        return true;
    }

private:
    persistence::AppendTicket ticket_;
};

class PersistenceAdmissionLog final : public DurableAdmissionLog {
public:
    explicit PersistenceAdmissionLog(std::shared_ptr<persistence::DurableLog> log)
        : log_(std::move(log)) {}

    [[nodiscard]] std::unique_ptr<DurableTicket> append(
        const DurableRecord& record) override {
        persistence::WalRecord wal_record;
        wal_record.kind = static_cast<persistence::WalRecordKind>(record.kind);
        wal_record.target_tick = record.target_tick;
        wal_record.payload = record.payload;
        // durable admission 只承诺 durable；relaxed 不属于该链路。
        wal_record.durability = persistence::Durability::durable;
        persistence::Result<persistence::AppendTicket> result =
            log_->append(std::move(wal_record));
        if (!result.ok()) {
            return nullptr;
        }
        return std::make_unique<PersistenceTicket>(std::move(result.value));
    }

private:
    std::shared_ptr<persistence::DurableLog> log_;
};

} // namespace

[[nodiscard]] std::shared_ptr<DurableAdmissionLog> make_persistence_admission_log(
    std::shared_ptr<persistence::DurableLog> log) {
    return std::make_shared<PersistenceAdmissionLog>(std::move(log));
}

[[nodiscard]] bool restore_durable_index(GatewayCore& core,
                                         const persistence::WalScanResult& scan) {
    for (const persistence::ScannedRecord& record : scan.records) {
        DurableRecordKind kind;
        if (record.kind == persistence::WalRecordKind::external_command) {
            kind = DurableRecordKind::external_command;
        } else if (record.kind == persistence::WalRecordKind::command_outcome) {
            kind = DurableRecordKind::command_outcome;
        } else {
            continue;
        }
        if (!core.restore_durable_record(kind, record.lsn.value, record.payload)) {
            return false;
        }
    }
    return true;
}

} // namespace geoworld::gateway
