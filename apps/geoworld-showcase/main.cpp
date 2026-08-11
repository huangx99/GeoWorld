#include "geoworld/ai/decision.hpp"
#include "geoworld/ai/fsm.hpp"
#include "geoworld/debug/state_hash.hpp"
#include "geoworld/rules/rule_engine.hpp"
#include "geoworld/spatial/cell_grid.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace {

using geoworld::foundation::WorldId;

struct ObjectView {
    std::uint64_t id{};
    std::string type;
    int floor{};
    double x{};
    double y{};
    double z{};
    std::string status;
    geoworld::spatial::CellKey cell;
};

struct Frame {
    std::uint64_t tick{};
    std::vector<std::string> events;
    std::vector<ObjectView> objects;
};

struct PersonAgent {
    WorldId id;
    int initial_floor{};
    double start_x{};
    double start_y{};
    std::uint64_t evacuation_start{};
    std::uint64_t safe_tick{};
    geoworld::ai::FiniteStateMachine fsm;
};

struct BattlefieldEntity {
    float east{};
    float north{};
    float up{};
    float east_velocity{};
    float north_velocity{};
    std::uint8_t team{};
    std::uint8_t health{100};
    std::uint8_t attack_power{100};
    std::uint8_t defense_power{100};
    std::uint8_t death_tick{255};
};

struct BattlefieldFrame {
    std::uint64_t tick{};
    std::uint64_t blue_alive{};
    std::uint64_t red_alive{};
    std::uint64_t shots{};
    std::uint64_t hits{};
    std::uint64_t blue_losses{};
    std::uint64_t red_losses{};
};

struct BattlefieldSample {
    std::uint64_t id{};
    std::uint8_t team{};
    double latitude{};
    double longitude{};
    double height{};
    geoworld::spatial::Ecef ecef;
    geoworld::spatial::Enu enu;
    geoworld::spatial::CellKey cell;
};

struct BattlefieldResult {
    std::uint64_t entity_count{};
    std::uint64_t tick_count{};
    std::uint64_t seed{};
    geoworld::spatial::Geodetic origin;
    double generation_ms{};
    double update_ms{};
    double updates_per_second{};
    std::size_t cell_count{};
    std::uint64_t final_hash{};
    std::vector<BattlefieldSample> samples;
    std::vector<BattlefieldFrame> frames;
    std::string death_ticks_base64;
};

template <typename T>
T property(const geoworld::world::WorldObject& object, std::string_view key, T fallback) {
    const auto iterator = object.properties.find(std::string{key});
    if (iterator == object.properties.end()) {
        return fallback;
    }
    if (const auto* value = std::get_if<T>(&iterator->second)) {
        return *value;
    }
    return fallback;
}

std::string json_escape(std::string_view value) {
    std::ostringstream output;
    for (const auto character : value) {
        switch (character) {
        case '"': output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        case '<': output << "\\u003c"; break;
        default: output << character; break;
        }
    }
    return output.str();
}

std::string make_evacuation_json(const std::vector<Frame>& frames, std::uint64_t final_hash) {
    std::ostringstream output;
    output << std::fixed << std::setprecision(2);
    output << "{\"schema_version\":1,\"scenario\":\"Industrial Park Emergency\",";
    output << "\"run_id\":\"showcase-001\",\"final_hash\":\"" << final_hash << "\",";
    output << "\"frames\":[";
    for (std::size_t frame_index = 0; frame_index < frames.size(); ++frame_index) {
        const auto& frame = frames[frame_index];
        if (frame_index != 0) output << ',';
        output << "{\"tick\":" << frame.tick << ",\"events\":[";
        for (std::size_t event_index = 0; event_index < frame.events.size(); ++event_index) {
            if (event_index != 0) output << ',';
            output << '"' << json_escape(frame.events[event_index]) << '"';
        }
        output << "],\"objects\":[";
        for (std::size_t object_index = 0; object_index < frame.objects.size(); ++object_index) {
            const auto& object = frame.objects[object_index];
            if (object_index != 0) output << ',';
            output << "{\"id\":" << object.id
                   << ",\"type\":\"" << json_escape(object.type)
                   << "\",\"floor\":" << object.floor
                   << ",\"x\":" << object.x << ",\"y\":" << object.y << ",\"z\":" << object.z
                   << ",\"status\":\"" << json_escape(object.status)
                   << "\",\"cell_x\":" << object.cell.x
                   << ",\"cell_y\":" << object.cell.y
                   << ",\"cell_z\":" << object.cell.z << '}';
        }
        output << "]}";
    }
    output << "]}";
    return output.str();
}

