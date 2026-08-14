// M5 durable admission 集成测试（进程内组合，与 geoworldd 相同的组合方式）：
// (a) 同键同内容跨模拟重启返回既有结果
// (b) 同键不同内容稳定拒绝 GWG205
// (c) durable accepted 后丢内存状态，scan 重建索引可查且记录完整
// (d) durable 接纳不可用映射 GWG206
// (e) 无 request_id 的 M4 路径回归
// (f) submitter 失败与命令序列回绕拒绝
// (g) ingress 序列回绕拒绝
#include "geoworld/gateway/auth.hpp"
#include "geoworld/gateway/durable.hpp"
#include "geoworld/gateway/durable_persistence.hpp"
#include "geoworld/gateway/errors.hpp"
#include "geoworld/gateway/gateway_core.hpp"
#include "geoworld/persistence/storage.hpp"
#include "geoworld/persistence/wal.hpp"
#include "geoworld/projection/engine.hpp"
#include "geoworld/protocol/version.hpp"
#include "geoworld/runtime/world_runtime.hpp"
#include "geoworld/simulation/command_buffer.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <unistd.h>

namespace {

using geoworld::foundation::WorldId;
using geoworld::gateway::CommandReceipt;
using geoworld::gateway::DurableAdmissionLog;
using geoworld::gateway::DurableAppendOutcome;
using geoworld::gateway::DurableRecord;
using geoworld::gateway::DurableRecordKind;
using geoworld::gateway::DurableRequestId;
using geoworld::gateway::DurableTicket;
using geoworld::gateway::ExternalCommand;
using geoworld::gateway::GatewayCore;
using geoworld::gateway::GatewayError;
using geoworld::gateway::ReceiptStatus;
using geoworld::gateway::SessionId;
using geoworld::gateway::SetPropertyParams;
using geoworld::persistence::BranchId;
using geoworld::persistence::FileOps;
using geoworld::persistence::WalConfig;
using geoworld::persistence::WalRecordKind;
using geoworld::persistence::WalScanResult;
using geoworld::persistence::WalWriter;

const geoworld::spatial::Geodetic kOriginGeodetic{31.0, 121.0, 0.0};
constexpr std::uint64_t kSeedWid = 7;
constexpr std::uint64_t kDurableWorldId = 1;
constexpr std::string_view kDurableBranchText = "00000000-0000-0000-0000-000000000001";
constexpr std::string_view kAdminToken = "admin-token";
constexpr std::string_view kWritableProperty = "speed";
constexpr std::uint64_t kOwnershipLeaseTicks = 100'000;
constexpr std::uint64_t kClientSequence = 1;
constexpr std::uint8_t kRequestIdFill = 0x42;
constexpr double kUpdatedSpeed = 9.5;
constexpr double kConflictingSpeed = 3.0;
constexpr auto kDriveDeadline = std::chrono::seconds{15};
constexpr auto kDriveSleep = std::chrono::milliseconds{1};

std::atomic<int> g_dir_counter{0};

struct TempDir {
    std::filesystem::path path;

    explicit TempDir(std::string_view name) {
        path = std::filesystem::temp_directory_path()
               / ("gw-m5b-" + std::string{name} + "-" + std::to_string(::getpid()) + "-"
                  + std::to_string(++g_dir_counter));
        std::filesystem::create_directories(path);
    }

    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

// 测试用立即完成的 fake 接纳日志：append 可配置为拒绝（nullptr）或
// 返回首张 try_outcome 即完成的票据，免 WAL 驱动异步链路。
class FakeTicket final : public DurableTicket {
public:
    explicit FakeTicket(std::uint64_t lsn) : lsn_(lsn) {}

    [[nodiscard]] bool try_outcome(DurableAppendOutcome& outcome) override {
        outcome.lsn = lsn_;
        outcome.ok = true;
        return true;
    }

private:
    std::uint64_t lsn_;
};

class FakeAdmissionLog final : public DurableAdmissionLog {
public:
    explicit FakeAdmissionLog(bool accepting) : accepting_(accepting) {}

