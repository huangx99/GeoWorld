// R4 扇出基准：标准负载与默认值冻结于 docs/M4.md「R4 可复现基准」与「工程验收门」。
// (a) memory 场景：传输替换为内存直写，测量投影 + FlatBuffers 编码 + Gateway 入队的纯扇出成本；
// (b) loopback 场景：真实 Beast WebSocket 数据面（loopback 明文，仅限显式测试配置）。
// 负载实体由基准自有的 world::World 持有，经 World::set_property 在 tick 边界前修改；
// 投影只经 ProjectionEngine::on_projection 稳定边界读取，编码与入队由 GatewayCore::pump 完成。
#include "geoworld/gateway/auth.hpp"
#include "geoworld/foundation/thread_pool.hpp"
#include "geoworld/gateway/config.hpp"
#include "geoworld/gateway/control_server.hpp"
#include "geoworld/gateway/errors.hpp"
#include "geoworld/gateway/frame_codec.hpp"
#include "geoworld/gateway/gateway_core.hpp"
#include "geoworld/gateway/stream_transport.hpp"
#include "geoworld/protocol/codec.hpp"
#include "geoworld/protocol/replica.hpp"
#include "geoworld/protocol/version.hpp"
#include "geoworld/protocol/wire.hpp"
#include "geoworld/projection/config.hpp"
#include "geoworld/projection/connection.hpp"
#include "geoworld/projection/engine.hpp"
#include "geoworld/projection/policy.hpp"
#include "geoworld/simulation/command_buffer.hpp"
#include "geoworld/spatial/coordinates.hpp"
#include "geoworld/world/world.hpp"

#include "stream_client.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <sys/resource.h>
#include <sys/utsname.h>

#ifndef GW_BENCH_BUILD_TYPE
#define GW_BENCH_BUILD_TYPE "unknown"
#endif
#ifndef GW_BENCH_BOOST_VERSION
#define GW_BENCH_BOOST_VERSION "unknown"
#endif
#ifndef GW_BENCH_PROTOBUF_VERSION
#define GW_BENCH_PROTOBUF_VERSION "unknown"
#endif
#ifndef GW_BENCH_GRPC_VERSION
#define GW_BENCH_GRPC_VERSION "unknown"
#endif
#ifndef GW_BENCH_OPENSSL_VERSION
#define GW_BENCH_OPENSSL_VERSION "unknown"
#endif
#ifndef GW_BENCH_FLATBUFFERS_VERSION
#define GW_BENCH_FLATBUFFERS_VERSION "unknown"
#endif

namespace {

using geoworld::foundation::WorldId;
using geoworld::gateway::GatewayCore;
using geoworld::projection::ConnectionId;
using geoworld::projection::ProjectionEngine;

// ---- R4 标准负载默认值（docs/M4.md §10）；全部经 CLI 覆盖 ----
constexpr std::uint64_t kDefaultEntities = 100'000;
constexpr std::uint64_t kDefaultConnections = 256;
constexpr std::uint64_t kDefaultVisiblePerConnection = 2'000;
constexpr std::uint64_t kDefaultTickHz = 20;
constexpr std::uint64_t kDefaultChangePercent = 10;
constexpr std::uint64_t kDefaultKeyframeIntervalSeconds = 5;
constexpr std::uint64_t kDefaultWarmupSeconds = 30;
constexpr std::uint64_t kDefaultSampleSeconds = 600;
constexpr std::uint64_t kDefaultSlowClientPercent = 5;
constexpr std::uint64_t kDefaultSlowSeconds = 30;
constexpr std::uint64_t kDefaultSeed = 20'260'812;
constexpr std::uint64_t kDefaultValidatorConnections = 4;
constexpr std::uint64_t kDefaultCommandRate = 0;
// 命令客户端以 follow 订阅只跟踪自身命令目标：半径只需保证目标可见以维持
// delta/ack 活性（ack 超时 10 s），同时避免给扇出负载测量引入额外流量。
constexpr double kCommandFollowRadiusMeters = 1.0;
// 世界与 AOI 几何基线：10 km 见方均匀分布，实体高度覆盖 z/floor 范围。
constexpr double kWorldExtentMeters = 10'000.0;
constexpr double kEntityMaxHeightMeters = 200.0;
constexpr double kAoiMinUpMeters = -100.0;
constexpr double kAoiMaxUpMeters = 1'000.0;
const geoworld::spatial::Geodetic kOriginGeodetic{31.0, 121.0, 0.0};
constexpr std::string_view kCredentialToken = "m4-bench-observer-token";
constexpr std::string_view kPrincipalId = "m4-bench-viewer";
constexpr std::string_view kSemanticType = "benchmark.entity";
constexpr std::string_view kGeometryRefPrefix = "geometry/benchmark/entity-";
constexpr std::string_view kPropertySpeed = "speed";
constexpr std::string_view kPropertyHeading = "heading";
constexpr std::string_view kStateStatus = "status";
constexpr std::array<std::string_view, 4> kStatusValues{
    "operational", "degraded", "maintenance", "standby"};
constexpr double kSpeedMax = 120.0;
constexpr double kHeadingMax = 360.0;
constexpr std::uint64_t kMicrosecondsPerSecond = 1'000'000;
constexpr std::uint64_t kBytesPerMebibyte = 1'048'576;
constexpr auto kClientIoTimeout = std::chrono::milliseconds{2'000};
constexpr auto kConnectStepTimeout = std::chrono::seconds{30};
constexpr auto kDrainGraceTimeout = std::chrono::seconds{10};
constexpr auto kPollSleep = std::chrono::milliseconds{1};

enum class Scenario { memory, memory_slow, loopback, loopback_slow, all };

struct BenchmarkConfig {
    std::uint64_t entities = kDefaultEntities;
    std::uint64_t connections = kDefaultConnections;
    std::uint64_t visible_per_connection = kDefaultVisiblePerConnection;
    std::uint64_t tick_hz = kDefaultTickHz;
    std::uint64_t change_percent = kDefaultChangePercent;
    std::uint64_t keyframe_interval_seconds = kDefaultKeyframeIntervalSeconds;
    std::uint64_t warmup_seconds = kDefaultWarmupSeconds;
    std::uint64_t sample_seconds = kDefaultSampleSeconds;
    std::uint64_t slow_client_percent = kDefaultSlowClientPercent;
    std::uint64_t slow_seconds = kDefaultSlowSeconds;
    std::uint64_t seed = kDefaultSeed;
    std::uint64_t validators = kDefaultValidatorConnections;
    // 每秒经 gRPC SubmitCommand 提交的 SetProperty 命令数；0 关闭（仅 loopback 生效）。
    std::uint64_t command_rate = kDefaultCommandRate;
    Scenario scenario = Scenario::all;
};

void print_usage() {
    std::cout <<
        "geoworld-m4-benchmark — R4 Gateway 扇出基准（docs/M4.md §10/§11）\n"
        "用法: geoworld-m4-benchmark [选项]\n"
        "  --scenario memory|memory-slow|loopback|loopback-slow|all  (默认 all)\n"
        "  --entities N              权威世界实体数 (默认 100000)\n"
        "  --connections N           虚拟连接数 (默认 256)\n"
        "  --visible-per-connection N 每连接平均可见实体 (默认 2000)\n"
        "  --tick-hz N               fast 状态帧频率 (默认 20)\n"
        "  --change-percent N        每 tick 变化实体百分比 (默认 10)\n"
        "  --keyframe-interval-seconds N (默认 5)\n"
        "  --warmup-seconds N        预热时长 (默认 30)\n"
        "  --sample-seconds N        正式采样时长 (默认 600)\n"
        "  --slow-client-percent N   慢连接占比 (默认 5，仅 *-slow 场景生效)\n"
        "  --slow-seconds N          慢连接停止消费时长 (默认 30)\n"
        "  --seed N                  随机种子 (默认 20260812)\n"
        "  --validators N            解码校验 replica hash 的连接数 (默认 4)\n"
        "  --command-rate N          每秒 gRPC SubmitCommand 命令数 (默认 0=关闭，仅 loopback)\n"
        "  --help\n";
}

[[nodiscard]] bool parse_u64(std::string_view text, std::uint64_t& out) {
    if (text.empty() || text.front() == '-') {
        return false;
    }
    const auto* last = text.data() + text.size();
    const auto result = std::from_chars(text.data(), last, out);
    return result.ec == std::errc{} && result.ptr == last;
}

// 返回 nullptr 表示解析成功；否则返回错误说明。
[[nodiscard]] const char* parse_args(int argc, char** argv, BenchmarkConfig& config) {
    static constexpr std::string_view kUnknown = "未知参数或缺值，见 --help";
    static constexpr std::string_view kNotNumber = "参数值不是非负整数";
    static constexpr std::string_view kBadScenario = "未知 --scenario 取值";
    for (int index = 1; index < argc; ++index) {
        const std::string_view key{argv[index]};
        if (key == "--help") {
            print_usage();
            std::exit(0);
        }
        if (key == "--scenario") {
            if (++index >= argc) {
                return kUnknown.data();
            }
            const std::string_view value{argv[index]};
            if (value == "memory") {
                config.scenario = Scenario::memory;
            } else if (value == "memory-slow") {
                config.scenario = Scenario::memory_slow;
            } else if (value == "loopback") {
                config.scenario = Scenario::loopback;
            } else if (value == "loopback-slow") {
                config.scenario = Scenario::loopback_slow;
            } else if (value == "all") {
                config.scenario = Scenario::all;
            } else {
                return kBadScenario.data();
            }
            continue;
        }
        std::uint64_t* target = nullptr;
        if (key == "--entities") target = &config.entities;
        else if (key == "--connections") target = &config.connections;
        else if (key == "--visible-per-connection") target = &config.visible_per_connection;
        else if (key == "--tick-hz") target = &config.tick_hz;
        else if (key == "--change-percent") target = &config.change_percent;
        else if (key == "--keyframe-interval-seconds") target = &config.keyframe_interval_seconds;
        else if (key == "--warmup-seconds") target = &config.warmup_seconds;
        else if (key == "--sample-seconds") target = &config.sample_seconds;
        else if (key == "--slow-client-percent") target = &config.slow_client_percent;
        else if (key == "--slow-seconds") target = &config.slow_seconds;
        else if (key == "--seed") target = &config.seed;
        else if (key == "--validators") target = &config.validators;
        else if (key == "--command-rate") target = &config.command_rate;
        else return kUnknown.data();
        if (++index >= argc) {
            return kUnknown.data();
        }
        if (!parse_u64(argv[index], *target)) {
            return kNotNumber.data();
        }
    }
    return nullptr;
}

[[nodiscard]] const char* validate_config(const BenchmarkConfig& config) {
    if (config.entities == 0 || config.connections == 0
        || config.visible_per_connection == 0) {
        return "entities/connections/visible-per-connection 必须大于 0";
    }
    if (config.tick_hz == 0 || config.tick_hz > 1'000
        || kMicrosecondsPerSecond % config.tick_hz != 0) {
        return "tick-hz 必须整除 1000000 且不超过 1000";
    }
    if (config.change_percent > 100 || config.slow_client_percent > 100) {
        return "百分比参数必须在 [0,100]";
    }
    if (config.sample_seconds == 0) {
        return "sample-seconds 必须大于 0";
    }
    return nullptr;
}

// ---- 机器与构建信息（验收门 9 要求随结果输出）----

void print_environment(const BenchmarkConfig& config) {
    utsname system_info{};
    static_cast<void>(uname(&system_info));
    std::string cpu_model = "unknown";
    std::uint64_t memory_total_kib = 0;
    {
        std::ifstream cpuinfo{"/proc/cpuinfo"};
        std::string line;
        while (std::getline(cpuinfo, line)) {
            if (line.rfind("model name", 0) == 0) {
                const std::string::size_type colon = line.find(':');
                if (colon != std::string::npos) {
                    cpu_model = line.substr(colon + 2);
                }
                break;
            }
        }
        std::ifstream meminfo{"/proc/meminfo"};
        while (std::getline(meminfo, line)) {
            if (line.rfind("MemTotal", 0) == 0) {
                const std::string::size_type colon = line.find(':');
                if (colon != std::string::npos) {
                    const std::string_view rest{line.data() + colon + 1,
                                                line.size() - colon - 1};
                    const std::size_t first = rest.find_first_of("0123456789");
                    if (first != std::string_view::npos) {
                        const std::string_view digits = rest.substr(first);
                        std::uint64_t value = 0;
                        if (parse_u64(
                                digits.substr(
                                    0, digits.find_first_not_of("0123456789")),
                                value)) {
                            memory_total_kib = value;
                        }
                    }
                }
                break;
            }
        }
    }
#if defined(__clang__)
    const std::string compiler = std::string{"clang "} + __clang_version__;
#elif defined(__GNUC__)
    const std::string compiler = std::string{"gcc "} + __VERSION__;
#else
    const std::string compiler = "unknown";
#endif
    std::cout << "== environment ==\n"
              << "os=" << system_info.sysname << ' ' << system_info.release << ' '
              << system_info.machine << '\n'
              << "cpu_model=" << cpu_model << '\n'
              << "cpu_cores=" << std::thread::hardware_concurrency() << '\n'
              << "memory_total_mib=" << memory_total_kib / 1024 << '\n'
              << "compiler=" << compiler << '\n'
              << "build_type=" << GW_BENCH_BUILD_TYPE << '\n'
              << "dep_boost=" << GW_BENCH_BOOST_VERSION << '\n'
              << "dep_protobuf=" << GW_BENCH_PROTOBUF_VERSION << '\n'
              << "dep_grpc=" << GW_BENCH_GRPC_VERSION << '\n'
              << "dep_openssl=" << GW_BENCH_OPENSSL_VERSION << '\n'
              << "dep_flatbuffers=" << GW_BENCH_FLATBUFFERS_VERSION << '\n'
              << "== load parameters ==\n"
              << "entities=" << config.entities << '\n'
              << "connections=" << config.connections << '\n'
              << "visible_per_connection=" << config.visible_per_connection << '\n'
              << "tick_hz=" << config.tick_hz << '\n'
              << "change_percent=" << config.change_percent << '\n'
              << "keyframe_interval_seconds=" << config.keyframe_interval_seconds << '\n'
              << "warmup_seconds=" << config.warmup_seconds << '\n'
              << "sample_seconds=" << config.sample_seconds << '\n'
              << "slow_client_percent=" << config.slow_client_percent << '\n'
              << "slow_seconds=" << config.slow_seconds << '\n'
              << "seed=" << config.seed << '\n'
              << "validators=" << config.validators << '\n'
              << "command_rate=" << config.command_rate << '\n'
              << "world_extent_meters=" << kWorldExtentMeters << '\n';
}

// ---- 资源占用采样：只读取 /proc 与 getrusage 的真实值 ----

[[nodiscard]] std::optional<std::uint64_t> read_rss_kib() {
    std::ifstream status{"/proc/self/status"};
    std::string line;
    while (std::getline(status, line)) {
        if (line.rfind("VmRSS", 0) == 0) {
            const std::string::size_type colon = line.find(':');
            if (colon == std::string::npos) {
                return std::nullopt;
            }
            const std::string_view rest{line.data() + colon + 1,
                                        line.size() - colon - 1};
            const std::size_t first = rest.find_first_of("0123456789");
            if (first == std::string_view::npos) {
                return std::nullopt;
            }
            const std::string_view digits = rest.substr(first);
            std::uint64_t value = 0;
            if (!parse_u64(
                    digits.substr(0, digits.find_first_not_of("0123456789")),
                    value)) {
                return std::nullopt;
            }
            return value;
        }
    }
    return std::nullopt;
}

[[nodiscard]] double read_process_cpu_seconds() {
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        return 0.0;
    }
    const auto to_seconds = [](const timeval& value) {
        return static_cast<double>(value.tv_sec)
            + static_cast<double>(value.tv_usec) / 1'000'000.0;
    };
    return to_seconds(usage.ru_utime) + to_seconds(usage.ru_stime);
}

// ---- 延迟样本统计 ----

struct LatencySamples {
    std::vector<std::uint64_t> samples_us;

