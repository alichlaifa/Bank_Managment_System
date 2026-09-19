#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace bank {

// Strongly typed identifiers. Each entity gets its own id type so an AccountId
// can never be silently passed where a CustomerId is expected; mismatches are
// rejected at compile time rather than discovered at runtime.
template <typename Tag>
class Id {
public:
    constexpr Id() = default;
    constexpr explicit Id(uint64_t value) : value_{value} {}

    [[nodiscard]] constexpr uint64_t value() const noexcept { return value_; }
    // .value() is the primary accessor; the explicit conversion exists so
    // integer call sites can cast without naming the method.
    [[nodiscard]] constexpr explicit operator uint64_t() const noexcept { return value_; }

    friend constexpr bool operator==(const Id&, const Id&) = default;
    friend constexpr auto operator<=>(const Id&, const Id&) = default;

private:
    uint64_t value_{0};
};

struct CustomerTag {};
struct AccountTag {};
struct TransactionTag {};

using CustomerId    = Id<CustomerTag>;
using AccountId     = Id<AccountTag>;
using TransactionId = Id<TransactionTag>;

enum class Role             { Customer, Teller, Admin };
enum class AccountStatus    { Active, Frozen, Closed };
enum class TransactionType  { Deposit, Withdraw, Transfer };
// Do not reorder. The integer value of each member is written as the
// state byte of a WAL record header ('0' + int(state)). Reordering
// silently corrupts existing journals.
enum class TransactionState { Pending, Validated, WalWritten, Applied, Committed, Failed, RolledBack };
enum class SessionState     { Anonymous, Authenticated, Expired, Revoked };

// String forms are the canonical on-disk representation, shared by SQLite rows
// and WAL records. Keep them lowercase and stable; they are treated as schema.
inline std::string_view to_string(Role role)
{
    switch (role) {
        case Role::Customer: return "customer";
        case Role::Teller:   return "teller";
        case Role::Admin:    return "admin";
    }
    return "unknown";
}

inline std::string_view to_string(AccountStatus status)
{
    switch (status) {
        case AccountStatus::Active: return "active";
        case AccountStatus::Frozen: return "frozen";
        case AccountStatus::Closed: return "closed";
    }
    return "unknown";
}

inline std::string_view to_string(TransactionType type)
{
    switch (type) {
        case TransactionType::Deposit:  return "deposit";
        case TransactionType::Withdraw: return "withdraw";
        case TransactionType::Transfer: return "transfer";
    }
    return "unknown";
}

inline std::string_view to_string(TransactionState state)
{
    switch (state) {
        case TransactionState::Pending:     return "pending";
        case TransactionState::Validated:   return "validated";
        case TransactionState::WalWritten:  return "wal_written";
        case TransactionState::Applied:     return "applied";
        case TransactionState::Committed:   return "committed";
        case TransactionState::Failed:      return "failed";
        case TransactionState::RolledBack:  return "rolled_back";
    }
    return "unknown";
}

inline std::string_view to_string(SessionState state)
{
    switch (state) {
        case SessionState::Anonymous:     return "anonymous";
        case SessionState::Authenticated: return "authenticated";
        case SessionState::Expired:       return "expired";
        case SessionState::Revoked:       return "revoked";
    }
    return "unknown";
}

inline std::optional<Role> role_from_string(std::string_view text)
{
    if (text == "customer") return Role::Customer;
    if (text == "teller")   return Role::Teller;
    if (text == "admin")    return Role::Admin;
    return std::nullopt;
}

inline std::optional<AccountStatus> account_status_from_string(std::string_view text)
{
    if (text == "active") return AccountStatus::Active;
    if (text == "frozen") return AccountStatus::Frozen;
    if (text == "closed") return AccountStatus::Closed;
    return std::nullopt;
}

inline std::optional<TransactionType> transaction_type_from_string(std::string_view text)
{
    if (text == "deposit")  return TransactionType::Deposit;
    if (text == "withdraw") return TransactionType::Withdraw;
    if (text == "transfer") return TransactionType::Transfer;
    return std::nullopt;
}

inline std::optional<TransactionState> transaction_state_from_string(std::string_view text)
{
    if (text == "pending")     return TransactionState::Pending;
    if (text == "validated")   return TransactionState::Validated;
    if (text == "wal_written") return TransactionState::WalWritten;
    if (text == "applied")     return TransactionState::Applied;
    if (text == "committed")   return TransactionState::Committed;
    if (text == "failed")      return TransactionState::Failed;
    if (text == "rolled_back") return TransactionState::RolledBack;
    return std::nullopt;
}

inline std::optional<SessionState> session_state_from_string(std::string_view text)
{
    if (text == "anonymous")     return SessionState::Anonymous;
    if (text == "authenticated") return SessionState::Authenticated;
    if (text == "expired")       return SessionState::Expired;
    if (text == "revoked")       return SessionState::Revoked;
    return std::nullopt;
}

} // namespace bank