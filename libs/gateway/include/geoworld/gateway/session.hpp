#pragma once

#include "geoworld/gateway/types.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>

namespace geoworld::gateway {

using SteadyClock = std::function<std::chrono::steady_clock::time_point()>;
using TokenGenerator = std::function<std::string()>;

struct SessionInfo {
    SessionId id{};
    Principal principal;
    std::uint32_t control_api_version{};
    std::uint32_t data_schema_version{};
    std::uint64_t created_tick{};
};

struct OpenedSession {
    SessionInfo session;
    std::string stream_ticket;
    std::uint64_t ticket_ttl_seconds{};
};

// 会话、短期一次性 stream ticket、每会话命令令牌桶和命令去重缓存。
class SessionManager {
public:
    struct Config {
        std::uint32_t ticket_ttl_seconds{30};
        std::uint32_t command_rate_per_session{100};
        std::size_t max_sessions{1024};
    };

    SessionManager(Config config, SteadyClock clock, TokenGenerator tokens);

    [[nodiscard]] std::optional<SessionInfo> find(SessionId id) const;
    [[nodiscard]] SessionId open(Principal principal, std::uint32_t control_version,
                                 std::uint32_t data_version, std::uint64_t current_tick);
    void close(SessionId id);

    // 签发一次性 ticket，绑定 session 与已协商版本。
    [[nodiscard]] std::string issue_ticket(SessionId id);
    // 校验即消费；过期、已使用或不匹配一律拒绝。
    [[nodiscard]] std::optional<SessionInfo> redeem_ticket(std::string_view ticket);

    // 命令令牌桶；返回 false 表示超过速率限制。
    [[nodiscard]] bool consume_command_budget(SessionId id);

    [[nodiscard]] std::optional<CommandReceipt> find_receipt(
        SessionId id, std::uint64_t client_sequence) const;
    void store_receipt(SessionId id, CommandReceipt receipt);

    [[nodiscard]] std::size_t size() const noexcept;

private:
    struct ReceiptKey {
        SessionId session;
        std::uint64_t client_sequence{};

        bool operator==(const ReceiptKey&) const = default;
    };

    struct ReceiptKeyHash {
        std::size_t operator()(const ReceiptKey& key) const noexcept;
    };

    struct TokenBucket {
        double tokens{};
        std::chrono::steady_clock::time_point last_refill{};
    };

    struct SessionState {
        SessionInfo info;
        TokenBucket bucket;
    };

    struct TicketState {
        SessionId session;
        std::chrono::steady_clock::time_point expires_at;
    };

    Config config_;
    SteadyClock clock_;
    TokenGenerator tokens_;
    std::uint64_t next_session_value_{1};
    std::unordered_map<SessionId, SessionState, SessionIdHash> sessions_;
    std::unordered_map<std::string, TicketState> tickets_;
    std::unordered_map<ReceiptKey, CommandReceipt, ReceiptKeyHash> receipts_;
};

} // namespace geoworld::gateway