    [[nodiscard]] std::unique_ptr<DurableTicket> append(
        const DurableRecord& record) override {
        if (!accepting_) {
            return nullptr;
        }
        last_appended_kind_ = record.kind;
        return std::make_unique<FakeTicket>(next_lsn_++);
    }

    [[nodiscard]] std::optional<DurableRecordKind> last_appended_kind() const {
        return last_appended_kind_;
    }

private:
    bool accepting_;
    std::uint64_t next_lsn_{1};
    std::optional<DurableRecordKind> last_appended_kind_;
};

// 组合根：projection engine + gateway core + world runtime，submit 直投 runtime。
struct Fixture {
    geoworld::projection::ProjectionConfig projection_config;
    geoworld::projection::ProjectionPolicy policy;
    std::unique_ptr<geoworld::projection::ProjectionEngine> engine;
    std::shared_ptr<geoworld::gateway::FixtureAuthentication> authentication;
    std::shared_ptr<geoworld::gateway::FixtureAuthorization> authorization;
    geoworld::runtime::WorldRuntime runtime;
    std::unique_ptr<GatewayCore> core;

    explicit Fixture(geoworld::gateway::GatewayConfig gateway_config = {},
                     bool failing_submitter = false) {
        projection_config.enu_origin = geoworld::spatial::geodetic_to_ecef(kOriginGeodetic);
        policy.allow_property(std::string{kWritableProperty});
        engine = std::make_unique<geoworld::projection::ProjectionEngine>(
            projection_config, policy);
        authentication = std::make_shared<geoworld::gateway::FixtureAuthentication>();
        authentication->add_credential(std::string{kAdminToken}, {"admin", true});
        authorization = std::make_shared<geoworld::gateway::FixtureAuthorization>();
        authorization->allow_writable_property(std::string{kWritableProperty});
        core = std::make_unique<GatewayCore>(
            gateway_config, *engine, authentication, authorization,
            [] { return std::chrono::steady_clock::now(); },
            [counter = 0ULL]() mutable { return "ticket-" + std::to_string(++counter); },
            geoworld::protocol::control_api_version,
            geoworld::protocol::data_schema_version);
        if (failing_submitter) {
            core->set_command_submitter(
                [](std::uint64_t, geoworld::simulation::CommandPayload,
                   geoworld::simulation::CommandMeta) { return 0; });
        } else {
            core->set_command_submitter(
                [this](std::uint64_t target_tick,
                       geoworld::simulation::CommandPayload payload,
                       geoworld::simulation::CommandMeta meta) {
                    return runtime.submit(target_tick, std::move(payload), meta);
                });
        }
    }

    [[nodiscard]] std::uint64_t current_tick() const {
        return static_cast<std::uint64_t>(runtime.clock().tick());
    }