    void record(std::chrono::steady_clock::duration elapsed) {
        const auto microseconds =
            std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
        samples_us.push_back(static_cast<std::uint64_t>(std::max<std::int64_t>(0, microseconds)));
    }

    void print(std::string_view name) const {
        if (samples_us.empty()) {
            std::cout << name << "_count=0\n";
            return;
        }
        std::vector<std::uint64_t> ordered = samples_us;
        std::sort(ordered.begin(), ordered.end());
        const auto percentile = [&ordered](double fraction) {
            const std::size_t index = static_cast<std::size_t>(
                std::ceil(fraction * static_cast<double>(ordered.size())));
            return ordered[std::clamp<std::size_t>(index, 1, ordered.size()) - 1];
        };
        double sum = 0.0;
        for (const std::uint64_t value : ordered) {
            sum += static_cast<double>(value);
        }
        std::cout << name << "_count=" << ordered.size() << '\n'
                  << name << "_mean_us=" << sum / static_cast<double>(ordered.size()) << '\n'
                  << name << "_p50_us=" << percentile(0.50) << '\n'
                  << name << "_p95_us=" << percentile(0.95) << '\n'
                  << name << "_p99_us=" << percentile(0.99) << '\n'
                  << name << "_max_us=" << ordered.back() << '\n';
    }
};

// ---- 负载世界 ----

struct BenchmarkWorld {
    geoworld::world::World world;
    std::vector<WorldId> wids;
    std::mt19937_64 rng;
};

[[nodiscard]] geoworld::world::PositionEcef enu_position(double east, double north,
                                                         double up) {
    const geoworld::spatial::Ecef origin = geoworld::spatial::geodetic_to_ecef(kOriginGeodetic);
    const geoworld::spatial::Ecef point = geoworld::spatial::enu_to_ecef(
        origin, kOriginGeodetic, geoworld::spatial::Enu{east, north, up});
    return geoworld::world::PositionEcef{point.x, point.y, point.z};
}

void build_world(const BenchmarkConfig& config, BenchmarkWorld& data) {
    std::uniform_real_distribution<double> plane{-kWorldExtentMeters / 2.0,
                                                 kWorldExtentMeters / 2.0};
    std::uniform_real_distribution<double> altitude{0.0, kEntityMaxHeightMeters};
    std::uniform_real_distribution<double> speed{0.0, kSpeedMax};
    std::uniform_real_distribution<double> heading{0.0, kHeadingMax};
    data.wids.reserve(config.entities);
    for (std::uint64_t index = 0; index < config.entities; ++index) {
        const WorldId wid{index + 1};
        geoworld::world::WorldObject object;
        object.id = wid;
        object.position = enu_position(plane(data.rng), plane(data.rng),
                                       altitude(data.rng));
        object.semantic_type = std::string{kSemanticType};
        object.geometry_ref = std::string{kGeometryRefPrefix} + std::to_string(wid.value);
        object.lifecycle = geoworld::world::LifecycleState::active;
        object.properties.emplace(std::string{kPropertySpeed}, speed(data.rng));
        object.properties.emplace(std::string{kPropertyHeading}, heading(data.rng));
        object.state.emplace(
            std::string{kStateStatus},
            std::string{kStatusValues[wid.value % kStatusValues.size()]});
        if (!data.world.insert(std::move(object))) {
            std::cerr << "世界构建失败 wid=" << wid.value << '\n';
            std::exit(2);
        }
        data.wids.push_back(wid);
    }
}

// 每 tick 改变 change_percent% 实体的两个数值属性；旋转窗口保证均匀覆盖，值来自种子随机源。
void mutate_world(const BenchmarkConfig& config, BenchmarkWorld& data,
                  std::uint64_t tick) {
    const std::uint64_t batch = std::max<std::uint64_t>(
        1, config.entities * config.change_percent / 100);
    const std::uint64_t first = ((tick - 1) * batch) % config.entities;
    std::uniform_real_distribution<double> speed{0.0, kSpeedMax};
    std::uniform_real_distribution<double> heading{0.0, kHeadingMax};
    for (std::uint64_t offset = 0; offset < batch; ++offset) {
        const WorldId wid = data.wids[(first + offset) % config.entities];
        static_cast<void>(data.world.set_property(
            wid, std::string{kPropertySpeed}, speed(data.rng)));
        static_cast<void>(data.world.set_property(
            wid, std::string{kPropertyHeading}, heading(data.rng)));
    }
}

// AOI 中心均匀铺在世界网格上，边长按可见密度计算；默认参数下边长远大于间距，AOI 互相重叠。
[[nodiscard]] geoworld::projection::Subscription make_subscription(
    const BenchmarkConfig& config, std::uint64_t connection_index) {
    const std::uint64_t grid = static_cast<std::uint64_t>(
        std::ceil(std::sqrt(static_cast<double>(config.connections))));
    const double spacing = kWorldExtentMeters / static_cast<double>(grid);
    const double visible_ratio = std::min<double>(
        1.0, static_cast<double>(config.visible_per_connection)
                 / static_cast<double>(config.entities));
    const double half_side = kWorldExtentMeters * std::sqrt(visible_ratio) / 2.0;
    const double east = -kWorldExtentMeters / 2.0
        + (static_cast<double>(connection_index % grid) + 0.5) * spacing;
    const double north = -kWorldExtentMeters / 2.0
        + (static_cast<double>(connection_index / grid) + 0.5) * spacing;
    geoworld::projection::Subscription subscription;
    subscription.area = geoworld::spatial::Aabb{
        geoworld::spatial::Enu{east - half_side, north - half_side, kAoiMinUpMeters},
        geoworld::spatial::Enu{east + half_side, north + half_side, kAoiMaxUpMeters},
    };
    return subscription;
}

// ---- 组合：world + ProjectionEngine + GatewayCore（与 geoworldd 相同的注入路径）----

struct Harness {
    explicit Harness(const BenchmarkConfig& config)
        : ticket_counter{std::make_shared<std::uint64_t>(0)},
          authentication{std::make_shared<geoworld::gateway::FixtureAuthentication>()},
          authorization{std::make_shared<geoworld::gateway::FixtureAuthorization>()},
          engine{make_projection_config(config), make_policy()},
          core{gateway_config, engine, authentication, authorization,
               [] { return std::chrono::steady_clock::now(); },
               [counter = ticket_counter] {
                   return "m4-bench-ticket-" + std::to_string(++*counter);
               },
               geoworld::protocol::control_api_version,
               geoworld::protocol::data_schema_version} {
        authentication->add_credential(std::string{kCredentialToken},
                                       {std::string{kPrincipalId}, false});
        core.set_frame_encoder(geoworld::gateway::make_frame_encoder());
        core.set_receipt_encoder(geoworld::gateway::make_receipt_encoder());
        core.set_heartbeat_encoder(geoworld::gateway::make_heartbeat_encoder());
        // 投影帧构建与帧编码按连接并行：输出与串行逐字节一致（hash_mismatches 验证）。
        // i7 级桌面核 HT 对内存密集型编码收益有限，工作线程取物理核数量级。
        const std::uint32_t hardware = std::thread::hardware_concurrency();
        thread_pool = std::make_shared<geoworld::foundation::ThreadPool>(
            hardware > 1 ? hardware - 1 : 1);
        engine.set_thread_pool(thread_pool);
        core.set_thread_pool(thread_pool);
        world_data.rng.seed(config.seed);
        build_world(config, world_data);
    }

