#include "TestSupport.h"

#include <gtest/gtest.h>

#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace bank;

namespace {

SessionManager make_sessions(FakeClock& clock, std::chrono::seconds ttl)
{
    return SessionManager{ttl, [&clock] { return clock.now(); }};
}

} // namespace

TEST(SessionManager, CreateProducesAuthenticatedSession)
{
    FakeClock clock;
    SessionManager sessions = make_sessions(clock, std::chrono::hours{8});

    const Session session = sessions.create(CustomerId{7}, Role::Teller);

    EXPECT_FALSE(session.token.empty());
    EXPECT_EQ(session.customer_id, CustomerId{7});
    EXPECT_EQ(session.role, Role::Teller);
    EXPECT_EQ(session.state, SessionState::Authenticated);
    EXPECT_GT(session.expires_at, clock.current);
}

TEST(SessionManager, CreateUsesInjectedTtl)
{
    FakeClock clock;
    const auto start = clock.current;
    SessionManager sessions = make_sessions(clock, std::chrono::seconds{10});

    const Session session = sessions.create(CustomerId{1}, Role::Customer);

    EXPECT_EQ(session.expires_at, start + std::chrono::seconds{10});
}

TEST(SessionManager, ValidateReturnsLiveSession)
{
    FakeClock clock;
    SessionManager sessions = make_sessions(clock, std::chrono::hours{1});

    const Session created = sessions.create(CustomerId{3}, Role::Admin);
    const auto validated = sessions.validate(created.token);

    ASSERT_TRUE(validated.has_value());
    EXPECT_EQ(validated->token, created.token);
    EXPECT_EQ(validated->customer_id, CustomerId{3});
    EXPECT_EQ(validated->role, Role::Admin);
    EXPECT_EQ(validated->state, SessionState::Authenticated);
    EXPECT_EQ(validated->expires_at, created.expires_at);
}

TEST(SessionManager, ValidateUnknownTokenReturnsNullopt)
{
    FakeClock clock;
    SessionManager sessions = make_sessions(clock, std::chrono::hours{1});

    EXPECT_FALSE(sessions.validate("not-a-real-token").has_value());
}

TEST(SessionManager, ValidateExpiredTokenReturnsNulloptAndPurges)
{
    FakeClock clock;
    SessionManager sessions = make_sessions(clock, std::chrono::seconds{5});

    const Session created = sessions.create(CustomerId{4}, Role::Customer);
    clock.advance(std::chrono::seconds{5});

    EXPECT_FALSE(sessions.validate(created.token).has_value());
    EXPECT_EQ(sessions.size(), std::size_t{0});
}

TEST(SessionManager, ValidateDoesNotExtendExpiry)
{
    FakeClock clock;
    SessionManager sessions = make_sessions(clock, std::chrono::seconds{10});

    const Session created = sessions.create(CustomerId{5}, Role::Customer);
    const auto first = sessions.validate(created.token);
    ASSERT_TRUE(first.has_value());

    clock.advance(std::chrono::seconds{4});
    const auto second = sessions.validate(created.token);
    ASSERT_TRUE(second.has_value());

    EXPECT_EQ(second->expires_at, created.expires_at);
    EXPECT_EQ(second->expires_at, first->expires_at);
}

TEST(SessionManager, RevokeInvalidatesToken)
{
    FakeClock clock;
    SessionManager sessions = make_sessions(clock, std::chrono::hours{1});

    const Session created = sessions.create(CustomerId{6}, Role::Teller);
    const auto revoked = sessions.revoke(created.token);

    ASSERT_TRUE(revoked.has_value());
    EXPECT_EQ(revoked->token, created.token);
    EXPECT_EQ(revoked->customer_id, CustomerId{6});
    EXPECT_EQ(revoked->state, SessionState::Revoked);
    EXPECT_FALSE(sessions.validate(created.token).has_value());
    EXPECT_EQ(sessions.size(), std::size_t{0});
}

