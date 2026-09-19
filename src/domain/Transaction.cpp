#include "domain/Transaction.h"
#include "domain/TransactionOps.h"

#include <stdexcept>
#include <string>
#include <type_traits>

namespace bank {
namespace {

// Formats cents as "$1234.56" without touching floating point.
std::string format_money(int64_t cents)
{
    return "$" + std::to_string(cents / 100) + "." +
           (cents % 100 < 10 ? "0" : "") + std::to_string(cents % 100);
}

} // namespace

// TODO: only Pending is emitted today. Applied/WalWritten/Failed/
//       RolledBack become reachable once the transaction processor
//       lands.
TransactionRecord project_transaction(TransactionId id, const Transaction& txn)
{
    TransactionRecord record;
    record.id = id;
    record.timestamp = timeutil::now();
    record.type = txn::type_of(txn);
    record.state = TransactionState::Pending;
    record.from = txn::debit_account(txn);
    record.to = txn::credit_account(txn);
    record.amount = txn::amount_of(txn);
    return record;
}

Transaction materialize_transaction(const TransactionRecord& record)
{
    switch (record.type) {
        case TransactionType::Deposit:
            if (!record.to.has_value()) {
                throw std::invalid_argument{"materialize: deposit record lacks a target"};
            }
            return Deposit{*record.to, record.amount};

        case TransactionType::Withdraw:
            if (!record.from.has_value()) {
                throw std::invalid_argument{"materialize: withdraw record lacks a source"};
            }
            return Withdraw{*record.from, record.amount};

        case TransactionType::Transfer:
            if (!record.from.has_value() || !record.to.has_value()) {
                throw std::invalid_argument{"materialize: transfer record lacks a leg"};
            }
            return Transfer{*record.from, *record.to, record.amount};
    }
    throw std::logic_error{"materialize: unreachable transaction type"};
}

std::string describe(const Transaction& txn)
{
    return std::visit([](const auto& x) -> std::string {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, Deposit>) {
            return "deposit account " + std::to_string(x.account_id.value()) +
                   " += " + format_money(x.amount.cents());
        } else if constexpr (std::is_same_v<T, Withdraw>) {
            return "withdraw account " + std::to_string(x.account_id.value()) +
                   " -= " + format_money(x.amount.cents());
        } else {
            return "transfer " + format_money(x.amount.cents()) + " account " +
                   std::to_string(x.from.value()) + " -> " + std::to_string(x.to.value());
        }
    }, txn);
}

} // namespace bank