    [[nodiscard]] static geoworld::projection::ProjectionConfig make_projection_config(
        const BenchmarkConfig& config) {
        geoworld::projection::ProjectionConfig projection_config;
        projection_config.data_frequency_hz = static_cast<std::uint32_t>(config.tick_hz);
        projection_config.keyframe_interval_seconds =
            static_cast<std::uint32_t>(config.keyframe_interval_seconds);
        projection_config.tick_dt_microseconds = static_cast<std::int64_t>(
            kMicrosecondsPerSecond / config.tick_hz);
        projection_config.enu_origin = geoworld::spatial::geodetic_to_ecef(kOriginGeodetic);
        std::string diagnostic;
        if (!geoworld::projection::validate(projection_config, diagnostic)) {
            std::cerr << "投影配置非法: " << diagnostic << '\n';
            std::exit(2);
        }
        return projection_config;
    }

    [[nodiscard]] static geoworld::projection::ProjectionPolicy make_policy() {
        geoworld::projection::ProjectionPolicy policy;
        policy.allow_property(std::string{kPropertySpeed});
        policy.allow_property(std::string{kPropertyHeading});
        policy.allow_state_key(std::string{kStateStatus});
        policy.set_frequency_resolver(
            [](const geoworld::world::WorldObject&) {
                return geoworld::projection::FrequencyClass::fast;
            });
        return policy;
    }

    // 为全部连接走真实控制面流程：open_session -> update_subscription。
    // loopback 场景的 attach_stream 由传输层在 WebSocket 握手时经 ticket 完成。
    void open_sessions(const BenchmarkConfig& config) {
        sessions.reserve(config.connections);
        connection_ids.resize(config.connections);
        for (std::uint64_t index = 0; index < config.connections; ++index) {
            GatewayCore::OpenSessionResult opened = core.open_session(
                std::string{kCredentialToken},
                geoworld::protocol::control_api_version,
                geoworld::protocol::control_api_version,
                geoworld::protocol::data_schema_version,
                geoworld::protocol::data_schema_version, 0);
            if (opened.error != geoworld::gateway::GatewayError::none) {
                std::cerr << "open_session 失败 code="
                          << geoworld::gateway::error_code(opened.error) << '\n';
                std::exit(4);
            }
            const geoworld::gateway::GatewayError subscription_error =
                core.update_subscription(opened.session.session.id,
                                         make_subscription(config, index));
            if (subscription_error != geoworld::gateway::GatewayError::none) {
                std::cerr << "update_subscription 失败 code="
                          << geoworld::gateway::error_code(subscription_error) << '\n';
                std::exit(4);
            }
            sessions.push_back(opened.session);
        }
    }

    // 内存 sink 场景：直接在进程内 redeem ticket 并 attach。
    void attach_all(const BenchmarkConfig& config) {
        open_sessions(config);
        for (std::uint64_t index = 0; index < config.connections; ++index) {
            const ConnectionId connection{index + 1};
            const std::optional<ConnectionId> attached = core.attach_stream(
                connection, sessions[index].stream_ticket);
            if (!attached.has_value()) {
                std::cerr << "attach_stream 失败 index=" << index << '\n';
                std::exit(4);
            }
            connection_ids[index] = connection;
        }
    }

