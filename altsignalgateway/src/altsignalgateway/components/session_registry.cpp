#include "altsignalgateway/components/session_registry.hpp"

#include "altsignalgateway/components/session.hpp"
#include "altsignalgateway/core/logger.hpp"

#include <algorithm>

namespace altsignalgateway::components {

SessionRegistry::SessionRegistry(int32_t max_clients, int32_t auth_failure_threshold,
                                 int32_t auth_failure_window_sec, int32_t auth_lockout_sec)
    : max_clients_(max_clients),
      auth_failure_threshold_(auth_failure_threshold),
      auth_failure_window_sec_(auth_failure_window_sec),
      auth_lockout_sec_(auth_lockout_sec) {}

bool SessionRegistry::attach(const std::shared_ptr<Session>& session) {
    std::lock_guard lock(mu_);
    if (static_cast<int32_t>(sessions_.size()) >= max_clients_) {
        log::warn("client_limit_reached",
                  {{"max_clients", max_clients_}, {"session_id", session->session_id()}});
        return false;
    }
    sessions_.emplace(session->session_id(), Entry{session, next_ordinal_++});
    return true;
}

void SessionRegistry::detach(const std::string& session_id) {
    std::lock_guard lock(mu_);
    sessions_.erase(session_id);
}

std::vector<std::shared_ptr<Session>> SessionRegistry::enforce_client_limit(
    const std::string& client_id, const std::string& keep_session_id, int32_t max_sessions) {
    std::vector<Entry> mine;
    {
        std::lock_guard lock(mu_);
        for (const auto& [id, e] : sessions_) {
            if (e.session->client_id() == client_id) mine.push_back(e);
        }
    }
    if (static_cast<int32_t>(mine.size()) <= max_sessions) return {};

    // Oldest first, by attach order.
    std::sort(mine.begin(), mine.end(),
              [](const Entry& a, const Entry& b) { return a.ordinal < b.ordinal; });
    std::vector<std::shared_ptr<Session>> superseded;
    int32_t to_drop = static_cast<int32_t>(mine.size()) - max_sessions;
    for (const auto& e : mine) {
        if (to_drop <= 0) break;
        // The session that just authenticated always survives: it is the one
        // the operator is watching connect, and the whole point of the policy
        // is that a reconnect wins over a socket that has not failed yet.
        if (e.session->session_id() == keep_session_id) continue;
        superseded.push_back(e.session);
        --to_drop;
    }
    return superseded;
}

std::vector<std::shared_ptr<Session>> SessionRegistry::snapshot() const {
    std::lock_guard lock(mu_);
    std::vector<std::shared_ptr<Session>> out;
    out.reserve(sessions_.size());
    for (const auto& [id, e] : sessions_) out.push_back(e.session);
    return out;
}

size_t SessionRegistry::size() const {
    std::lock_guard lock(mu_);
    return sessions_.size();
}

size_t SessionRegistry::authenticated_count() const {
    std::lock_guard lock(mu_);
    size_t n = 0;
    for (const auto& [id, e] : sessions_) {
        if (!e.session->client_id().empty()) ++n;
    }
    return n;
}

bool SessionRegistry::is_locked_out(const std::string& peer) const {
    std::lock_guard lock(mu_);
    const auto it = failures_.find(peer);
    if (it == failures_.end()) return false;
    return std::chrono::steady_clock::now() < it->second.locked_until;
}

void SessionRegistry::note_auth_failure(const std::string& peer) {
    std::lock_guard lock(mu_);
    const auto now = std::chrono::steady_clock::now();
    auto& rec = failures_[peer];
    // A sliding window, not a lifetime counter: a client that fails once a day
    // for a fortnight is not under attack, and locking it out on the fifteenth
    // day would be the service inventing an outage.
    if (rec.window_start == std::chrono::steady_clock::time_point{} ||
        now - rec.window_start > std::chrono::seconds(auth_failure_window_sec_)) {
        rec.window_start = now;
        rec.count = 0;
    }
    ++rec.count;
    if (rec.count >= auth_failure_threshold_) {
        rec.locked_until = now + std::chrono::seconds(auth_lockout_sec_);
        rec.count = 0;
        rec.window_start = now;
        log::warn("auth_lockout_applied",
                  {{"peer", peer}, {"lockout_sec", auth_lockout_sec_}});
    }
}

void SessionRegistry::note_auth_success(const std::string& peer) {
    std::lock_guard lock(mu_);
    failures_.erase(peer);
}

} // namespace altsignalgateway::components
