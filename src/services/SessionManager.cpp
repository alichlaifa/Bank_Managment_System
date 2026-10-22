#include "services/SessionManager.h"

#include <cstddef>
#include <random>
#include <utility>

namespace bank {

namespace {

constexpr std::size_t kTokenBytes = 32;
constexpr std::size_t kTokenHexChars = kTokenBytes * 2;

} // namespace

SessionManager::SessionManager(std::chrono::seconds ttl, NowFn now)
    : ttl_{ttl}, now_{std::move(now)}
{
}

Session SessionManager::create(CustomerId customer_id, Role role)
{
    std::lock_guard<std::mutex> guard{mutex_};

    std::string token = generate_token();
    while (sessions_.contains(token)) {
        token = generate_token();
    }

    Session session{
        std::move(token),
        customer_id,
        role,
        SessionState::Authenticated,
        now_() + ttl_,
    };
    sessions_.emplace(session.token, session);
    return session;
}

std::optional<Session> SessionManager::validate(std::string_view token)
{
    std::lock_guard<std::mutex> guard{mutex_};

    const std::string key{token};
    const auto it = sessions_.find(key);
    if (it == sessions_.end()) {
        return std::nullopt;
    }
    if (now_() >= it->second.expires_at) {
        sessions_.erase(it);
        return std::nullopt;
    }
    return it->second;
}

std::optional<Session> SessionManager::revoke(std::string_view token)
{
    std::lock_guard<std::mutex> guard{mutex_};

    const std::string key{token};
    const auto it = sessions_.find(key);
    if (it == sessions_.end()) {
        return std::nullopt;
    }

    Session revoked = it->second;
    sessions_.erase(it);
    revoked.state = SessionState::Revoked;
    return revoked;
}

std::size_t SessionManager::size() const
{
    std::lock_guard<std::mutex> guard{mutex_};
    return sessions_.size();
}

std::string SessionManager::generate_token()
{
    static constexpr char kHex[] = "0123456789abcdef";
    static thread_local std::random_device device;
    static thread_local std::uniform_int_distribution<unsigned> byte_dist{0U, 255U};

    std::string token;
    token.reserve(kTokenHexChars);
    for (std::size_t i = 0; i < kTokenBytes; ++i) {
        const unsigned byte = byte_dist(device);
        token.push_back(kHex[byte >> 4U]);
        token.push_back(kHex[byte & 0xFU]);
    }
    return token;
}

} // namespace bank