    std::shared_ptr<std::uint64_t> ticket_counter;
    std::shared_ptr<geoworld::gateway::FixtureAuthentication> authentication;
    std::shared_ptr<geoworld::gateway::FixtureAuthorization> authorization;
    std::shared_ptr<geoworld::foundation::ThreadPool> thread_pool;
    BenchmarkWorld world_data;
    geoworld::gateway::GatewayConfig gateway_config{};
    ProjectionEngine engine;
    GatewayCore core;
    std::vector<geoworld::gateway::OpenedSession> sessions;
    std::vector<ConnectionId> connection_ids;
};

// ---- 每连接统计与慢连接选择 ----

struct ConnectionStats {
    std::uint64_t drained_bytes{};
    std::uint64_t frames{};
    std::uint64_t keyframes{};
    std::uint64_t deltas{};
    std::uint64_t heartbeats{};
    std::uint64_t reliable_events{};
    std::uint64_t acks_sent{};
    bool disconnected{};
    std::string disconnect_reason;
};

[[nodiscard]] std::uint64_t slow_connection_count(const BenchmarkConfig& config,
                                                  bool slow_scenario) {
    if (!slow_scenario) {
        return 0;
    }
    return config.connections * config.slow_client_percent / 100;
}

[[nodiscard]] bool is_slow_connection(const BenchmarkConfig& config,
                                      std::uint64_t slow_count, std::uint64_t index) {
    if (slow_count == 0) {
        return false;
    }
    const std::uint64_t stride = config.connections / slow_count;
    return stride > 0 && index % stride == 0 && index / stride < slow_count;
}

// ---- 内存模型测量（docs/M4.md §3 末段要求分项报告）----

// 连接侧内存样本必须在连接仍挂载时采集；loopback 在 transport.shutdown() 后
// engine 中的连接投影已被拆除，不能事后再读。
struct ConnectionMemorySample {
    bool attached{};
    std::uint64_t view_entries{};
    std::uint64_t unacked_record_bytes{};
    std::uint64_t fid_entries{};
};

struct EngineMemorySnapshot {
    std::uint64_t history_frames{};
    std::vector<ConnectionMemorySample> connections;
};

[[nodiscard]] EngineMemorySnapshot capture_memory_snapshot(const Harness& harness) {
    EngineMemorySnapshot snapshot;
    snapshot.history_frames = harness.engine.history_size();
    snapshot.connections.reserve(harness.connection_ids.size());
    for (const ConnectionId id : harness.connection_ids) {
        ConnectionMemorySample sample;
        if (const geoworld::projection::ConnectionProjection* connection =
                harness.engine.connection(id)) {
            sample.attached = true;
            sample.view_entries = connection->current_view().size();
            sample.unacked_record_bytes = connection->unacked_bytes();
            sample.fid_entries = connection->fid_count();
        }
        snapshot.connections.push_back(sample);
    }
    return snapshot;
}

[[nodiscard]] std::size_t estimate_projected_entity_bytes(
    const geoworld::projection::ProjectedEntity& entity) {
    std::size_t bytes = sizeof(entity) + entity.semantic_type.capacity()
        + entity.geometry_ref.capacity();
    const auto bag_bytes = [](const geoworld::world::PropertyBag& bag) {
        std::size_t total = 0;
        for (const auto& [key, value] : bag) {
            total += sizeof(geoworld::world::PropertyBag::value_type) + key.capacity();
            if (const auto* text = std::get_if<std::string>(&value)) {
                total += text->capacity();
            }
        }
        return total;
    };
    bytes += bag_bytes(entity.properties) + bag_bytes(entity.state);
    return bytes;
}

void print_memory_model(const Harness& harness, const BenchmarkConfig& config,
                        const EngineMemorySnapshot& snapshot) {
    const geoworld::world::WorldObject* sample =
        harness.world_data.world.find(harness.world_data.wids.front());
    std::size_t entity_bytes = 0;
    if (sample != nullptr) {
        // engine.policy() 不是 const 接口；用同一工厂重建等价策略做样例投影估算。
        geoworld::projection::ProjectionPolicy policy = Harness::make_policy();
        entity_bytes = estimate_projected_entity_bytes(policy.project(*sample));
    }
    const std::uint64_t changed_per_frame = std::max<std::uint64_t>(
        1, config.entities * config.change_percent / 100);
    // 共享历史字节是按下限公式估算：帧数 × 每帧变化实体 × 单个规范化投影字节；
    // 不含 shared_ptr 控制块与 vector 余量。真实帧数来自 engine.history_size()。
    std::uint64_t unacked_record_bytes = 0;
    std::uint64_t view_entries = 0;
    std::uint64_t fid_entries = 0;
    for (const ConnectionMemorySample& connection : snapshot.connections) {
        unacked_record_bytes += connection.unacked_record_bytes;
        view_entries += connection.view_entries;
        fid_entries += connection.fid_entries;
    }
    using ViewEntry = std::pair<const WorldId, geoworld::projection::TrackedEntity>;
    using FidEntry = std::pair<const WorldId, geoworld::foundation::FeatureId>;
    using FidReverseEntry = std::pair<const std::uint32_t, WorldId>;
    const std::uint64_t view_bytes_est = view_entries * sizeof(ViewEntry);
    const std::uint64_t fid_bytes_est =
        fid_entries * (sizeof(FidEntry) + sizeof(FidReverseEntry));
    std::cout << "memory_shared_history_frames=" << snapshot.history_frames << '\n'
              << "memory_shared_history_changed_entities_per_frame=" << changed_per_frame
              << '\n'
              << "memory_projected_entity_bytes_est=" << entity_bytes << '\n'
              << "memory_shared_history_bytes_est="
              << snapshot.history_frames * changed_per_frame * entity_bytes << '\n'
              << "memory_connection_unacked_record_bytes=" << unacked_record_bytes << '\n'
              << "memory_connection_view_entries=" << view_entries << '\n'
              << "memory_connection_fid_entries=" << fid_entries << '\n'
              << "memory_connection_state_bytes_est="
              << unacked_record_bytes + view_bytes_est + fid_bytes_est << '\n'
              // 公共 API 缺口：GatewayCore 不暴露 StateQueue/ReliableQueue 占用，
              // StateQueue::bytes()/resync_required() 存在但无法从核心外部读取。
              << "memory_send_queue_bytes=unavailable(gateway-core-public-api-gap)\n";
}

// ---- 场景公共报告 ----

struct ScenarioReport {
    LatencySamples fanout;       // 投影边界 -> 全部健康连接完成 Gateway 入队
    LatencySamples projection;   // on_projection：采集 + 规范化 + AOI + delta 构建
    LatencySamples encode_enqueue; // pump：FlatBuffers 编码 + Gateway 入队
    // 按本 tick 是否产生 keyframe 帧分解 fanout，定位长尾构成。
    LatencySamples fanout_keyframe_ticks;
    LatencySamples fanout_normal_ticks;
    std::uint64_t keyframe_frames_total{};
    std::uint64_t keyframe_frames_tick_max{};
    std::uint64_t ticks{};
    std::uint64_t tick_overruns{};
    double achieved_tick_hz{};
    double cpu_cores{};
    std::uint64_t rss_baseline_kib{};
    std::uint64_t rss_end_kib{};
    std::uint64_t rss_peak_kib{};
    std::uint64_t hash_mismatches{};
    std::uint64_t slow_count{};
    std::vector<ConnectionStats> connections;
};

void print_report(std::string_view scenario, const BenchmarkConfig& config,
                  const ScenarioReport& report, const Harness& harness,
                  const EngineMemorySnapshot& snapshot) {
    std::uint64_t total_bytes = 0;
    std::uint64_t total_frames = 0;
    std::uint64_t keyframes = 0;
    std::uint64_t deltas = 0;
    std::uint64_t heartbeats = 0;
    std::uint64_t reliables = 0;
    std::uint64_t acks = 0;
    std::uint64_t disconnects = 0;
    std::uint64_t measured_visible_sum = 0;
    std::uint64_t active_count = 0;
    std::vector<double> healthy_bandwidth;
    for (std::size_t index = 0; index < report.connections.size(); ++index) {
        const ConnectionStats& stats = report.connections[index];
        total_bytes += stats.drained_bytes;
        total_frames += stats.frames;
        keyframes += stats.keyframes;
        deltas += stats.deltas;
        heartbeats += stats.heartbeats;
        reliables += stats.reliable_events;
        acks += stats.acks_sent;
        disconnects += stats.disconnected ? 1 : 0;
        if (index < snapshot.connections.size() && snapshot.connections[index].attached
            && !stats.disconnected) {
            measured_visible_sum += snapshot.connections[index].view_entries;
            ++active_count;
        }
        if (!stats.disconnected
            && !is_slow_connection(config, report.slow_count, index)) {
            healthy_bandwidth.push_back(
                static_cast<double>(stats.drained_bytes)
                / static_cast<double>(config.sample_seconds));
        }
    }
    double bandwidth_mean = 0.0;
    double bandwidth_max = 0.0;
    for (const double value : healthy_bandwidth) {
        bandwidth_mean += value;
        bandwidth_max = std::max(bandwidth_max, value);
    }
    if (!healthy_bandwidth.empty()) {
        bandwidth_mean /= static_cast<double>(healthy_bandwidth.size());
    }
    const double rss_growth_percent = report.rss_baseline_kib == 0
        ? 0.0
        : 100.0
              * (static_cast<double>(report.rss_end_kib)
                 - static_cast<double>(report.rss_baseline_kib))
              / static_cast<double>(report.rss_baseline_kib);

    std::cout << "== scenario: " << scenario << " ==\n"
              << "ticks=" << report.ticks << '\n'
              << "achieved_tick_hz=" << report.achieved_tick_hz << '\n'
              << "tick_overruns=" << report.tick_overruns << '\n'
              << "latency_definition=projection_boundary_to_gateway_enqueue_all_healthy\n";
    report.fanout.print("fanout_latency");
    report.fanout_keyframe_ticks.print("fanout_keyframe_tick");
    report.fanout_normal_ticks.print("fanout_normal_tick");
    const double keyframes_per_tick_mean = report.ticks == 0
        ? 0.0
        : static_cast<double>(report.keyframe_frames_total)
              / static_cast<double>(report.ticks);
    std::cout << "keyframes_per_tick_mean=" << keyframes_per_tick_mean << '\n'
              << "keyframes_per_tick_max=" << report.keyframe_frames_tick_max << '\n';
    report.projection.print("projection_phase");
    report.encode_enqueue.print("encode_enqueue_phase");
    std::cout << "frames_total=" << total_frames << '\n'
              << "keyframes_total=" << keyframes << '\n'
              << "deltas_total=" << deltas << '\n'
              << "heartbeats_total=" << heartbeats << '\n'
              << "reliable_events_total=" << reliables << '\n'
              << "acks_total=" << acks << '\n'
              << "drained_bytes_total=" << total_bytes << '\n'
              << "throughput_bytes_per_second="
              << static_cast<double>(total_bytes)
                     / static_cast<double>(config.sample_seconds)
              << '\n'
              << "per_connection_bandwidth_mean_bytes_per_second=" << bandwidth_mean
              << '\n'
              << "per_connection_bandwidth_max_bytes_per_second=" << bandwidth_max << '\n'
              << "per_connection_bandwidth_mean_mib_per_second="
              << bandwidth_mean / static_cast<double>(kBytesPerMebibyte) << '\n'
              << "measured_visible_per_connection_mean="
              << (active_count == 0
                      ? 0.0
                      : static_cast<double>(measured_visible_sum)
                            / static_cast<double>(active_count))
              << '\n'
              << "disconnects_total=" << disconnects << '\n';
    for (std::size_t index = 0; index < report.connections.size(); ++index) {
        const ConnectionStats& stats = report.connections[index];
        if (stats.disconnected) {
            std::cout << "disconnect_connection=" << harness.connection_ids[index].value
                      << " reason=" << stats.disconnect_reason << '\n';
        }
    }
    std::cout << "cpu_cores_avg=" << report.cpu_cores << '\n'
              << "rss_baseline_mib=" << report.rss_baseline_kib / 1024 << '\n'
              << "rss_end_mib=" << report.rss_end_kib / 1024 << '\n'
              << "rss_peak_mib=" << report.rss_peak_kib / 1024 << '\n'
              << "rss_growth_percent_after_warmup=" << rss_growth_percent << '\n'
              << "hash_mismatches=" << report.hash_mismatches << '\n';
    print_memory_model(harness, config, snapshot);
}

// ---- (a) 内存 sink 场景 ----

// 解码校验连接排干一帧：分类计数、重建 replica 并与服务端投影视图 hash 比对。
void drain_memory_connection(Harness& harness, ConnectionId id, ConnectionStats& stats,
                             bool validate,
                             geoworld::protocol::ReplicaAccumulator& replica,
                             std::uint64_t& hash_mismatches) {
    bool applied_state_frame = false;
    while (std::optional<geoworld::gateway::FrameBytes> bytes =
               harness.core.next_outbound(id)) {
        stats.drained_bytes += bytes->size();
        ++stats.frames;
        if (!validate) {
            continue;
        }
        geoworld::protocol::DecodeFailure failure;
        const std::optional<geoworld::protocol::WireFrame> frame =
            geoworld::protocol::decode_server_frame(
                std::span<const std::uint8_t>{
                    reinterpret_cast<const std::uint8_t*>(bytes->data()), bytes->size()},
                failure);
        if (!frame.has_value()) {
            std::cerr << "帧解码失败 connection=" << id.value
                      << " code=" << failure.error_code << '\n';
            ++hash_mismatches;
            continue;
        }
        if (std::holds_alternative<geoworld::protocol::WireKeyframe>(*frame)) {
            ++stats.keyframes;
            applied_state_frame = true;
        } else if (std::holds_alternative<geoworld::protocol::WireDelta>(*frame)) {
            ++stats.deltas;
            applied_state_frame = true;
        } else if (std::holds_alternative<geoworld::protocol::WireHeartbeat>(*frame)) {
            ++stats.heartbeats;
        } else {
            ++stats.reliable_events;
        }
        replica.apply(*frame);
    }
    if (validate && applied_state_frame
        && replica.hash() != harness.engine.connection_view_hash(id)) {
        std::cerr << "replica hash 与服务端投影视图不一致 connection=" << id.value
                  << '\n';
        ++hash_mismatches;
    }
}

// 模拟及时 ack 的健康客户端：确认该连接已发送记录中的最新 snapshot。
void acknowledge_latest(Harness& harness, ConnectionId id, ConnectionStats& stats) {
    const geoworld::projection::ConnectionProjection* connection =
        harness.engine.connection(id);
    if (connection == nullptr || connection->unacked_records().empty()) {
        return;
    }
    const std::uint64_t newest = connection->unacked_records().back().snapshot_id;
    if (newest <= connection->acked_baseline()) {
        return;
    }
    if (harness.core.inbound_ack(id, connection->stream_epoch(), newest)) {
        ++stats.acks_sent;
    }
}

[[nodiscard]] int run_memory_scenario(const BenchmarkConfig& config,
                                      bool slow_scenario) {
    Harness harness{config};
    harness.attach_all(config);
    const std::uint64_t slow_count = slow_connection_count(config, slow_scenario);
    const std::uint64_t warmup_ticks = config.warmup_seconds * config.tick_hz;
    const std::uint64_t sample_ticks = config.sample_seconds * config.tick_hz;
    const std::uint64_t total_ticks = warmup_ticks + sample_ticks;
    const std::uint64_t slow_window_ticks = config.slow_seconds * config.tick_hz;
    const auto tick_period = std::chrono::microseconds{
        static_cast<std::int64_t>(kMicrosecondsPerSecond / config.tick_hz)};

    ScenarioReport report;
    report.slow_count = slow_count;
    report.connections.resize(config.connections);
    std::vector<geoworld::protocol::ReplicaAccumulator> replicas(config.connections);
    std::vector<char> detached(config.connections, 0);

    const auto run_started = std::chrono::steady_clock::now();
    double cpu_baseline = 0.0;
    auto sample_started = run_started;
    for (std::uint64_t tick = 1; tick <= total_ticks; ++tick) {
        const bool sampling = tick > warmup_ticks;
        if (sampling && tick == warmup_ticks + 1) {
            cpu_baseline = read_process_cpu_seconds();
            sample_started = std::chrono::steady_clock::now();
            report.rss_baseline_kib = read_rss_kib().value_or(0);
        }
        const bool in_slow_window = slow_count > 0 && sampling
            && tick <= warmup_ticks + slow_window_ticks;

        mutate_world(config, harness.world_data, tick);
        const auto projection_started = std::chrono::steady_clock::now();
        // state_hash 仅供观察者记录，ProjectionEngine 当前不消费该参数。
        harness.engine.on_projection(harness.world_data.world, tick, 0);
        const auto projection_done = std::chrono::steady_clock::now();
        harness.core.pump(tick);
        const std::uint64_t tick_keyframes = harness.core.last_pump_keyframe_count();
        const auto enqueue_done = std::chrono::steady_clock::now();

        for (std::uint64_t index = 0; index < config.connections; ++index) {
            if (detached[index] != 0) {
                continue;
            }
            const ConnectionId id = harness.connection_ids[index];
            ConnectionStats& stats = report.connections[index];
            const bool slow_now =
                is_slow_connection(config, slow_count, index) && in_slow_window;
            if (!slow_now) {
                const bool validate =
                    index >= config.connections - config.validators
                    && !is_slow_connection(config, slow_count, index);
                drain_memory_connection(harness, id, stats, validate,
                                        replicas[index], report.hash_mismatches);
                acknowledge_latest(harness, id, stats);
            }
            // 与真实传输一致：核心判定必须断开即拆除连接并记录原因。
            if (harness.core.must_disconnect(id)) {
                stats.disconnected = true;
                stats.disconnect_reason =
                    geoworld::gateway::error_code(harness.core.disconnect_reason(id));
                harness.core.detach_stream(id);
                detached[index] = 1;
            }
        }

        if (sampling) {
            report.projection.record(projection_done - projection_started);
            report.encode_enqueue.record(enqueue_done - projection_done);
            report.fanout.record(enqueue_done - projection_started);
            if (tick_keyframes > 0) {
                report.fanout_keyframe_ticks.record(enqueue_done - projection_started);
            } else {
                report.fanout_normal_ticks.record(enqueue_done - projection_started);
            }
            report.keyframe_frames_total += tick_keyframes;
            report.keyframe_frames_tick_max =
                std::max(report.keyframe_frames_tick_max, tick_keyframes);
            ++report.ticks;
            const std::optional<std::uint64_t> rss = read_rss_kib();
            if (rss.has_value()) {
                report.rss_peak_kib = std::max(report.rss_peak_kib, *rss);
            }
        }
        const auto tick_deadline =
            run_started + tick_period * static_cast<std::int64_t>(tick);
        if (std::chrono::steady_clock::now() > tick_deadline) {
            if (sampling) {
                ++report.tick_overruns;
            }
        } else {
            std::this_thread::sleep_until(tick_deadline);
        }
    }

    const double wall_seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - sample_started).count();
    report.achieved_tick_hz = wall_seconds > 0.0
        ? static_cast<double>(report.ticks) / wall_seconds
        : 0.0;
    report.cpu_cores = wall_seconds > 0.0
        ? (read_process_cpu_seconds() - cpu_baseline) / wall_seconds
        : 0.0;
    report.rss_end_kib = read_rss_kib().value_or(0);
    report.rss_peak_kib = std::max(report.rss_peak_kib, report.rss_end_kib);

