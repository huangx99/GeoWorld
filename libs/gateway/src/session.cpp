#include "geoworld/gateway/session.hpp"

namespace geoworld::gateway {

[[nodiscard]] std::size_t SessionManager::ReceiptKeyHash::operator()(
    const ReceiptKey& key) const noexcept {
    const std::size_t high = static_cast<std::size_t>(
        key.session.value ^ (key.session.value >> 32U));
    const std::size_t low = static_cast<std::size_t>(
        key.client_sequence ^ (key.client_sequence >> 32U));
    return high ^ (low * 1099511628211ULL);
}

SessionManager::SessionManager(Config config, SteadyClock clock, TokenGenerator tokens)
    : config_(config), clock_(std::move(clock)), tokens_(std::move(tokens)) {}

[[nodiscard]] std::optional<SessionInfo> SessionManager::find(SessionId id) const {
    const auto found = sessions_.find(id);
    if (found == sessions_.end()) {
        return std::nullopt;
    }
    return found->second.info;
}

[[nodiscard]] SessionId SessionManager::open(Principal principal,
                                             std::uint32_t control_version,
                                             std::uint32_t data_version,
                                             std::uint64_t current_tick) {
    if (sessions_.size() >= config_.max_sessions) {
        return SessionId{};
    }
    SessionState state;
    state.info = SessionInfo{SessionId{next_session_value_++}, std::move(principal),
                             control_version, data_version, current_tick};
    state.bucket.tokens = static_cast<double>(config_.command_rate_per_session);
    state.bucket.last_refill = clock_();
    const SessionId id = state.info.id;
    sessions_.emplace(id, std::move(state));
    return id;
}

void SessionManager::close(SessionId id) {
    sessions_.erase(id);
    for (auto ticket = tickets_.begin(); ticket != tickets_.end();) {
        if (ticket->second.session == id) {
            ticket = tickets_.erase(ticket);
        } else {
            ++ticket;
        }
    }
}

[[nodiscard]] std::string SessionManager::issue_ticket(SessionId id) {
    const auto found = sessions_.find(id);
    if (found == sessions_.end()) {
        return {};
    }
    std::string ticket = tokens_();
    tickets_.emplace(ticket, TicketState{
        id, clock_() + std::chrono::seconds{config_.ticket_ttl_seconds}});
    return ticket;
}

[[nodiscard]] std::optional<SessionInfo> SessionManager::redeem_ticket(
    std::string_view ticket) {
    const auto found = tickets_.find(std::string{ticket});
    if (found == tickets_.end()) {
        return std::nullopt;
    }
    // 一次性：取出即失效。
    const TicketState state = found->second;
    tickets_.erase(found);
    if (clock_() > state.expires_at) {
        return std::nullopt;
    }
    return find(state.session);
}

[[nodiscard]] bool SessionManager::consume_command_budget(SessionId id) {
    const auto found = sessions_.find(id);
    if (found == sessions_.end()) {
        return false;
    }
    TokenBucket& bucket = found->second.bucket;
    const auto now = clock_();
    const double elapsed = std::chrono::duration<double>(now - bucket.last_refill).count();
    const double rate = static_cast<double>(config_.command_rate_per_session);
    bucket.tokens = std::min(rate, bucket.tokens + elapsed * rate);
    bucket.last_refill = now;
    if (bucket.tokens < 1.0) {
        return false;
    }
    bucket.tokens -= 1.0;
    return true;
}

[[nodiscard]] std::optional<CommandReceipt> SessionManager::find_receipt(
    SessionId id, std::uint64_t client_sequence) const {
    const auto found = receipts_.find(ReceiptKey{id, client_sequence});
    if (found == receipts_.end()) {
        return std::nullopt;
    }
    return found->second;
}

void SessionManager::store_receipt(SessionId id, CommandReceipt receipt) {
    receipts_[ReceiptKey{id, receipt.client_sequence}] = receipt;
}

[[nodiscard]] std::size_t SessionManager::size() const noexcept {
    return sessions_.size();
}

} // namespace geoworld::gateway
