#pragma once

#include "core/TimeUtil.h"
#include "domain/Session.h"
#include "domain/Types.h"

#include <chrono>
#include <cstddef>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace bank {

class SessionManager {
public:
    using Clock = timeutil::Clock;
    using NowFn = std::function<Clock::time_point()>;

    explicit SessionManager(
        // 8 hours: chosen for the demo; production policy should be
        // configurable and likely shorter, with refresh/rotation.
        std::chrono::seconds ttl = std::chrono::hours{8},
        NowFn now = &timeutil::now);

    [[nodiscard]] Session create(CustomerId customer_id, Role role);

    [[nodiscard]] std::optional<Session> validate(std::string_view token);

    // Atomically removes a live token and returns the removed session.
    // Returns nullopt for an unknown/expired token.
    [[nodiscard]] std::optional<Session> revoke(std::string_view token);

    // Diagnostic/test helper: number of entries currently stored.
    // Expired entries are removed lazily by validate()/revoke().
    [[nodiscard]] std::size_t size() const;

private:
    [[nodiscard]] static std::string generate_token();

    mutable std::mutex mutex_;
    std::unordered_map<std::string, Session> sessions_;
    std::chrono::seconds ttl_;
    NowFn now_;
};

} // namespace bank
