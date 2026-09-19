#include "persistence/SnapshotStore.h"

#include "persistence/Serializer.h"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>

namespace bank {

SnapshotStore::SnapshotStore(std::filesystem::path path) : path_{std::move(path)} {}

void SnapshotStore::store(const Snapshot& snapshot)
{
    json::Value doc = json::Value::object();
    doc.set("lsn", json::Value::integer(static_cast<int64_t>(snapshot.lsn)));

    json::Value accounts = json::Value::array();
    for (const Account& account : snapshot.accounts) {
        accounts.push(codec::account_to_json(account));
    }
    doc.set("accounts", std::move(accounts));

    const std::filesystem::path tmp = path_.string() + ".tmp";
    {
        std::ofstream out{tmp, std::ios::binary | std::ios::trunc};
        out << json::dump(doc) << '\n';
        out.flush();
        if (!out.good()) {
            throw std::runtime_error{"SnapshotStore: write failed"};
        }
    }

    std::error_code ec;
    std::filesystem::rename(tmp, path_, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        throw std::runtime_error{"SnapshotStore: commit rename failed: " + ec.message()};
    }
}

std::optional<Snapshot> SnapshotStore::load() const
{
    std::ifstream in{path_, std::ios::binary};
    if (!in) return std::nullopt;

    std::stringstream buffer;
    buffer << in.rdbuf();
    auto doc = json::parse(buffer.str());
    if (!doc || doc->type() != json::Type::Object) return std::nullopt;

    const json::Value* lsn = doc->find("lsn");
    const json::Value* accounts = doc->find("accounts");
    if (lsn == nullptr || accounts == nullptr || !lsn->as_integer()) {
        return std::nullopt;
    }

    Snapshot snapshot;
    snapshot.lsn = static_cast<std::size_t>(*lsn->as_integer());

    // A corrupt account entry makes the whole snapshot untrustworthy; fail
    // loudly rather than recovering to a wrong balance.
    for (const json::Value& item : accounts->items()) {
        auto account = codec::account_from_json(item);
        if (!account) return std::nullopt;
        snapshot.accounts.push_back(std::move(*account));
    }
    return snapshot;
}

void SnapshotStore::clear()
{
    std::error_code ec;
    std::filesystem::remove(path_, ec);
}

} // namespace bank