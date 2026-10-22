#include "TestSupport.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <vector>

using namespace bank;

namespace {

struct AuthFixture {
    InMemoryCustomerRepository customers;
    InMemoryAuditRepository audit;
    SessionManager sessions;
    AuthService auth{customers, sessions, audit};

    AuthFixture()
    {
        seed_customer(customers, CustomerId{1}, "1234", Role::Customer);
        seed_customer(customers, CustomerId{2}, "9999", Role::Teller);
    }
};

std::vector<AuditEntry> entries_for_action(const AuditRepository& audit,
                                           std::string_view action)
{
    AuditFilter filter;
    filter.action = std::string{action};
    return audit.find(filter);
}

} // namespace

TEST(AuthService, LoginWithCorrectPinReturnsSession)
{
    AuthFixture fixture;

    const auto result = fixture.auth.login(CustomerId{1}, "1234");

    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value().customer_id, CustomerId{1});
    EXPECT_EQ(result.value().role, Role::Customer);
    EXPECT_EQ(result.value().state, SessionState::Authenticated);
    const auto validated = fixture.sessions.validate(result.value().token);
    ASSERT_TRUE(validated.has_value());
    EXPECT_EQ(validated->customer_id, CustomerId{1});
}

TEST(AuthService, LoginSuccessIsAudited)
{
    AuthFixture fixture;

    ASSERT_TRUE(fixture.auth.login(CustomerId{1}, "1234").ok());

    const auto entries = entries_for_action(fixture.audit, "auth.login");
    ASSERT_EQ(entries.size(), std::size_t{1});
    EXPECT_EQ(entries.front().actor_id, CustomerId{1});
    EXPECT_EQ(entries.front().details, "success");
}

TEST(AuthService, LoginWrongPinForExistingCustomerFails)
{
    AuthFixture fixture;

    const auto result = fixture.auth.login(CustomerId{1}, "0000");

    ASSERT_TRUE(result.failed());
    EXPECT_FALSE(result.error().empty());
    EXPECT_EQ(result.error(), "invalid credentials");
    EXPECT_EQ(fixture.sessions.size(), std::size_t{0});
}

TEST(AuthService, LoginWrongPinIsAudited)
{
    AuthFixture fixture;

    ASSERT_TRUE(fixture.auth.login(CustomerId{1}, "0000").failed());

    const auto failures =
        entries_for_action(fixture.audit, "auth.login_failed");
    ASSERT_EQ(failures.size(), std::size_t{1});
    EXPECT_EQ(failures.front().actor_id, CustomerId{1});
    EXPECT_EQ(fixture.audit.find(AuditFilter{}).size(), std::size_t{1});
}

TEST(AuthService, LoginNonexistentCustomerFails)
{
    AuthFixture fixture;

    const auto result = fixture.auth.login(CustomerId{404}, "1234");

    ASSERT_TRUE(result.failed());
    EXPECT_FALSE(result.error().empty());
    EXPECT_EQ(result.error(), "invalid credentials");
    EXPECT_EQ(fixture.sessions.size(), std::size_t{0});
}

TEST(AuthService, LoginNonexistentCustomerIsAudited)
{
    AuthFixture fixture;

    ASSERT_TRUE(fixture.auth.login(CustomerId{404}, "1234").failed());

    const auto failures =
        entries_for_action(fixture.audit, "auth.login_failed");
    ASSERT_EQ(failures.size(), std::size_t{1});
    EXPECT_EQ(failures.front().actor_id, CustomerId{404});
    EXPECT_EQ(failures.front().details, "invalid credentials");
}

TEST(AuthService, BothLoginFailurePathsProduceTheSameErrorMessage)
{
    AuthFixture fixture;

    const auto wrong_pin = fixture.auth.login(CustomerId{1}, "0000");
    const auto missing = fixture.auth.login(CustomerId{404}, "0000");

    ASSERT_TRUE(wrong_pin.failed());
    ASSERT_TRUE(missing.failed());
    EXPECT_EQ(wrong_pin.error(), missing.error());

    const auto failures =
        entries_for_action(fixture.audit, "auth.login_failed");
    ASSERT_EQ(failures.size(), std::size_t{2});
    EXPECT_EQ(failures[0].action, failures[1].action);
    EXPECT_EQ(failures[0].details, failures[1].details);
}

TEST(AuthService, FailedLoginDoesNotCreateSession)
{
    AuthFixture fixture;

    ASSERT_TRUE(fixture.auth.login(CustomerId{1}, "0000").failed());

    EXPECT_EQ(fixture.sessions.size(), std::size_t{0});
    EXPECT_FALSE(fixture.sessions.validate("anything").has_value());
}

TEST(AuthService, LoginSucceedsEvenIfAuditFails)
{
    InMemoryCustomerRepository customers;
    seed_customer(customers, CustomerId{1}, "1234");
    ThrowingAuditRepository throwing_audit;
    SessionManager sessions;
    AuthService auth{customers, sessions, throwing_audit};

    const auto result = auth.login(CustomerId{1}, "1234");

    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value().customer_id, CustomerId{1});
    EXPECT_EQ(sessions.size(), std::size_t{1});
    const auto validated = sessions.validate(result.value().token);
    ASSERT_TRUE(validated.has_value());
    EXPECT_EQ(validated->state, SessionState::Authenticated);
}

TEST(AuthService, LogoutRevokesSession)
{
    AuthFixture fixture;
    const auto login = fixture.auth.login(CustomerId{1}, "1234");
    ASSERT_TRUE(login.ok());

    const auto result = fixture.auth.logout(login.value().token);

    ASSERT_TRUE(result.ok());
    EXPECT_FALSE(fixture.sessions.validate(login.value().token).has_value());
}

TEST(AuthService, LoggedOutSessionIsInvalid)
{
    AuthFixture fixture;
    const auto login = fixture.auth.login(CustomerId{1}, "1234");
    ASSERT_TRUE(login.ok());
    ASSERT_TRUE(fixture.auth.logout(login.value().token).ok());

    EXPECT_FALSE(fixture.sessions.validate(login.value().token).has_value());
    EXPECT_FALSE(fixture.sessions.revoke(login.value().token).has_value());
}

TEST(AuthService, LogoutUnknownTokenFails)
{
    AuthFixture fixture;

    const auto result = fixture.auth.logout("not-a-token");

    ASSERT_TRUE(result.failed());
    EXPECT_FALSE(result.error().empty());
    EXPECT_EQ(result.error(), "invalid session");
}

TEST(AuthService, LogoutIsAudited)
{
    AuthFixture fixture;
    const auto login = fixture.auth.login(CustomerId{2}, "9999");
    ASSERT_TRUE(login.ok());

    ASSERT_TRUE(fixture.auth.logout(login.value().token).ok());

    const auto entries = entries_for_action(fixture.audit, "auth.logout");
    ASSERT_EQ(entries.size(), std::size_t{1});
    EXPECT_EQ(entries.front().actor_id, CustomerId{2});
    EXPECT_EQ(entries.front().details, "success");
}

TEST(AuthService, SuccessfulLoginThenFailedLoginAuditCountsAreIndependent)
{
    AuthFixture fixture;

    ASSERT_TRUE(fixture.auth.login(CustomerId{1}, "1234").ok());
    ASSERT_TRUE(fixture.auth.login(CustomerId{1}, "0000").failed());

    EXPECT_EQ(entries_for_action(fixture.audit, "auth.login").size(),
              std::size_t{1});
    EXPECT_EQ(entries_for_action(fixture.audit, "auth.login_failed").size(),
              std::size_t{1});
}