    // 播种活动对象并开管理员会话，获取可写属性所有权。
    [[nodiscard]] SessionId open_admin_session() {
        geoworld::world::WorldObject seed;
        seed.id = WorldId{kSeedWid};
        seed.semantic_type = "geoworld.demo";
        seed.lifecycle = geoworld::world::LifecycleState::active;
        seed.properties.emplace(std::string{kWritableProperty}, 1.0);
        static_cast<void>(runtime.submit(
            0, geoworld::simulation::CreateObjectCommand{seed}));
        static_cast<void>(runtime.step());

        const GatewayCore::OpenSessionResult opened = core->open_session(
            kAdminToken, geoworld::protocol::control_api_version,
            geoworld::protocol::control_api_version,
            geoworld::protocol::data_schema_version,
            geoworld::protocol::data_schema_version, current_tick());
        if (opened.error != GatewayError::none) {
            return SessionId{};
        }
        const SessionId session = opened.session.session.id;
        if (core->acquire_ownership(session, WorldId{kSeedWid},
                                    {std::string{kWritableProperty}},
                                    current_tick() + kOwnershipLeaseTicks,
                                    current_tick())
            != GatewayError::none) {
            return SessionId{};
        }
        return session;
    }
};

[[nodiscard]] DurableRequestId make_request_id(std::uint8_t seed) {
    DurableRequestId id{};
    id.fill(seed);
    return id;
}

[[nodiscard]] ExternalCommand make_speed_command(SessionId session, double value) {
    ExternalCommand command;
    command.session = session;
    command.client_sequence = kClientSequence;
    command.target_wid = WorldId{kSeedWid};
    command.params = SetPropertyParams{std::string{kWritableProperty}, value};
    command.request_id = make_request_id(kRequestIdFill);
    return command;
}

struct DurableResult {
    bool completed{};
    GatewayError error{GatewayError::none};
    CommandReceipt receipt{};
};

// 提交 durable 命令并驱动 poll/step 直到结论达成（立即结论已同步完成）。
[[nodiscard]] DurableResult submit_durable_driven(Fixture& fixture,
                                                  const ExternalCommand& command) {
    DurableResult result;
    fixture.core->submit_durable_command(
        command.session, command, fixture.current_tick(),
        [&result](GatewayError error, const CommandReceipt& receipt) {
            result.completed = true;
            result.error = error;
            result.receipt = receipt;
        });
    const auto deadline = std::chrono::steady_clock::now() + kDriveDeadline;
    while (!result.completed && std::chrono::steady_clock::now() < deadline) {
        fixture.core->poll_durable_tickets();
        // 结论达成即停：命令尚未被后续 step 应用，幂等索引停在 durable_accepted。
        if (result.completed) {
            break;
        }
        // apply 报告必须回喂 on_commands_applied，否则终态回执与幂等索引丢失。
        const geoworld::runtime::StepResult step = fixture.runtime.step();
        fixture.core->on_commands_applied(step.commands);
        std::this_thread::sleep_for(kDriveSleep);
    }
    return result;
}

// 推进 tick 直到指定 client_sequence 的回执到达 applied 终态。
[[nodiscard]] bool drive_until_applied(Fixture& fixture, SessionId session,
                                       std::uint64_t client_sequence) {
    const auto deadline = std::chrono::steady_clock::now() + kDriveDeadline;
    while (std::chrono::steady_clock::now() < deadline) {
        const geoworld::runtime::StepResult step = fixture.runtime.step();
        fixture.core->on_commands_applied(step.commands);
        fixture.core->poll_durable_tickets();
        const std::optional<CommandReceipt> receipt =
            fixture.core->sessions().find_receipt(session, client_sequence);
        if (receipt.has_value() && receipt->status == ReceiptStatus::applied) {
            return true;
        }
        std::this_thread::sleep_for(kDriveSleep);
    }
    return false;
}

[[nodiscard]] WalConfig make_wal_config(const std::filesystem::path& root) {
    WalConfig config;
    config.durable_root = root;
    config.world = WorldId{kDurableWorldId};
    const std::optional<BranchId> branch =
        geoworld::persistence::parse_branch_id(kDurableBranchText);
    config.branch = *branch;
    return config;
}

[[nodiscard]] WalScanResult scan_layout(const WalConfig& config, FileOps& ops) {
    const geoworld::persistence::DurableLayout layout =
        geoworld::persistence::make_durable_layout(config.durable_root, config.world,
                                                   config.branch);
    return geoworld::persistence::scan_wal_directory(layout.wal_dir(), ops);
}

bool g_failed = false;

void check(bool condition, const char* name) {
    if (!condition) {
        std::cerr << "FAIL: " << name << '\n';
        g_failed = true;
    }
}

// (a)/(c)：durable accepted -> 丢内存状态（模拟重启）-> scan 重建 -> 幂等结果可查。
void test_restart_idempotency() {
    TempDir dir{"restart"};
    const WalConfig wal_config = make_wal_config(dir.path);
    const std::shared_ptr<FileOps> ops = geoworld::persistence::make_posix_file_ops();

    std::uint64_t first_lsn = 0;
    {
        Fixture fixture;
        auto writer = std::make_shared<WalWriter>(wal_config, ops);
        check(writer->start().ok(), "a: writer start");
        fixture.core->set_durable_log(
            geoworld::gateway::make_persistence_admission_log(writer));
        const SessionId session = fixture.open_admin_session();
        check(session.valid(), "a: open session");

        const DurableResult result =
            submit_durable_driven(fixture, make_speed_command(session, kUpdatedSpeed));
        check(result.completed, "a: durable completion");
        check(result.error == GatewayError::none, "a: durable accepted error");
        check(result.receipt.status == ReceiptStatus::durable_accepted,
              "a: durable accepted status");
        check(result.receipt.durable_lsn != 0, "a: durable lsn present");
        first_lsn = result.receipt.durable_lsn;

        // 命令尚未应用：同键同内容重试返回 durable accepted（含同一 LSN）。
        const DurableResult early_retry =
            submit_durable_driven(fixture, make_speed_command(session, kUpdatedSpeed));
        check(early_retry.completed && early_retry.error == GatewayError::none,
              "a: early retry ok");
        check(early_retry.receipt.status == ReceiptStatus::durable_accepted,
              "a: early retry durable accepted");
        check(early_retry.receipt.durable_lsn == first_lsn, "a: early retry lsn");

        // 推进到 applied 终态，让 command_outcome 落盘。
        check(drive_until_applied(fixture, session, kClientSequence),
              "c: command applied");
        writer->shutdown();
    }

    // (c)：scan 校验记录完整性（external_command + command_outcome）。
    const WalScanResult scan = scan_layout(wal_config, *ops);
    check(scan.ok(), "c: scan ok");
    bool saw_command = false;
    bool saw_outcome = false;
    for (const auto& record : scan.records) {
        saw_command = saw_command || record.kind == WalRecordKind::external_command;
        saw_outcome = saw_outcome || record.kind == WalRecordKind::command_outcome;
    }
    check(saw_command, "c: external_command record persisted");
    check(saw_outcome, "c: command_outcome record persisted");

    // 模拟重启：全新 Fixture + 新 writer，同目录 scan 重建索引。
    Fixture restarted;
    auto writer = std::make_shared<WalWriter>(wal_config, ops);
    check(writer->start().ok(), "a: restarted writer start");
    check(geoworld::gateway::restore_durable_index(*restarted.core, scan),
          "a: restore index");
    restarted.core->set_durable_log(
        geoworld::gateway::make_persistence_admission_log(writer));
    const SessionId session = restarted.open_admin_session();
    check(session.valid(), "a: restarted open session");

    // (a)：同键同内容 -> 既有终态（applied）直接返回，不重复执行。
    const DurableResult same =
        submit_durable_driven(restarted, make_speed_command(session, kUpdatedSpeed));
    check(same.completed, "a: idempotent completion");
    check(same.error == GatewayError::none, "a: idempotent error");
    check(same.receipt.status == ReceiptStatus::applied, "a: idempotent applied");
    check(same.receipt.durable_lsn == first_lsn, "a: idempotent lsn stable");

    // (b)：同键不同内容 -> GWG205 稳定拒绝（重启后同样成立）。
    const DurableResult conflict =
        submit_durable_driven(restarted, make_speed_command(session, kConflictingSpeed));
    check(conflict.completed, "b: conflict completion");
    check(conflict.error == GatewayError::idempotency_conflict, "b: conflict error");
    check(std::string_view{geoworld::gateway::error_code(conflict.error)} == "GWG205",
          "b: conflict code GWG205");
    writer->shutdown();
}

// (b) 进程内变体：同键不同内容在单次运行内即拒绝。
void test_inprocess_conflict() {
    TempDir dir{"conflict"};
    const WalConfig wal_config = make_wal_config(dir.path);
    const std::shared_ptr<FileOps> ops = geoworld::persistence::make_posix_file_ops();

    Fixture fixture;
    auto writer = std::make_shared<WalWriter>(wal_config, ops);
    check(writer->start().ok(), "b: writer start");
    fixture.core->set_durable_log(
        geoworld::gateway::make_persistence_admission_log(writer));
    const SessionId session = fixture.open_admin_session();
    check(session.valid(), "b: open session");

    const DurableResult first =
        submit_durable_driven(fixture, make_speed_command(session, kUpdatedSpeed));
    check(first.completed && first.error == GatewayError::none, "b: first accepted");

    const DurableResult conflict =
        submit_durable_driven(fixture, make_speed_command(session, kConflictingSpeed));
    check(conflict.completed, "b: in-process conflict completion");
    check(conflict.error == GatewayError::idempotency_conflict,
          "b: in-process conflict error");

    // 同键同内容重试：返回既有 durable accepted（驱动达成即停，命令尚未应用）。
    const DurableResult retry =
        submit_durable_driven(fixture, make_speed_command(session, kUpdatedSpeed));
    check(retry.completed && retry.error == GatewayError::none, "b: retry ok");
    check(retry.receipt.status == ReceiptStatus::durable_accepted, "b: retry status");
    check(retry.receipt.durable_lsn == first.receipt.durable_lsn, "b: retry lsn");
    writer->shutdown();
}

// (d)：未配置 WAL 与 append 立即拒绝都映射 GWG206。
void test_durability_unavailable() {
    {
        Fixture fixture;
        const SessionId session = fixture.open_admin_session();
        check(session.valid(), "d: open session");
        const DurableResult result =
            submit_durable_driven(fixture, make_speed_command(session, kUpdatedSpeed));
        check(result.completed, "d: no-log completion");
        check(result.error == GatewayError::durability_unavailable, "d: no-log error");
        check(std::string_view{geoworld::gateway::error_code(result.error)} == "GWG206",
              "d: no-log code GWG206");
    }
    {
        Fixture fixture;
        fixture.core->set_durable_log(std::make_shared<FakeAdmissionLog>(false));
        const SessionId session = fixture.open_admin_session();
        check(session.valid(), "d: fake open session");
        const DurableResult result =
            submit_durable_driven(fixture, make_speed_command(session, kUpdatedSpeed));
        check(result.completed, "d: rejecting completion");
        check(result.error == GatewayError::durability_unavailable,
              "d: rejecting error GWG206");
    }
}

// (e)：无 request_id 的 M4 进程内去重路径回归。
void test_legacy_path_regression() {
    Fixture fixture;
    const SessionId session = fixture.open_admin_session();
    check(session.valid(), "e: open session");

    ExternalCommand command = make_speed_command(session, kUpdatedSpeed);
    command.request_id.reset();
    const auto [first_error, first] =
        fixture.core->submit_command(session, command, fixture.current_tick());
    check(first_error == GatewayError::none, "e: first error");
    check(first.status == ReceiptStatus::accepted, "e: first accepted");
    check(first.ingress_sequence != 0, "e: first ingress");

    const auto [retry_error, retry] =
        fixture.core->submit_command(session, command, fixture.current_tick());
    check(retry_error == GatewayError::none, "e: retry error");
    check(retry.status == ReceiptStatus::duplicate, "e: retry duplicate");
    check(retry.ingress_sequence == first.ingress_sequence, "e: retry ingress stable");
}

// (f)：命令序列回绕返回 0；submitter 失败在两条链路都转 admission 拒绝。
void test_submitter_failure() {
    {
        geoworld::simulation::CommandBuffer buffer{
            std::numeric_limits<std::uint64_t>::max()};
        geoworld::world::WorldObject object;
        object.id = WorldId{kSeedWid};
        geoworld::simulation::CommandPayload payload =
            geoworld::simulation::DestroyObjectCommand{object.id};
        const std::uint64_t last = buffer.enqueue(0, payload);
        check(last == std::numeric_limits<std::uint64_t>::max(), "f: last sequence");
        check(buffer.enqueue(0, payload) == 0, "f: sequence wrap rejected");
        check(buffer.size() == 1, "f: wrap not enqueued");
    }
    {
        Fixture fixture{{}, true};
        const SessionId session = fixture.open_admin_session();
        check(session.valid(), "f: open session");
        ExternalCommand command = make_speed_command(session, kUpdatedSpeed);
        command.request_id.reset();
        const auto [error, receipt] =
            fixture.core->submit_command(session, command, fixture.current_tick());
        check(error == GatewayError::limit_exceeded, "f: submitter failure error");
        check(receipt.status == ReceiptStatus::rejected, "f: submitter failure rejected");
    }
    {
        // durable 路径：WAL 承诺达成后投递失败 -> 终态拒绝 + 幂等索引记终态。
        Fixture fixture{{}, true};
        auto log = std::make_shared<FakeAdmissionLog>(true);
        fixture.core->set_durable_log(log);
        const SessionId session = fixture.open_admin_session();
        check(session.valid(), "f: durable open session");
        const DurableResult result =
            submit_durable_driven(fixture, make_speed_command(session, kUpdatedSpeed));
        check(result.completed, "f: durable submitter completion");
        check(result.error == GatewayError::limit_exceeded,
              "f: durable submitter error");
        check(result.receipt.status == ReceiptStatus::rejected,
              "f: durable submitter rejected");
        // 幂等重试返回既有终态。
        const DurableResult retry =
            submit_durable_driven(fixture, make_speed_command(session, kUpdatedSpeed));
        check(retry.completed && retry.receipt.status == ReceiptStatus::rejected,
              "f: durable final retry rejected");
        check(retry.error == GatewayError::limit_exceeded, "f: durable final retry error");
    }
}

// (g)：ingress 序列回绕上限 -> 两条链路都明确拒绝 GWG006。
void test_ingress_wrap() {
    geoworld::gateway::GatewayConfig config;
    config.initial_ingress_sequence = std::numeric_limits<std::uint64_t>::max();

    Fixture fixture{config};
    auto log = std::make_shared<FakeAdmissionLog>(true);
    fixture.core->set_durable_log(log);
    const SessionId session = fixture.open_admin_session();
    check(session.valid(), "g: open session");

    ExternalCommand legacy = make_speed_command(session, kUpdatedSpeed);
    legacy.request_id.reset();
    const auto [error, receipt] =
        fixture.core->submit_command(session, legacy, fixture.current_tick());
    check(error == GatewayError::limit_exceeded, "g: legacy wrap error");
    check(receipt.status == ReceiptStatus::rejected, "g: legacy wrap rejected");

    const DurableResult durable =
        submit_durable_driven(fixture, make_speed_command(session, kUpdatedSpeed));
    check(durable.completed, "g: durable wrap completion");
    check(durable.error == GatewayError::limit_exceeded, "g: durable wrap error");
    // 回绕拒绝发生在 WAL append 之前：fake 日志不得收到记录。
    check(!log->last_appended_kind().has_value(), "g: no wal append before wrap reject");
}

} // namespace

int main() {
    test_restart_idempotency();
    test_inprocess_conflict();
    test_durability_unavailable();
    test_legacy_path_regression();
    test_submitter_failure();
    test_ingress_wrap();
    if (g_failed) {
        std::cerr << "m5_durable_test: FAILED\n";
        return 1;
    }
    std::cout << "m5_durable_test: OK\n";
    return 0;
}
