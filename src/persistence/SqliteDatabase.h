#pragma once

#include <filesystem>
#include <string>

struct sqlite3;

namespace bank {

// Owns a single SQLite connection and the schema. Not copyable; the concurrency
// boundary lives above this class, so a connection is used by exactly one
// writer at a time.
class SqliteDatabase {
public:
    explicit SqliteDatabase(const std::filesystem::path& path);
    ~SqliteDatabase();

    SqliteDatabase(const SqliteDatabase&) = delete;
    SqliteDatabase& operator=(const SqliteDatabase&) = delete;

    [[nodiscard]] sqlite3* raw() const noexcept { return db_; }

private:
    void initialize_schema();

    sqlite3* db_ = nullptr;
};

} // namespace bank