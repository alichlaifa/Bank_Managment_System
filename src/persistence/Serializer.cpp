#include "persistence/Serializer.h"

#include "domain/Types.h"

#include <optional>
#include <string>

namespace bank::codec {
namespace {

using json::Value;

constexpr const char* kId      = "id";
constexpr const char* kTs      = "ts";
constexpr const char* kType    = "type";
constexpr const char* kState   = "state";
constexpr const char* kFrom    = "from";
constexpr const char* kTo      = "to";
constexpr const char* kAmount  = "amount";
constexpr const char* kOwner   = "owner";
constexpr const char* kBalance = "balance";
constexpr const char* kStatus  = "status";
constexpr const char* kVersion = "version";

} // namespace

json::Value transaction_to_json(const TransactionRecord& record)
{
    Value obj = Value::object();
    obj.set(kId, Value::integer(static_cast<int64_t>(record.id.value())));
    obj.set(kTs, Value::integer(timeutil::to_unix_ms(record.timestamp)));
    obj.set(kType, Value::string(std::string(to_string(record.type))));
    obj.set(kState, Value::string(std::string(to_string(record.state))));

    const auto account_id = [](const std::optional<AccountId>& id) {
        return id.has_value() ? Value::integer(static_cast<int64_t>(id->value())) : Value::null();
    };
    obj.set(kFrom, account_id(record.from));
    obj.set(kTo, account_id(record.to));
    obj.set(kAmount, Value::integer(record.amount.cents()));
    return obj;
}

json::Value account_to_json(const Account& account)
{
    Value obj = Value::object();
    obj.set(kId, Value::integer(static_cast<int64_t>(account.id().value())));
    obj.set(kOwner, Value::integer(static_cast<int64_t>(account.owner_id().value())));
    obj.set(kBalance, Value::integer(account.balance().cents()));
    obj.set(kStatus, Value::string(std::string(to_string(account.status()))));
    obj.set(kVersion, Value::integer(static_cast<int64_t>(account.version())));
    return obj;
}

std::optional<TransactionRecord> transaction_from_json(const json::Value& value)
{
    if (value.type() != json::Type::Object) return std::nullopt;

    const auto int_of = [&value](const char* key) -> std::optional<int64_t> {
        const Value* v = value.find(key);
        return (v != nullptr) ? v->as_integer() : std::nullopt;
    };
    const auto str_of = [&value](const char* key) -> std::optional<std::string> {
        const Value* v = value.find(key);
        if (v == nullptr) return std::nullopt;
        if (auto s = v->as_string()) return std::string{*s};
        return std::nullopt;
    };
    // optional<optional<Id>>: a missing key yields nullopt, while a key
    // present with JSON null yields an engaged optional holding an empty
    // one. That distinction separates "absent" from "explicitly null".
    const auto opt_id_of = [&value](const char* key) -> std::optional<std::optional<AccountId>> {
        const Value* v = value.find(key);
        if (v == nullptr) return std::nullopt;
        if (v->type() == json::Type::Null) return std::optional<AccountId>{};
        const auto i = v->as_integer();
        if (!i) return std::nullopt;
        if (*i < 0) return std::nullopt;
        return std::optional<AccountId>{AccountId{static_cast<uint64_t>(*i)}};
    };

    const auto id     = int_of(kId);
    const auto ts     = int_of(kTs);
    const auto type_s = str_of(kType);
    const auto state_s = str_of(kState);
    const auto from   = opt_id_of(kFrom);
    const auto to     = opt_id_of(kTo);
    const auto amount = int_of(kAmount);
    if (!id || !ts || !type_s || !state_s || !from || !to || !amount || *amount < 0) {
        return std::nullopt;
    }

    const auto type  = transaction_type_from_string(*type_s);
    const auto state = transaction_state_from_string(*state_s);
    if (!type || !state) return std::nullopt;

    TransactionRecord record;
    record.id = TransactionId{static_cast<uint64_t>(*id)};
    record.timestamp = timeutil::from_unix_ms(*ts);
    record.type = *type;
    record.state = *state;
    record.from = *from;
    record.to = *to;
    record.amount = Money{*amount};
    return record;
}

std::optional<Account> account_from_json(const json::Value& value)
{
    if (value.type() != json::Type::Object) return std::nullopt;

    const auto int_of = [&value](const char* key) -> std::optional<int64_t> {
        const Value* v = value.find(key);
        return (v != nullptr) ? v->as_integer() : std::nullopt;
    };
    const auto str_of = [&value](const char* key) -> std::optional<std::string> {
        const Value* v = value.find(key);
        if (v == nullptr) return std::nullopt;
        if (auto s = v->as_string()) return std::string{*s};
        return std::nullopt;
    };

    const auto id       = int_of(kId);
    const auto owner    = int_of(kOwner);
    const auto balance  = int_of(kBalance);
    const auto status_s = str_of(kStatus);
    const auto version  = int_of(kVersion);
    if (!id || !owner || !balance || !status_s || !version) return std::nullopt;
    if (*id < 0 || *owner < 0 || *balance < 0 || *version < 0) return std::nullopt;

    const auto status = account_status_from_string(*status_s);
    if (!status) return std::nullopt;

    return Account{AccountId{static_cast<uint64_t>(*id)},
                   CustomerId{static_cast<uint64_t>(*owner)},
                   Money{*balance},
                   *status,
                   static_cast<uint64_t>(*version)};
}

std::string serialize(const TransactionRecord& record)
{
    return json::dump(transaction_to_json(record));
}

std::optional<TransactionRecord> deserialize_transaction(std::string_view line)
{
    auto value = json::parse(line);
    if (!value) return std::nullopt;
    return transaction_from_json(*value);
}

std::string serialize(const Account& account)
{
    return json::dump(account_to_json(account));
}

std::optional<Account> deserialize_account(std::string_view line)
{
    auto value = json::parse(line);
    if (!value) return std::nullopt;
    return account_from_json(*value);
}

} // namespace bank::codec