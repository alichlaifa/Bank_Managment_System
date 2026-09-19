#include "persistence/SqliteDatabase.h"

#include <sqlite3.h>

#include <stdexcept>
#include <string>

namespace bank {

namespace {

constexpr const char* kSchema =
    "CREATE TABLE IF NOT EXISTS customers ("
    "  id            INTEGER PRIMARY KEY,"
    "  name          TEXT    NOT NULL,"
    "  pin_algorithm TEXT    NOT NULL,"
    "  pin_salt      TEXT    NOT NULL,"
    "  pin_digest    TEXT    NOT NULL,"
    "  role          TEXT    NOT NULL);"

    "CREATE TABLE IF NOT EXISTS accounts ("
    "  id            INTEGER PRIMARY KEY,"
    "  owner_id      INTEGER NOT NULL REFERENCES customers(id),"
    "  balance_cents INTEGER NOT NULL,"
    "  status        TEXT    NOT NULL,"
    "  version       INTEGER NOT NULL);"

    "CREATE TABLE IF NOT EXISTS transactions ("
    "  id           INTEGER PRIMARY KEY,"
    "  timestamp_ms INTEGER NOT NULL,"
    "  type         TEXT    NOT NULL,"
    "  state        TEXT    NOT NULL,"
    "  from_id      INTEGER,"
    "  to_id        INTEGER,"
    "  amount_cents INTEGER NOT NULL);"

    "CREATE TABLE IF NOT EXISTS audit_log ("
    "  id           INTEGER PRIMARY KEY,"
    "  timestamp_ms INTEGER NOT NULL,"
    "  actor_id     INTEGER NOT NULL,"
    "  action       TEXT    NOT NULL,"
    "  details      TEXT    NOT NULL);";

} // namespace

SqliteDatabase::SqliteDatabase(const std::filesystem::path& path)
{
    const int rc = sqlite3_open_v2(path.c_str(), &db_,
                                   SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    if (rc != SQLITE_OK) {
        const std::string message = db_ != nullptr ? sqlite3_errmsg(db_) : "sqlite3_open_v2 failed";
        if (db_ != nullptr) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
        throw std::runtime_error{"cannot open database " + path.string() + ": " + message};
    }

    // WAL journaling keeps readers from blocking the single writer and gives
    // the durability guarantees this bank's checkpoint machinery needs.
    //
    // PRAGMA return codes are deliberately ignored: each can only fail in a
    // way construction cannot act on here (journal_mode=WAL is impossible on
    // read-only or in-memory databases), and an unopenable file already threw
    // above. If durability ever depends on synchronous=FULL taking effect,
    // these should become checked calls.
    sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA synchronous=FULL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA foreign_keys=ON;", nullptr, nullptr, nullptr);

    initialize_schema();
}

SqliteDatabase::~SqliteDatabase()
{
    if (db_ != nullptr) {
        sqlite3_close(db_);
    }
}

void SqliteDatabase::initialize_schema()
{
    char* error = nullptr;
    const int rc = sqlite3_exec(db_, kSchema, nullptr, nullptr, &error);
    if (rc != SQLITE_OK) {
        const std::string message = error != nullptr ? error : "unknown schema error";
        sqlite3_free(error);
        throw std::runtime_error{"schema initialization failed: " + message};
    }
}

} // namespace bank