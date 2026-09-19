#pragma once

#include "domain/Entities.h"
#include "domain/Transaction.h"
#include "persistence/Json.h"

#include <optional>
#include <string>
#include <string_view>

namespace bank::codec {

// One-line JSON en/decoding for WAL records and snapshots. Records are
// self-describing: unknown keys are ignored on read, and a missing key makes
// the record unreadable (nullopt) so corrupt input never silently degrades.

json::Value transaction_to_json(const TransactionRecord& record);
json::Value account_to_json(const Account& account);

std::optional<TransactionRecord> transaction_from_json(const json::Value& value);
std::optional<Account> account_from_json(const json::Value& value);

std::string serialize(const TransactionRecord& record);
std::optional<TransactionRecord> deserialize_transaction(std::string_view line);

std::string serialize(const Account& account);
std::optional<Account> deserialize_account(std::string_view line);

} // namespace bank::codec