std::uint32_t mix32(std::uint32_t value) noexcept {
    value ^= value >> 16;
    value *= 0x7feb352dU;
    value ^= value >> 15;
    value *= 0x846ca68bU;
    return value ^ (value >> 16);
}

double unit_random(std::uint32_t index, std::uint32_t salt) noexcept {
    return static_cast<double>(mix32(index + salt * 0x9e3779b9U + 0x4c44424fU)) / 4294967296.0;
}

std::uint64_t hash_mix(std::uint64_t hash, std::uint64_t value) noexcept {
    for (int byte = 0; byte < 8; ++byte) {
        hash ^= (value >> (byte * 8)) & 0xffU;
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::string base64_encode(const std::vector<std::uint8_t>& bytes) {
    constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    encoded.reserve((bytes.size() + 2) / 3 * 4);
    for (std::size_t index = 0; index < bytes.size(); index += 3) {
        const auto remaining = bytes.size() - index;
        const auto value = static_cast<std::uint32_t>(bytes[index]) << 16
            | (remaining > 1 ? static_cast<std::uint32_t>(bytes[index + 1]) << 8 : 0)
            | (remaining > 2 ? static_cast<std::uint32_t>(bytes[index + 2]) : 0);
        encoded.push_back(alphabet[(value >> 18) & 0x3fU]);
        encoded.push_back(alphabet[(value >> 12) & 0x3fU]);
        encoded.push_back(remaining > 1 ? alphabet[(value >> 6) & 0x3fU] : '=');
        encoded.push_back(remaining > 2 ? alphabet[value & 0x3fU] : '=');
    }
    return encoded;
}

BattlefieldResult run_battlefield() {
    constexpr std::uint64_t entity_count = 200'000;
    constexpr std::uint64_t tick_count = 120;
    constexpr std::uint64_t seed = 0x47574f524c44424fULL;
    const geoworld::spatial::Geodetic origin{35.25, 104.75, 920.0};
    const auto origin_ecef = geoworld::spatial::geodetic_to_ecef(origin);
    const geoworld::spatial::CellGrid grid{1000.0, 6};

    const auto generation_start = std::chrono::steady_clock::now();
    std::vector<BattlefieldEntity> entities(entity_count);
    for (std::uint32_t index = 0; index < entity_count; ++index) {
        auto& entity = entities[index];
        entity.team = index < entity_count / 2 ? 0 : 1;
        const auto team_index = index % static_cast<std::uint32_t>(entity_count / 2);
        const auto formation = team_index / 10'000U;
        const auto column = formation % 5U;
        const auto row = formation / 5U;
        const auto east_center = entity.team == 0
            ? -23'000.0F + static_cast<float>(column) * 3'500.0F
            : 9'000.0F + static_cast<float>(column) * 3'500.0F;
        const auto north_center = row == 0 ? -11'000.0F : 11'000.0F;
        entity.east = east_center + static_cast<float>(unit_random(index, 1)
            + unit_random(index, 6) - 1.0) * 2'300.0F;
        entity.north = north_center + static_cast<float>(unit_random(index, 2)
            + unit_random(index, 7) - 1.0) * 6'500.0F;
        entity.up = 10.0F + static_cast<float>(unit_random(index, 3) * 40.0);
        const auto direction = entity.team == 0 ? 1.0F : -1.0F;
        entity.east_velocity = direction * (120.0F + static_cast<float>(unit_random(index, 4) * 80.0));
        entity.north_velocity = static_cast<float>(unit_random(index, 5) * 12.0 - 6.0);
        entity.health = static_cast<std::uint8_t>(80U + unit_random(index, 8) * 81.0);
        entity.attack_power = static_cast<std::uint8_t>(40U + unit_random(index, 9) * 141.0);
        entity.defense_power = static_cast<std::uint8_t>(35U + unit_random(index, 10) * 126.0);
    }
    const auto generation_end = std::chrono::steady_clock::now();

    std::vector<BattlefieldFrame> battle_frames;
    std::uint64_t blue_alive = entity_count / 2;
    std::uint64_t red_alive = entity_count / 2;
    std::uint64_t shots = 0;
    std::uint64_t hits = 0;
    battle_frames.push_back({0, blue_alive, red_alive, 0, 0, 0, 0});
    const auto update_start = std::chrono::steady_clock::now();
    for (std::uint64_t tick = 0; tick < tick_count; ++tick) {
        for (auto& entity : entities) {
            if (entity.health == 0) continue;
            if (tick < 35) entity.east += entity.east_velocity;
            entity.north += entity.north_velocity;
            entity.north_velocity += (entity.team == 0 ? 1.0F : -1.0F)
                * static_cast<float>((static_cast<int>(tick) % 7) - 3) * 0.08F;
        }
        if (tick >= 7) {
            for (std::uint32_t pair = 0; pair < entity_count / 2; ++pair) {
                auto& blue = entities[pair];
                auto& red = entities[pair + entity_count / 2];
                if (blue.health == 0 || red.health == 0) continue;
                shots += 2;
                const auto blue_roll = mix32(pair + static_cast<std::uint32_t>(tick) * 0x45d9f3bU
                    + 0x4c44424fU);
                const auto red_roll = mix32(pair + static_cast<std::uint32_t>(tick) * 0x27d4eb2dU
                    + 0x9e3779b9U);
                const auto blue_chance = std::clamp(35 + static_cast<int>(blue.attack_power) / 3
                    - static_cast<int>(red.defense_power) / 5, 25, 125);
                const auto red_chance = std::clamp(35 + static_cast<int>(red.attack_power) / 3
                    - static_cast<int>(blue.defense_power) / 5, 25, 125);
                const auto blue_hit = blue_roll % 1000U < static_cast<std::uint32_t>(blue_chance);
                const auto red_hit = red_roll % 1000U < static_cast<std::uint32_t>(red_chance);
                hits += static_cast<std::uint64_t>(blue_hit) + static_cast<std::uint64_t>(red_hit);
                const auto blue_damage = red_hit ? std::max(3, 6 + static_cast<int>(red.attack_power) / 18
                    + static_cast<int>((red_roll >> 12) % 10U) - static_cast<int>(blue.defense_power) / 25) : 0;
                const auto red_damage = blue_hit ? std::max(3, 6 + static_cast<int>(blue.attack_power) / 18
                    + static_cast<int>((blue_roll >> 12) % 10U) - static_cast<int>(red.defense_power) / 25) : 0;
                if (blue_damage >= blue.health) {
                    blue.health = 0;
                    blue.death_tick = static_cast<std::uint8_t>(tick + 1);
                    --blue_alive;
                } else {
                    blue.health = static_cast<std::uint8_t>(blue.health - blue_damage);
                }
                if (red_damage >= red.health) {
                    red.health = 0;
                    red.death_tick = static_cast<std::uint8_t>(tick + 1);
                    --red_alive;
                } else {
                    red.health = static_cast<std::uint8_t>(red.health - red_damage);
                }
            }
        }
        battle_frames.push_back({tick + 1, blue_alive, red_alive, shots, hits,
            entity_count / 2 - blue_alive, entity_count / 2 - red_alive});
    }
    const auto update_end = std::chrono::steady_clock::now();

    std::unordered_set<geoworld::spatial::CellKey, geoworld::spatial::CellKeyHash> cells;
    cells.reserve(entity_count / 2);
    for (const auto& entity : entities) {
        cells.insert(grid.cell_for({entity.east, entity.north, entity.up}, 0));
    }

    BattlefieldResult result;
    result.entity_count = entity_count;
    result.tick_count = tick_count;
    result.seed = seed;
    result.origin = origin;
    result.generation_ms = std::chrono::duration<double, std::milli>(generation_end - generation_start).count();
    result.update_ms = std::chrono::duration<double, std::milli>(update_end - update_start).count();
    result.updates_per_second = result.update_ms > 0.0
        ? static_cast<double>(entity_count * tick_count) / (result.update_ms / 1000.0) : 0.0;
    result.cell_count = cells.size();
    result.frames = std::move(battle_frames);
    std::vector<std::uint8_t> death_ticks;
    death_ticks.reserve(entities.size());
    for (const auto& entity : entities) death_ticks.push_back(entity.death_tick);
    result.death_ticks_base64 = base64_encode(death_ticks);

    std::uint64_t hash = 1469598103934665603ULL;
    for (std::size_t index = 0; index < entities.size(); ++index) {
        const auto& entity = entities[index];
        hash = hash_mix(hash, index + 1);
        hash = hash_mix(hash, entity.team);
        hash = hash_mix(hash, entity.health);
        hash = hash_mix(hash, entity.attack_power);
        hash = hash_mix(hash, entity.defense_power);
        hash = hash_mix(hash, entity.death_tick);
        hash = hash_mix(hash, static_cast<std::uint64_t>(std::llround(entity.east * 100.0F)));
        hash = hash_mix(hash, static_cast<std::uint64_t>(std::llround(entity.north * 100.0F)));
    }
    result.final_hash = hash;

    for (const auto index : std::array<std::size_t, 4>{0, entity_count / 2,
                                                       entity_count - 2, entity_count - 1}) {
        const auto& entity = entities[index];
        const geoworld::spatial::Enu enu{entity.east, entity.north, entity.up};
        const auto ecef = geoworld::spatial::enu_to_ecef(origin_ecef, origin, enu);
        const auto geodetic = geoworld::spatial::ecef_to_geodetic(ecef);
        result.samples.push_back({index + 1, entity.team, geodetic.latitude_degrees,
            geodetic.longitude_degrees, geodetic.height_meters, ecef, enu, grid.cell_for(enu, 0)});
    }
    return result;
}

std::string make_battlefield_json(const BattlefieldResult& result) {
    std::ostringstream output;
    output << std::fixed << std::setprecision(6);
    output << "{\"entity_count\":" << result.entity_count
           << ",\"tick_count\":" << result.tick_count
           << ",\"combat\":{\"health_min\":80,\"health_max\":160,\"attack_min\":40,\"attack_max\":180,\"defense_min\":35,\"defense_max\":160}"
           << ",\"seed\":\"" << result.seed << "\",\"origin\":{";
    output << "\"latitude\":" << result.origin.latitude_degrees
           << ",\"longitude\":" << result.origin.longitude_degrees
           << ",\"height\":" << result.origin.height_meters << "},\"metrics\":{";
    output << "\"generation_ms\":" << result.generation_ms
           << ",\"update_ms\":" << result.update_ms
           << ",\"updates_per_second\":" << result.updates_per_second
           << ",\"cell_count\":" << result.cell_count
           << ",\"final_hash\":\"" << result.final_hash << "\"},\"samples\":[";
    for (std::size_t index = 0; index < result.samples.size(); ++index) {
        if (index != 0) output << ',';
        const auto& sample = result.samples[index];
        output << "{\"id\":" << sample.id << ",\"team\":" << static_cast<int>(sample.team)
               << ",\"latitude\":" << sample.latitude << ",\"longitude\":" << sample.longitude
               << ",\"height\":" << sample.height << ",\"ecef\":{";
        output << "\"x\":" << sample.ecef.x << ",\"y\":" << sample.ecef.y
               << ",\"z\":" << sample.ecef.z << "},\"enu\":{";
        output << "\"east\":" << sample.enu.east << ",\"north\":" << sample.enu.north
               << ",\"up\":" << sample.enu.up << "},\"cell\":{";
        output << "\"x\":" << sample.cell.x << ",\"y\":" << sample.cell.y
               << ",\"z\":" << sample.cell.z << "}}";
    }
    output << "],\"frames\":[";
    for (std::size_t index = 0; index < result.frames.size(); ++index) {
        if (index != 0) output << ',';
        const auto& frame = result.frames[index];
        output << "{\"tick\":" << frame.tick
               << ",\"blue_alive\":" << frame.blue_alive << ",\"red_alive\":" << frame.red_alive
               << ",\"shots\":" << frame.shots << ",\"hits\":" << frame.hits
               << ",\"blue_losses\":" << frame.blue_losses
               << ",\"red_losses\":" << frame.red_losses << '}';
    }
    output << "],\"death_ticks_base64\":\"" << result.death_ticks_base64 << "\"}";
    return output.str();
}

std::string make_combined_json(const std::vector<Frame>& frames, std::uint64_t evacuation_hash,
                               const BattlefieldResult& battlefield) {
    std::ostringstream output;
    output << "{\"schema_version\":2,\"scenarios\":{";
    output << "\"evacuation\":" << make_evacuation_json(frames, evacuation_hash)
           << ",\"battlefield\":" << make_battlefield_json(battlefield) << "}}";
    return output.str();
}

bool write_text(const std::filesystem::path& path, std::string_view value) {
    std::ofstream output(path, std::ios::binary);
    output.write(value.data(), static_cast<std::streamsize>(value.size()));
    return output.good();
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

geoworld::world::WorldObject make_object(WorldId id, std::string type,
                                         geoworld::schema::PropertyBag properties) {
    geoworld::world::WorldObject object;
    object.id = id;
    object.semantic_type = std::move(type);
    object.lifecycle = geoworld::world::LifecycleState::active;
    object.properties = std::move(properties);
    return object;
}

} // namespace

int main(int argc, char** argv) {
    const std::filesystem::path output_directory = argc > 1 ? argv[1] : "build/showcase";
    const std::filesystem::path template_path = "assets/showcase/offline_viewer.template.html";
    std::filesystem::create_directories(output_directory);

    geoworld::world::World world;
    geoworld::simulation::CommandBuffer commands;
    geoworld::rules::RuleEngine rules;
    geoworld::ai::DecisionIntentBuffer intents;
    geoworld::spatial::CellGrid grid{25.0, 4};

    static_cast<void>(commands.enqueue(0, geoworld::simulation::CreateObjectCommand{
        make_object({1}, "building", {{"alarm", false}, {"status", std::string{"normal"}}})
    }));
    for (int floor = 1; floor <= 3; ++floor) {
        static_cast<void>(commands.enqueue(0, geoworld::simulation::CreateObjectCommand{
            make_object({static_cast<std::uint64_t>(10 + floor)}, "floor",
                        {{"floor", static_cast<std::int64_t>(floor)},
                         {"status", std::string{"clear"}}, {"x", 50.0}, {"y", 50.0}})
        }));
    }
    static_cast<void>(commands.enqueue(0, geoworld::simulation::CreateObjectCommand{
        make_object({100}, "device", {{"floor", std::int64_t{2}}, {"x", 76.0}, {"y", 28.0},
                    {"temperature", 24.0}, {"alarm", false}, {"status", std::string{"normal"}}})
    }));

    std::vector<PersonAgent> agents;
    for (int index = 0; index < 12; ++index) {
        const auto floor = index / 4 + 1;
        const auto x = 18.0 + static_cast<double>(index % 4) * 15.0;
        const auto y = 22.0 + static_cast<double>(index % 3) * 13.0;
        const WorldId id{static_cast<std::uint64_t>(200 + index)};
        static_cast<void>(commands.enqueue(0, geoworld::simulation::CreateObjectCommand{
            make_object(id, "person", {{"floor", static_cast<std::int64_t>(floor)},
                        {"x", x}, {"y", y}, {"z", static_cast<double>(floor - 1) * 4.0},
                        {"status", std::string{"working"}}})
        }));
        PersonAgent agent{id, floor, x, y};
        static_cast<void>(agent.fsm.define_state({"working", {{"alarm", "evacuating", 0}}}));
        static_cast<void>(agent.fsm.define_state({"evacuating", {{"arrived", "safe", 0}}}));
        static_cast<void>(agent.fsm.define_state({"safe", {}}));
        static_cast<void>(agent.fsm.set_initial("working"));
        agents.push_back(std::move(agent));
    }

    static_cast<void>(rules.register_rule({
        "device-overheat", 1, 100, "temperature.changed",
        geoworld::rules::Expression{{{"temperature", geoworld::rules::Comparison::greater_equal, 80.0}}},
        {geoworld::rules::SetPropertyAction{{100}, "status", std::string{"fault"}},
         geoworld::rules::SetPropertyAction{{100}, "alarm", true},
         geoworld::rules::SetPropertyAction{{1}, "alarm", true},
         geoworld::rules::SetPropertyAction{{1}, "status", std::string{"emergency"}}}
    }));
    static_cast<void>(rules.register_rule({
        "smoke-detected", 1, 80, "smoke.detected",
        geoworld::rules::Expression{{{"intensity", geoworld::rules::Comparison::greater_equal, 0.5}}},
        {geoworld::rules::SetPropertyAction{{}, "status", std::string{"hazard"}}}
    }));

    std::vector<Frame> frames;
    bool previous_alarm = false;
    std::vector<std::string> previous_person_status(agents.size(), "working");
    std::vector<std::string> previous_floor_status(3, "clear");

    for (std::uint64_t tick = 0; tick <= 24; ++tick) {
        if (tick == 4) {
            static_cast<void>(commands.enqueue(tick, geoworld::simulation::SetPropertyCommand{
                {100}, "temperature", 92.0
            }));
        }
        static_cast<void>(commands.apply(world, tick));

        Frame frame;
        frame.tick = tick;
        if (tick == 4) frame.events.push_back("Device temperature reached 92 C");
        if (tick == 5) {
            static_cast<void>(rules.publish(tick, 100, "temperature.changed", {100},
                                            {{"temperature", 92.0}}));
            frame.events.push_back("Overheat event entered the rule engine");
        }
        if (tick == 8 || tick == 11 || tick == 14) {
            const auto floor = tick == 8 ? 2 : (tick == 11 ? 1 : 3);
            static_cast<void>(rules.publish(tick, 80, "smoke.detected",
                                            {static_cast<std::uint64_t>(10 + floor)},
                                            {{"intensity", 0.8}}));
            frame.events.push_back("Smoke event detected on floor " + std::to_string(floor));
        }
        static_cast<void>(rules.evaluate(tick, world, commands));

        const auto* building = world.find({1});
        const auto alarm = building != nullptr && property(*building, "alarm", false);
        if (alarm && !previous_alarm) frame.events.push_back("Emergency rule committed building alarm");
        previous_alarm = alarm;

        for (std::size_t index = 0; index < agents.size(); ++index) {
            auto& agent = agents[index];
            if (alarm && agent.fsm.state() == "working") {
                static_cast<void>(agent.fsm.dispatch("alarm"));
                agent.evacuation_start = tick;
                agent.safe_tick = tick + 6 + static_cast<std::uint64_t>(agent.initial_floor * 3)
                    + agent.id.value % 3;
                static_cast<void>(intents.submit(tick, {agent.id, "status", std::string{"evacuating"}}));
            }
            if (agent.fsm.state() == "evacuating") {
                if (tick >= agent.safe_tick) {
                    static_cast<void>(agent.fsm.dispatch("arrived"));
                    static_cast<void>(intents.submit(tick, {agent.id, "status", std::string{"safe"}}));
                    static_cast<void>(intents.submit(tick, {agent.id, "floor", std::int64_t{1}}));
                    static_cast<void>(intents.submit(tick, {agent.id, "x", 5.0}));
                    static_cast<void>(intents.submit(tick, {agent.id, "y", 90.0}));
                    static_cast<void>(intents.submit(tick, {agent.id, "z", 0.0}));
                } else {
                    const auto duration = static_cast<double>(agent.safe_tick - agent.evacuation_start);
                    const auto progress = static_cast<double>(tick - agent.evacuation_start) / duration;
                    const auto stair_progress = std::min(1.0, progress / 0.35);
                    const auto descend_progress = std::clamp((progress - 0.35) / 0.4, 0.0, 1.0);
                    const auto exit_progress = std::clamp((progress - 0.75) / 0.25, 0.0, 1.0);
                    const auto floor = std::max(1, agent.initial_floor
                        - static_cast<int>(descend_progress * static_cast<double>(agent.initial_floor - 1)));
                    const auto x = progress < 0.35 ? agent.start_x + (15.0 - agent.start_x) * stair_progress
                        : (progress < 0.75 ? 15.0 : 15.0 + (5.0 - 15.0) * exit_progress);
                    const auto y = progress < 0.35 ? agent.start_y + (80.0 - agent.start_y) * stair_progress
                        : (progress < 0.75 ? 80.0 : 80.0 + (90.0 - 80.0) * exit_progress);
                    const auto z = progress < 0.35 ? static_cast<double>(agent.initial_floor - 1) * 4.0
                        : static_cast<double>(agent.initial_floor - 1) * 4.0 * (1.0 - descend_progress);
                    static_cast<void>(intents.submit(tick, {agent.id, "floor", static_cast<std::int64_t>(floor)}));
                    static_cast<void>(intents.submit(tick, {agent.id, "x", x}));
                    static_cast<void>(intents.submit(tick, {agent.id, "y", y}));
                    static_cast<void>(intents.submit(tick, {agent.id, "z", z}));
                }
            }
        }
        static_cast<void>(intents.flush(tick, commands));

        for (std::size_t index = 0; index < agents.size(); ++index) {
            const auto* person = world.find(agents[index].id);
            if (person == nullptr) continue;
            const auto status = property(*person, "status", std::string{"unknown"});
            if (status != previous_person_status[index]) {
                frame.events.push_back("Person " + std::to_string(person->id.value) + " became " + status);
                previous_person_status[index] = status;
            }
        }
        for (int floor = 1; floor <= 3; ++floor) {
            const auto* floor_object = world.find({static_cast<std::uint64_t>(10 + floor)});
            if (floor_object == nullptr) continue;
            const auto status = property(*floor_object, "status", std::string{"clear"});
            if (status != previous_floor_status[static_cast<std::size_t>(floor - 1)]) {
                frame.events.push_back("Floor " + std::to_string(floor) + " became " + status);
                previous_floor_status[static_cast<std::size_t>(floor - 1)] = status;
            }
        }

        for (const auto& object : world.snapshot()) {
            if (object.semantic_type != "person" && object.semantic_type != "device"
                && object.semantic_type != "floor") {
                continue;
            }
            const auto floor = static_cast<int>(property(object, "floor", std::int64_t{1}));
            const auto x = property(object, "x", 50.0);
            const auto y = property(object, "y", 50.0);
            const auto z = property(object, "z", static_cast<double>(floor - 1) * 4.0);
            frame.objects.push_back({
                object.id.value, object.semantic_type, floor, x, y, z,
                property(object, "status", std::string{"unknown"}),
                grid.cell_for_2_5d({x, y, static_cast<double>(floor - 1) * 4.0}, 0, 4.0)
            });
        }
        frames.push_back(std::move(frame));
    }

    const auto final_hash = geoworld::debug::world_state_hash(world);
    const auto battlefield = run_battlefield();
    if (battlefield.entity_count != 200'000 || battlefield.frames.size() != 121
        || battlefield.frames.back().blue_losses == 0 || battlefield.frames.back().red_losses == 0) {
        std::cerr << "battlefield combat validation failed\n";
        return 1;
    }
    const auto json = make_combined_json(frames, final_hash, battlefield);
    auto html = read_text(template_path);
    const std::string placeholder = "__GEOWORLD_SHOWCASE_DATA__";
    const auto placeholder_position = html.find(placeholder);
    if (html.empty() || placeholder_position == std::string::npos) {
        std::cerr << "showcase template is missing or invalid\n";
        return 1;
    }
    html.replace(placeholder_position, placeholder.size(), json);

    const auto json_path = output_directory / "showcase-run.json";
    const auto html_path = output_directory / "showcase.html";
    if (!write_text(json_path, json) || !write_text(html_path, html)) {
        std::cerr << "failed to write showcase output\n";
        return 1;
    }
    std::cout << "GeoWorld showcase generated\n"
              << "  HTML: " << std::filesystem::absolute(html_path) << '\n'
              << "  JSON: " << std::filesystem::absolute(json_path) << '\n'
              << "  evacuation_hash=" << final_hash << '\n'
              << "  battlefield_entities=" << battlefield.entity_count << '\n'
              << "  battlefield_hash=" << battlefield.final_hash << '\n'
              << "  battlefield_updates_per_second=" << battlefield.updates_per_second << '\n';
    return 0;
}