    const EngineMemorySnapshot snapshot = capture_memory_snapshot(harness);
    print_report(slow_scenario ? "memory-slow" : "memory", config, report, harness,
                 snapshot);
    return report.hash_mismatches == 0 ? 0 : 3;
}

// ---- (b) loopback 真实 WebSocket 场景 ----

struct LoopbackShared {
    std::atomic<std::uint64_t> connect_gate{0};
    std::atomic<bool> stopping{false};
    // 0=未开始 1=慢窗口内 2=已结束
    std::atomic<int> slow_phase{0};
};

struct LoopbackClientContext {
    std::uint64_t index{};
    bool slow{};
    bool validator{};
    std::string ticket;
    geoworld::client::StreamClientConfig client_config;
    LoopbackShared* shared{};
    ConnectionStats stats;
    std::atomic<bool> connected{false};
    std::atomic<bool> first_hash_posted{false};
    std::atomic<std::uint64_t> first_keyframe_hash{0};
    std::atomic<bool> final_posted{false};
    std::atomic<std::uint64_t> final_hash{0};
    // 客户端线程在连接意外结束（非收尾阶段）时置位；stats 只在线程 join 后读取。
    std::atomic<bool> stream_failed{false};
    // 只由主线程读写：首个 keyframe hash 是否已与服务端视图比对过。
    bool first_hash_verified{};
};