TEST(SessionManager, RevokeUnknownTokenReturnsNullopt)
{
    FakeClock clock;
    SessionManager sessions = make_sessions(clock, std::chrono::hours{1});

    EXPECT_FALSE(sessions.revoke("not-a-real-token").has_value());
    EXPECT_EQ(sessions.size(), std::size_t{0});
}

TEST(SessionManager, RevokeIsIdempotent)
{
    FakeClock clock;
    SessionManager sessions = make_sessions(clock, std::chrono::hours{1});

    const Session created = sessions.create(CustomerId{8}, Role::Customer);
    ASSERT_TRUE(sessions.revoke(created.token).has_value());

    const std::size_t size_after_first = sessions.size();
    EXPECT_FALSE(sessions.revoke(created.token).has_value());
    EXPECT_EQ(sessions.size(), size_after_first);
}

TEST(SessionManager, TokensAre64LowercaseHex)
{
    FakeClock clock;
    SessionManager sessions = make_sessions(clock, std::chrono::hours{1});

    for (int i = 0; i < 64; ++i) {
        const Session session = sessions.create(CustomerId{9}, Role::Customer);
        EXPECT_EQ(session.token.size(), std::size_t{64});
        for (const char c : session.token) {
            const bool is_lower_hex =
                (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
            EXPECT_TRUE(is_lower_hex) << "unexpected token character: " << c;
        }
    }
}

TEST(SessionManager, SequentialTokensAreUnique)
{
    FakeClock clock;
    SessionManager sessions = make_sessions(clock, std::chrono::hours{1});

    std::set<std::string> unique;
    for (int i = 0; i < 1000; ++i) {
        unique.insert(sessions.create(CustomerId{1}, Role::Customer).token);
    }

    EXPECT_EQ(unique.size(), std::size_t{1000});
    EXPECT_EQ(sessions.size(), std::size_t{1000});
}

TEST(SessionManager, ConcurrentTokenCreationIsUnique)
{
    FakeClock clock;
    SessionManager sessions = make_sessions(clock, std::chrono::hours{1});

    const std::vector<std::string> tokens =
        create_tokens_concurrently(sessions, CustomerId{1}, 4, 100);
    const std::set<std::string> unique{tokens.begin(), tokens.end()};

    EXPECT_EQ(tokens.size(), std::size_t{400});
    EXPECT_EQ(unique.size(), std::size_t{400});
    EXPECT_EQ(sessions.size(), std::size_t{400});
}

TEST(SessionManager, ConcurrentValidateAndRevokeDoNotRace)
{
    FakeClock clock;
    SessionManager sessions = make_sessions(clock, std::chrono::hours{1});

    constexpr int kSessionCount = 200;
    std::vector<std::string> tokens;
    tokens.reserve(kSessionCount);
    for (int i = 0; i < kSessionCount; ++i) {
        tokens.push_back(sessions.create(CustomerId{1}, Role::Customer).token);
    }

    std::atomic<bool> mismatched{false};
    std::barrier start{5};
    std::vector<std::jthread> workers;
    workers.reserve(5);

    workers.emplace_back([&sessions, &tokens, &start] {
        start.arrive_and_wait();
        for (const std::string& token : tokens) {
            (void)sessions.revoke(token);
        }
    });

    for (int i = 0; i < 4; ++i) {
        workers.emplace_back([&sessions, &tokens, &start, &mismatched] {
            start.arrive_and_wait();
            for (const std::string& token : tokens) {
                const auto session = sessions.validate(token);
                if (session.has_value() &&
                    (session->token != token ||
                     session->customer_id != CustomerId{1})) {
                    mismatched.store(true);
                }
            }
        });
    }

    workers.clear();

    EXPECT_FALSE(mismatched.load());
    EXPECT_EQ(sessions.size(), std::size_t{0});
    for (const std::string& token : tokens) {
        EXPECT_FALSE(sessions.validate(token).has_value());
    }
}
