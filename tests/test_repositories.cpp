#include <doctest.h>

#include "TestSupport.h"

#include "domain/Account.h"
#include "domain/AuditEntry.h"
#include "domain/Customer.h"
#include "domain/PasswordHasher.h"
#include "domain/Transaction.h"
#include "persistence/AuditFilter.h"
#include "persistence/InMemoryAccountRepository.h"
#include "persistence/InMemoryAuditRepository.h"
#include "persistence/SqliteAccountRepository.h"
#include "persistence/SqliteAuditRepository.h"
#include "persistence/SqliteCustomerRepository.h"
#include "persistence/SqliteDatabase.h"
#include "persistence/SqliteTransactionRepository.h"

using namespace bank;

TEST_CASE("InMemoryAccountRepository round-trips a save/find/remove cycle")
{
    InMemoryAccountRepository repo;
    const Account account{AccountId{1}, CustomerId{2}, Money{500}, AccountStatus::Active, 0};

    REQUIRE_FALSE(repo.find_by_id(AccountId{1}).has_value());
    repo.save(account);
    REQUIRE(repo.find_by_id(AccountId{1}).has_value());
    REQUIRE(repo.find_by_id(AccountId{1})->balance() == Money{500});
    REQUIRE(repo.all().size() == 1);

    repo.remove(AccountId{1});
    REQUIRE(repo.all().empty());
}

TEST_CASE("InMemoryAuditRepository filters on all fields")
{
    InMemoryAuditRepository repo;
    repo.append(AuditEntry{1, timeutil::now(), CustomerId{1}, "LOGIN", "ok"});
    repo.append(AuditEntry{2, timeutil::now(), CustomerId{2}, "TRANSFER", "700"});

    AuditFilter all;
    REQUIRE(repo.find(all).size() == 2);

    AuditFilter by_action;
    by_action.action = "LOGIN";
    REQUIRE(repo.find(by_action).size() == 1);

    AuditFilter by_actor;
    by_actor.actor = CustomerId{2};
    REQUIRE(repo.find(by_actor).size() == 1);
    REQUIRE(repo.find(by_actor)[0].id == 2);
}

TEST_CASE("SQLite repositories persist across separate database connections")
{
    TempDir dir{"sqlite_persist"};
    const auto db_path = dir.path / "bank.db";

    {
        SqliteDatabase db{db_path};
        SqliteCustomerRepository customers{db};
        SqliteAccountRepository accounts{db};

        customers.save(Customer{CustomerId{1}, "Ada",
                       PasswordHasher::create_with_salt("4321",
                           "00112233445566778899aabbccddeeff"),
                       Role::Teller});
        accounts.save(Account{AccountId{1}, CustomerId{1},
                       Money{1'250}, AccountStatus::Active, 0});
    }
    {
        // Fresh connection — schema and data must survive.
        SqliteDatabase db{db_path};
        SqliteCustomerRepository customers{db};
        SqliteAccountRepository accounts{db};

        REQUIRE_FALSE(customers.find_by_id(CustomerId{9}).has_value());

        const auto customer = customers.find_by_id(CustomerId{1});
        REQUIRE(customer.has_value());
        REQUIRE(customer->verify_pin("4321"));

        const auto account = accounts.find_by_id(AccountId{1});
        REQUIRE(account.has_value());
        REQUIRE(account->balance() == Money{1'250});
        REQUIRE(account->version() == 0);
        accounts.remove(AccountId{1});
        REQUIRE(accounts.all().empty());
    }
}

TEST_CASE("SQLite transaction and audit repositories store and filter correctly")
{
    TempDir dir{"sqlite_txns"};
    SqliteDatabase db{dir.path / "test.db"};

    SqliteTransactionRepository txns{db};
    TransactionRecord record;
    record.id = TransactionId{1};
    record.timestamp = timeutil::now();
    record.type = TransactionType::Deposit;
    record.state = TransactionState::Committed;
    record.from = std::nullopt;
    record.to = AccountId{4};
    record.amount = Money{250};
    txns.append(record);

    const auto found = txns.for_account(AccountId{4});
    REQUIRE(found.size() == 1);
    REQUIRE(found[0].amount == Money{250});
    REQUIRE(txns.for_account(AccountId{5}).empty());

    SqliteAuditRepository audit{db};
    audit.append(AuditEntry{1, timeutil::now(), CustomerId{3}, "LOGIN", "ok"});
    audit.append(AuditEntry{2, timeutil::now(), CustomerId{3}, "TRANSFER", "bad"});

    AuditFilter action_filter;
    action_filter.action = "LOGIN";
    REQUIRE(audit.find(action_filter).size() == 1);

    AuditFilter actor_filter;
    actor_filter.actor = CustomerId{3};
    REQUIRE(audit.find(actor_filter).size() == 2);

    AuditFilter miss_filter;
    miss_filter.actor = CustomerId{99};
    REQUIRE(audit.find(miss_filter).empty());
}