void run_loopback_client(LoopbackClientContext& context) {
    // 连接按序号逐个放行，主线程据此把 transport 分配的 ConnectionId 对齐到连接下标。
    while (context.shared->connect_gate.load(std::memory_order_acquire) != context.index
           && !context.shared->stopping.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    geoworld::client::StreamClient client{context.client_config};
    std::string diagnostic;
    if (!client.connect_stream(diagnostic, context.ticket)) {
        std::cerr << "loopback 客户端连接失败 index=" << context.index << ": "
                  << diagnostic << '\n';
        context.connected.store(true, std::memory_order_release);
        context.final_posted.store(true, std::memory_order_release);
        return;
    }
    context.connected.store(true, std::memory_order_release);

    geoworld::protocol::ReplicaAccumulator replica;
    while (true) {
        if (context.slow
            && context.shared->slow_phase.load(std::memory_order_acquire) == 1
            && !context.shared->stopping.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
            continue;
        }
        std::optional<geoworld::protocol::WireFrame> frame =
            client.read_frame(diagnostic);
        if (!frame.has_value()) {
            // stopping 期间读超时或对端关闭都视为正常收尾。
            if (!context.shared->stopping.load(std::memory_order_acquire)) {
                context.stats.disconnected = true;
                context.stats.disconnect_reason = "transport_closed";
                context.stream_failed.store(true, std::memory_order_release);
            }
            break;
        }
        std::uint64_t epoch = 0;
        std::uint64_t snapshot = 0;
        bool state_frame = true;
        if (const auto* keyframe =
                std::get_if<geoworld::protocol::WireKeyframe>(&*frame)) {
            ++context.stats.keyframes;
            epoch = keyframe->stream_epoch;
            snapshot = keyframe->snapshot_id;
        } else if (const auto* delta =
                       std::get_if<geoworld::protocol::WireDelta>(&*frame)) {
            ++context.stats.deltas;
            epoch = delta->stream_epoch;
            snapshot = delta->snapshot_id;
        } else if (std::holds_alternative<geoworld::protocol::WireHeartbeat>(*frame)) {
            ++context.stats.heartbeats;
            state_frame = false;
        } else {
            ++context.stats.reliable_events;
            state_frame = false;
        }
        if (context.validator) {
            replica.apply(*frame);
        }
        ++context.stats.frames;
        if (state_frame) {
            if (!client.send_ack(epoch, snapshot, diagnostic)) {
                break;
            }
            ++context.stats.acks_sent;
            if (context.validator
                && !context.first_hash_posted.load(std::memory_order_relaxed)
                && std::holds_alternative<geoworld::protocol::WireKeyframe>(*frame)) {
                context.first_keyframe_hash.store(replica.hash(),
                                                  std::memory_order_release);
                context.first_hash_posted.store(true, std::memory_order_release);
            }
        }
    }
    if (context.validator) {
        context.final_hash.store(replica.hash(), std::memory_order_release);
    }
    context.final_posted.store(true, std::memory_order_release);
}

// ---- (c) 门 8 命令 admission 测量（--command-rate，仅 loopback）----
// 命令客户端走完整参考路径：gRPC open_session -> update_subscription(follow 自身目标)
// -> acquire_ownership -> 数据面连接 -> 按固定速率 SubmitCommand(SetProperty)。
// admission 延迟 = RPC 发出 -> 收到 accepted 应答（含控制面封送主线程执行的等待）；
// 终态回执经会话数据面可靠队列投递，按 client_sequence 核对不丢失、不重复。

struct CommandClientShared {
    // fanout 连接的 ConnectionId 对齐完成后才放行命令客户端的数据面握手。
    std::atomic<bool> streams_allowed{false};
    std::atomic<bool> submission_enabled{true};
    // 只记录采样窗口内的 admission 样本；提交本身全程保持以维持 ack 活性。
    std::atomic<bool> recording{false};
    std::atomic<bool> stopping{false};
};

struct CommandClientContext {
    std::uint64_t index{};
    WorldId target{};
    std::uint64_t rate_per_second{};
    std::uint64_t lease_ticks{};
    geoworld::client::StreamClientConfig client_config;
    CommandClientShared* shared{};
    // 结果字段由 reader/submitter 线程持 mutex 写入，主线程在 join 后读取。
    std::mutex mutex;
    LatencySamples admission;
    std::unordered_map<std::uint64_t, std::uint64_t> receipts;
    std::string diagnostic;
    std::uint64_t submitted{};
    std::uint64_t accepted{};
    std::uint64_t rpc_failed{};
    std::uint64_t admission_rejected{};
    std::atomic<bool> control_ready{false};
    std::atomic<bool> stream_ready{false};
    std::atomic<bool> submitter_done{false};
    std::atomic<bool> failed{false};
};

void command_client_fail(CommandClientContext& context, std::string diagnostic) {
    {
        std::lock_guard lock{context.mutex};
        context.diagnostic = std::move(diagnostic);
    }
    context.failed.store(true, std::memory_order_release);
    // 失败也要放行另一线程的等待，避免收尾死锁。
    context.control_ready.store(true, std::memory_order_release);
    context.stream_ready.store(true, std::memory_order_release);
}

// 数据面读循环：ack 状态帧保活，按 client_sequence 统计终态回执。
void run_command_client_reader(CommandClientContext& context,
                               geoworld::client::StreamClient& client) {
    while ((!context.control_ready.load(std::memory_order_acquire)
            || !context.shared->streams_allowed.load(std::memory_order_acquire))
           && !context.failed.load(std::memory_order_acquire)
           && !context.shared->stopping.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(kPollSleep);
    }
    if (context.failed.load(std::memory_order_acquire)
        || context.shared->stopping.load(std::memory_order_acquire)) {
        context.stream_ready.store(true, std::memory_order_release);
        return;
    }
    std::string diagnostic;
    if (!client.connect_stream(diagnostic)) {
        command_client_fail(context, "connect_stream: " + diagnostic);
        return;
    }
    context.stream_ready.store(true, std::memory_order_release);
    while (true) {
        std::optional<geoworld::protocol::WireFrame> frame =
            client.read_frame(diagnostic);
        if (!frame.has_value()) {
            if (!context.shared->stopping.load(std::memory_order_acquire)) {
                command_client_fail(context, "read_frame: " + diagnostic);
            }
            return;
        }
        std::uint64_t epoch = 0;
        std::uint64_t snapshot = 0;
        bool state_frame = true;
        if (const auto* keyframe =
                std::get_if<geoworld::protocol::WireKeyframe>(&*frame)) {
            epoch = keyframe->stream_epoch;
            snapshot = keyframe->snapshot_id;
        } else if (const auto* delta =
                       std::get_if<geoworld::protocol::WireDelta>(&*frame)) {
            epoch = delta->stream_epoch;
            snapshot = delta->snapshot_id;
        } else if (const auto* reliable =
                       std::get_if<geoworld::protocol::WireReliable>(&*frame)) {
            state_frame = false;
            if (reliable->kind == geoworld::protocol::ReliableKind::command_receipt) {
                std::lock_guard lock{context.mutex};
                ++context.receipts[reliable->receipt.client_sequence];
            }
        } else {
            state_frame = false; // Heartbeat 不参与回执统计。
        }
        if (state_frame && !client.send_ack(epoch, snapshot, diagnostic)) {
            command_client_fail(context, "send_ack: " + diagnostic);
            return;
        }
    }
}

// 控制面 + 提交循环：gRPC 会话与所有权建立后按固定速率提交，只记录采样窗口样本。
// 与 reader 共享同一 StreamClient：控制面（gRPC channel）与数据面（WebSocket）
// 是不相交的成员与传输，两线程各走一路，无共享可变状态。
void run_command_client_submitter(CommandClientContext& context,
                                  geoworld::client::StreamClient& client) {
    std::string diagnostic;
    if (!client.open_session(diagnostic)) {
        command_client_fail(context, "open_session: " + diagnostic);
        context.submitter_done.store(true, std::memory_order_release);
        return;
    }
    geoworld::projection::Subscription subscription;
    subscription.follow = context.target;
    subscription.follow_radius_meters = kCommandFollowRadiusMeters;
    if (!client.update_subscription(subscription, diagnostic)) {
        command_client_fail(context, "update_subscription: " + diagnostic);
        context.submitter_done.store(true, std::memory_order_release);
        return;
    }
    if (!client.acquire_ownership(context.target, {std::string{kPropertySpeed}},
                                  client.session().current_tick + context.lease_ticks,
                                  diagnostic)) {
        command_client_fail(context, "acquire_ownership: " + diagnostic);
        context.submitter_done.store(true, std::memory_order_release);
        return;
    }
    context.control_ready.store(true, std::memory_order_release);

    // 终态回执只投递到会话已挂载的连接，数据面就绪前提交会漏收回执。
    while (!context.stream_ready.load(std::memory_order_acquire)
           && !context.failed.load(std::memory_order_acquire)
           && !context.shared->stopping.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(kPollSleep);
    }
    std::uint64_t sequence = 0;
    const auto start = std::chrono::steady_clock::now();
    while (!context.shared->stopping.load(std::memory_order_acquire)
           && context.shared->submission_enabled.load(std::memory_order_acquire)
           && !context.failed.load(std::memory_order_acquire)) {
        ++sequence;
        // 固定速率：到期时刻相对起点有理数累积，不作浮点累加，避免漂移。
        const auto due = start
            + std::chrono::microseconds{static_cast<std::int64_t>(
                kMicrosecondsPerSecond * sequence / context.rate_per_second)};
        std::this_thread::sleep_until(due);
        if (context.shared->stopping.load(std::memory_order_acquire)
            || !context.shared->submission_enabled.load(std::memory_order_acquire)
            || context.failed.load(std::memory_order_acquire)) {
            break;
        }
        const auto rpc_started = std::chrono::steady_clock::now();
        // expected_object_version=0：不检查版本，命令恒合法。
        std::optional<geoworld::client::SubmitResult> result =
            client.submit_set_property(context.target, std::string{kPropertySpeed},
                                       geoworld::world::PropertyValue{
                                           static_cast<double>(sequence)},
                                       sequence, 0, diagnostic);
        const auto rpc_done = std::chrono::steady_clock::now();
        std::lock_guard lock{context.mutex};
        ++context.submitted;
        if (!result.has_value()) {
            ++context.rpc_failed;
            context.diagnostic = diagnostic;
            continue;
        }
        if (result->status != geoworld::gateway::ReceiptStatus::accepted) {
            ++context.admission_rejected;
            context.diagnostic = result->error_code;
            continue;
        }
        ++context.accepted;
        if (context.shared->recording.load(std::memory_order_acquire)) {
            context.admission.record(rpc_done - rpc_started);
        }
    }
    context.submitter_done.store(true, std::memory_order_release);
}

[[nodiscard]] int run_loopback_scenario(const BenchmarkConfig& config,
                                        bool slow_scenario) {
    Harness harness{config};
    geoworld::gateway::TransportConfig transport_config;
    transport_config.port = 0; // 临时端口，避免与其他进程冲突。
    geoworld::gateway::StreamTransport transport{
        harness.core, geoworld::protocol::ProtocolLimits{}, transport_config};
    std::string diagnostic;
    if (!transport.start(diagnostic)) {
        std::cerr << "transport 启动失败: " << diagnostic << '\n';
        return 4;
    }
    // 会话与订阅在进程内经控制面接口建立；数据面 attach 由传输握手经 ticket 完成。
    harness.open_sessions(config);

    // ---- 门 8：--command-rate 命令 admission 测量 ----
    // gRPC 控制面与 fanout 传输同进程运行；RPC 经封送由主线程 poll 执行，
    // GatewayCore 不从 gRPC 工作线程触碰。命令经 CommandBuffer 在 tick 边界应用。
    std::atomic<std::uint64_t> authoritative_tick{0};
    geoworld::simulation::CommandBuffer command_buffer;
    std::unique_ptr<geoworld::gateway::ControlServer> control;
    if (config.command_rate > 0) {
        harness.authorization->allow_writable_property(std::string{kPropertySpeed});
        harness.core.set_command_submitter(
            [&command_buffer](std::uint64_t target_tick,
                              geoworld::simulation::CommandPayload payload,
                              geoworld::simulation::CommandMeta meta) {
                return command_buffer.enqueue(target_tick, std::move(payload), meta);
            });
        geoworld::gateway::ControlServerConfig control_config;
        control_config.port = 0; // 临时端口，避免与其他进程冲突。
        control_config.tick_source = [&authoritative_tick] {
            return authoritative_tick.load(std::memory_order_acquire);
        };
        control = std::make_unique<geoworld::gateway::ControlServer>(harness.core,
                                                                     control_config);
        if (!control->start(diagnostic)) {
            std::cerr << "控制面启动失败: " << diagnostic << '\n';
            return 4;
        }
    }
    const auto poll_transports = [&transport, &control] {
        transport.poll();
        if (control != nullptr) {
            control->poll();
        }
    };

    const std::uint64_t slow_count = slow_connection_count(config, slow_scenario);
    LoopbackShared shared;
    std::vector<std::unique_ptr<LoopbackClientContext>> contexts;
    contexts.reserve(config.connections);
    std::vector<std::thread> threads;
    threads.reserve(config.connections);
    for (std::uint64_t index = 0; index < config.connections; ++index) {
        auto context = std::make_unique<LoopbackClientContext>();
        context->index = index;
        context->slow = is_slow_connection(config, slow_count, index);
        context->validator =
            index >= config.connections - config.validators && !context->slow;
        context->ticket = harness.sessions[index].stream_ticket;
        context->client_config.stream_port = transport.bound_port();
        context->client_config.io_timeout = kClientIoTimeout;
        context->shared = &shared;
        contexts.push_back(std::move(context));
    }
    for (auto& context : contexts) {
        threads.emplace_back(run_loopback_client, std::ref(*context));
    }

    // 顺序建连并把 transport 分配的 ConnectionId 对齐到连接下标。
    std::vector<ConnectionId> active_seen;
    const std::uint64_t warmup_ticks = config.warmup_seconds * config.tick_hz;
    const std::uint64_t sample_ticks = config.sample_seconds * config.tick_hz;
    const std::uint64_t total_ticks = warmup_ticks + sample_ticks;
    const std::uint64_t slow_window_ticks = config.slow_seconds * config.tick_hz;
    const auto tick_period = std::chrono::microseconds{
        static_cast<std::int64_t>(kMicrosecondsPerSecond / config.tick_hz)};

    // 命令客户端池：每会话命令预算有限（GatewayConfig 基线），池大小按预算分摊；
    // 每客户端独占一个目标实体，租约覆盖整个运行窗口。数据面握手等 fanout
    // 连接对齐完成后放行，避免占用对齐窗口内的 ConnectionId。
    CommandClientShared command_shared;
    std::vector<std::unique_ptr<CommandClientContext>> command_contexts;
    std::vector<std::unique_ptr<geoworld::client::StreamClient>> command_clients;
    std::vector<std::thread> command_threads;
    if (control != nullptr) {
        const std::uint64_t per_session_budget =
            harness.gateway_config.command_rate_per_session;
        const std::uint64_t pool = std::min(
            config.entities,
            std::max<std::uint64_t>(
                1, (config.command_rate + per_session_budget - 1) / per_session_budget));
        const std::uint64_t lease_ticks =
            total_ticks + harness.gateway_config.max_command_lead_ticks + 1;
        command_contexts.reserve(pool);
        command_clients.reserve(pool);
        for (std::uint64_t index = 0; index < pool; ++index) {
            auto context = std::make_unique<CommandClientContext>();
            context->index = index;
            context->target = WorldId{index + 1};
            // 聚合速率在池内均匀分摊，余数从低序号客户端起每家 +1。
            context->rate_per_second = config.command_rate / pool
                + (index < config.command_rate % pool ? 1 : 0);
            context->lease_ticks = lease_ticks;
            context->client_config.control_address =
                "127.0.0.1:" + std::to_string(control->bound_port());
            context->client_config.stream_port = transport.bound_port();
            context->client_config.credential_token = std::string{kCredentialToken};
            context->client_config.io_timeout = kClientIoTimeout;
            context->shared = &command_shared;
            command_clients.push_back(std::make_unique<geoworld::client::StreamClient>(
                context->client_config));
            command_contexts.push_back(std::move(context));
        }
        for (std::size_t index = 0; index < command_contexts.size(); ++index) {
            command_threads.emplace_back(run_command_client_submitter,
                                         std::ref(*command_contexts[index]),
                                         std::ref(*command_clients[index]));
            command_threads.emplace_back(run_command_client_reader,
                                         std::ref(*command_contexts[index]),
                                         std::ref(*command_clients[index]));
        }
    }

    bool mapping_failed = false;
    for (std::uint64_t index = 0; index < config.connections; ++index) {
        const auto deadline = std::chrono::steady_clock::now() + kConnectStepTimeout;
        while (!contexts[index]->connected.load(std::memory_order_acquire)) {
            poll_transports();
            if (std::chrono::steady_clock::now() > deadline) {
                mapping_failed = true;
                break;
            }
            std::this_thread::sleep_for(kPollSleep);
        }
        const std::size_t expected = active_seen.size() + 1;
        while (transport.active_connections().size() < expected
               && std::chrono::steady_clock::now() <= deadline) {
            poll_transports();
            std::this_thread::sleep_for(kPollSleep);
        }
        const std::vector<ConnectionId> active = transport.active_connections();
        ConnectionId assigned{};
        for (const ConnectionId id : active) {
            if (std::find(active_seen.begin(), active_seen.end(), id)
                == active_seen.end()) {
                assigned = id;
                break;
            }
        }
        if (!assigned.valid()) {
            mapping_failed = true;
        }
        active_seen.push_back(assigned);
        harness.connection_ids[index] = assigned;
        shared.connect_gate.store(index + 1, std::memory_order_release);
        if (mapping_failed) {
            break;
        }
    }
    if (mapping_failed) {
        std::cerr << "loopback 建连或 ConnectionId 对齐失败\n";
        shared.stopping.store(true, std::memory_order_release);
        shared.connect_gate.store(config.connections, std::memory_order_release);
        command_shared.stopping.store(true, std::memory_order_release);
        transport.shutdown();
        for (auto& thread : threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        for (auto& thread : command_threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        return 4;
    }

    // fanout 连接对齐完成：放行命令客户端数据面握手并等待就绪（gRPC 建会话、
    // 所有权与握手都经封送，等待期间主线程持续 poll 两个传输面）。
    if (control != nullptr) {
        command_shared.streams_allowed.store(true, std::memory_order_release);
        const auto deadline = std::chrono::steady_clock::now() + kConnectStepTimeout;
        bool all_ready = false;
        bool setup_failed = false;
        while (std::chrono::steady_clock::now() < deadline) {
            poll_transports();
            all_ready = true;
            for (const auto& context : command_contexts) {
                if (context->failed.load(std::memory_order_acquire)) {
                    setup_failed = true;
                } else if (!context->stream_ready.load(std::memory_order_acquire)) {
                    all_ready = false;
                }
            }
            if (setup_failed || all_ready) {
                break;
            }
            std::this_thread::sleep_for(kPollSleep);
        }
        if (setup_failed || !all_ready) {
            std::cerr << "命令客户端建立失败\n";
            for (const auto& context : command_contexts) {
                if (context->failed.load(std::memory_order_acquire)) {
                    std::lock_guard lock{context->mutex};
                    std::cerr << "  index=" << context->index << ": "
                              << context->diagnostic << '\n';
                }
            }
            shared.stopping.store(true, std::memory_order_release);
            command_shared.stopping.store(true, std::memory_order_release);
            transport.shutdown();
            for (auto& thread : threads) {
                if (thread.joinable()) {
                    thread.join();
                }
            }
            for (auto& thread : command_threads) {
                if (thread.joinable()) {
                    thread.join();
                }
            }
            return 4;
        }
    }

    ScenarioReport report;
    report.slow_count = slow_count;
    report.connections.resize(config.connections);

    // 启动屏障：先推进一个 tick 产出初始 keyframe，世界冻结等待全部校验连接
    // 应用并回贴 replica hash，比对通过后才开始计时主循环；消除视图推进竞争。
    harness.engine.on_projection(harness.world_data.world, 1, 0);
    harness.core.pump(1);
    poll_transports();
    std::uint64_t barrier_hash_mismatches = 0;
    {
        const auto barrier_deadline =
            std::chrono::steady_clock::now() + kConnectStepTimeout;
        while (std::chrono::steady_clock::now() < barrier_deadline) {
            poll_transports();
            bool all_posted = true;
            for (const auto& context : contexts) {
                if (context->validator
                    && !context->first_hash_posted.load(std::memory_order_acquire)) {
                    all_posted = false;
                    break;
                }
            }
            if (all_posted) {
                break;
            }
            std::this_thread::sleep_for(kPollSleep);
        }
        for (std::uint64_t index = 0; index < config.connections; ++index) {
            LoopbackClientContext& context = *contexts[index];
            if (!context.validator
                || !context.first_hash_posted.load(std::memory_order_acquire)) {
                continue;
            }
            const ConnectionId id = harness.connection_ids[index];
            if (context.first_keyframe_hash.load(std::memory_order_acquire)
                != harness.engine.connection_view_hash(id)) {
                std::cerr << "初始 keyframe 后 replica hash 不一致 connection="
                          << id.value << '\n';
                ++barrier_hash_mismatches;
            }
            context.first_hash_verified = true;
        }
    }

    const auto run_started = std::chrono::steady_clock::now();
    double cpu_baseline = 0.0;
    auto sample_started = run_started;
    bool baseline_taken = false;
    for (std::uint64_t tick = 2; tick <= total_ticks; ++tick) {
        const bool sampling = tick > warmup_ticks;
        if (sampling && !baseline_taken) {
            baseline_taken = true;
            cpu_baseline = read_process_cpu_seconds();
            sample_started = std::chrono::steady_clock::now();
            report.rss_baseline_kib = read_rss_kib().value_or(0);
            command_shared.recording.store(true, std::memory_order_release);
            if (slow_count > 0) {
                shared.slow_phase.store(1, std::memory_order_release);
            }
        }
        if (slow_count > 0 && sampling && tick > warmup_ticks + slow_window_ticks
            && shared.slow_phase.load(std::memory_order_acquire) == 1) {
            shared.slow_phase.store(2, std::memory_order_release);
        }

        authoritative_tick.store(tick, std::memory_order_release);
        // 命令在 tick 边界应用，与 geoworldd 的 step 语义一致；终态回执即席入可靠队列。
        if (control != nullptr) {
            geoworld::simulation::ApplyReport apply_report =
                command_buffer.apply(harness.world_data.world, tick);
            if (apply_report.applied > 0 || apply_report.rejected > 0) {
                harness.core.on_commands_applied(apply_report);
            }
        }
        mutate_world(config, harness.world_data, tick);
        const auto projection_started = std::chrono::steady_clock::now();
        harness.engine.on_projection(harness.world_data.world, tick, 0);
        const auto projection_done = std::chrono::steady_clock::now();
        harness.core.pump(tick);
        const auto enqueue_done = std::chrono::steady_clock::now();
        poll_transports();

        // 服务端记录必须断开的连接及原因（慢连接背压 GWG105 等）。
        for (std::uint64_t index = 0; index < config.connections; ++index) {
            const ConnectionId id = harness.connection_ids[index];
            if (!id.valid() || report.connections[index].disconnected) {
                continue;
            }
            if (harness.core.must_disconnect(id)) {
                report.connections[index].disconnected = true;
                report.connections[index].disconnect_reason =
                    geoworld::gateway::error_code(harness.core.disconnect_reason(id));
            }
        }

        if (sampling) {
            report.projection.record(projection_done - projection_started);
            report.encode_enqueue.record(enqueue_done - projection_done);
            report.fanout.record(enqueue_done - projection_started);
            ++report.ticks;
            const std::optional<std::uint64_t> rss = read_rss_kib();
            if (rss.has_value()) {
                report.rss_peak_kib = std::max(report.rss_peak_kib, *rss);
            }
        }
        const auto tick_deadline =
            run_started + tick_period * static_cast<std::int64_t>(tick - 1);
        if (std::chrono::steady_clock::now() > tick_deadline) {
            if (sampling) {
                ++report.tick_overruns;
            }
        }
        while (std::chrono::steady_clock::now() < tick_deadline) {
            poll_transports();
            std::this_thread::sleep_for(kPollSleep);
        }
    }

    // 采样墙钟到 tick 循环结束为止；收尾排干与关闭不计入吞吐与 CPU 统计。
    const auto loop_finished = std::chrono::steady_clock::now();
    // 内存模型与投影视图必须在连接仍挂载时采集。
    const EngineMemorySnapshot snapshot = capture_memory_snapshot(harness);

    // 命令测量收尾：停止提交并等 in-flight RPC 完成（封送需主线程 poll），再补齐
    // lead 窗口内已接受命令的 apply，保证每条 accepted 命令都有终态回执可核对。
    if (control != nullptr) {
        command_shared.submission_enabled.store(false, std::memory_order_release);
        const auto submit_deadline = std::chrono::steady_clock::now() + kDrainGraceTimeout;
        while (std::chrono::steady_clock::now() < submit_deadline) {
            poll_transports();
            bool all_done = true;
            for (const auto& context : command_contexts) {
                if (!context->submitter_done.load(std::memory_order_acquire)) {
                    all_done = false;
                    break;
                }
            }
            if (all_done) {
                break;
            }
            std::this_thread::sleep_for(kPollSleep);
        }
        for (std::uint64_t lead = 1;
             lead <= harness.gateway_config.max_command_lead_ticks; ++lead) {
            geoworld::simulation::ApplyReport apply_report = command_buffer.apply(
                harness.world_data.world, total_ticks + lead);
            if (apply_report.applied > 0 || apply_report.rejected > 0) {
                harness.core.on_commands_applied(apply_report);
            }
        }
    }

    // 收尾：停止推进世界，等待客户端排干后比对最终 replica hash。
    shared.stopping.store(true, std::memory_order_release);
    command_shared.stopping.store(true, std::memory_order_release);
    const auto drain_deadline = std::chrono::steady_clock::now() + kDrainGraceTimeout;
    bool all_final_posted = false;
    while (std::chrono::steady_clock::now() < drain_deadline) {
        poll_transports();
        all_final_posted = true;
        for (const auto& context : contexts) {
            if (context->validator
                && !context->final_posted.load(std::memory_order_acquire)) {
                all_final_posted = false;
                break;
            }
        }
        bool receipts_complete = true;
        for (const auto& context : command_contexts) {
            std::lock_guard lock{context->mutex};
            std::uint64_t received = 0;
            for (const auto& [sequence, count] : context->receipts) {
                received += count;
            }
            if (received < context->accepted) {
                receipts_complete = false;
                break;
            }
        }
        if (all_final_posted && receipts_complete) {
            break;
        }
        std::this_thread::sleep_for(kPollSleep);
    }
    static_cast<void>(all_final_posted);

    // 最终 hash 比对必须赶在 transport.shutdown() 拆除连接投影之前。
    for (std::uint64_t index = config.connections - config.validators;
         index < config.connections; ++index) {
        LoopbackClientContext& context = *contexts[index];
        if (!context.validator
            || context.stream_failed.load(std::memory_order_acquire)
            || !context.final_posted.load(std::memory_order_acquire)) {
            continue;
        }
        const ConnectionId id = harness.connection_ids[index];
        if (harness.engine.connection(id) != nullptr
            && context.final_hash.load(std::memory_order_acquire)
                   != harness.engine.connection_view_hash(id)) {
            std::cerr << "收尾 replica hash 不一致 connection=" << id.value << '\n';
            ++report.hash_mismatches;
        }
    }

    transport.shutdown();
    for (auto& thread : threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    for (auto& thread : command_threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }

    for (std::uint64_t index = 0; index < config.connections; ++index) {
        ConnectionStats& stats = report.connections[index];
        stats.frames = contexts[index]->stats.frames;
        stats.keyframes = contexts[index]->stats.keyframes;
        stats.deltas = contexts[index]->stats.deltas;
        stats.heartbeats = contexts[index]->stats.heartbeats;
        stats.reliable_events = contexts[index]->stats.reliable_events;
        stats.acks_sent = contexts[index]->stats.acks_sent;
        if (contexts[index]->stats.disconnected && !stats.disconnected) {
            stats.disconnected = true;
            stats.disconnect_reason = contexts[index]->stats.disconnect_reason;
        }
    }

    const double wall_seconds = std::chrono::duration<double>(
        loop_finished - sample_started).count();
    report.achieved_tick_hz = wall_seconds > 0.0
        ? static_cast<double>(report.ticks) / wall_seconds
        : 0.0;
    report.cpu_cores = wall_seconds > 0.0
        ? (read_process_cpu_seconds() - cpu_baseline) / wall_seconds
        : 0.0;
    report.rss_end_kib = read_rss_kib().value_or(0);
    report.rss_peak_kib = std::max(report.rss_peak_kib, report.rss_end_kib);
    report.hash_mismatches += barrier_hash_mismatches;

    // loopback 的字节数不可经公共 API 观测（传输在内部排干队列），带宽以 memory 场景为准。
    print_report(slow_scenario ? "loopback-slow" : "loopback", config, report, harness,
                 snapshot);
    std::cout << "loopback_bandwidth_note=bytes-not-observable-via-public-api;"
                 "see-memory-scenario\n";

    // 门 8 输出与断言：终态回执不得丢失或重复，合法命令不得有 RPC 失败或 admission 拒绝。
    bool commands_ok = true;
    if (control != nullptr) {
        LatencySamples admission;
        std::uint64_t submitted = 0;
        std::uint64_t accepted = 0;
        std::uint64_t rpc_failed = 0;
        std::uint64_t admission_rejected = 0;
        std::uint64_t receipts_total = 0;
        std::uint64_t receipts_duplicated = 0;
        for (const auto& context : command_contexts) {
            if (context->failed.load(std::memory_order_acquire)) {
                commands_ok = false;
                std::cerr << "命令客户端失败 index=" << context->index << '\n';
            }
            submitted += context->submitted;
            accepted += context->accepted;
            rpc_failed += context->rpc_failed;
            admission_rejected += context->admission_rejected;
            for (const std::uint64_t sample : context->admission.samples_us) {
                admission.samples_us.push_back(sample);
            }
            for (const auto& [sequence, count] : context->receipts) {
                receipts_total += count;
                if (count > 1) {
                    receipts_duplicated += count - 1;
                }
            }
        }
        const std::uint64_t receipts_distinct = receipts_total - receipts_duplicated;
        const std::uint64_t receipts_missing =
            accepted > receipts_distinct ? accepted - receipts_distinct : 0;
        admission.print("command_admission_latency");
        std::cout << "command_submitted_total=" << submitted << '\n'
                  << "command_accepted_total=" << accepted << '\n'
                  << "command_rpc_failed_total=" << rpc_failed << '\n'
                  << "command_admission_rejected_total=" << admission_rejected << '\n'
                  << "command_receipts_total=" << receipts_total << '\n'
                  << "command_receipts_missing=" << receipts_missing << '\n'
                  << "command_receipts_duplicated=" << receipts_duplicated << '\n'
                  << "command_admission_note=includes-control-dispatch-marshal-wait\n";
        if (rpc_failed > 0 || admission_rejected > 0 || receipts_missing > 0
            || receipts_duplicated > 0) {
            commands_ok = false;
        }
    }
    return report.hash_mismatches == 0 && commands_ok ? 0 : 3;
}

} // namespace

int main(int argc, char** argv) {
    BenchmarkConfig config;
    if (const char* error = parse_args(argc, argv, config)) {
        std::cerr << "参数错误: " << error << '\n';
        print_usage();
        return 2;
    }
    if (const char* error = validate_config(config)) {
        std::cerr << "配置非法: " << error << '\n';
        return 2;
    }
    config.validators = std::min(config.validators, config.connections);
    print_environment(config);

    int status = 0;
    const auto run = [&config, &status](Scenario scenario) {
        int result = 0;
        switch (scenario) {
        case Scenario::memory:
            result = run_memory_scenario(config, false);
            break;
        case Scenario::memory_slow:
            result = run_memory_scenario(config, true);
            break;
        case Scenario::loopback:
            result = run_loopback_scenario(config, false);
            break;
        case Scenario::loopback_slow:
            result = run_loopback_scenario(config, true);
            break;
        case Scenario::all:
            break;
        }
        if (result != 0 && status == 0) {
            status = result;
        }
    };
    if (config.scenario == Scenario::all) {
        run(Scenario::memory);
        run(Scenario::memory_slow);
        run(Scenario::loopback);
        run(Scenario::loopback_slow);
    } else {
        run(config.scenario);
    }
    return status;
}
