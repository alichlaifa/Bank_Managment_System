#pragma once

#include "domain/Entities.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <vector>

namespace bank {

// A point-in-time image of all accounts plus the WAL LSN it covers. Recovery =
// load snapshot, then replay the WAL strictly after `lsn`.
struct Snapshot {
    std::size_t lsn;
    std::vector<Account> accounts;
};

// Persists snapshots as a single JSON document. Writes go through a temp file
// followed by an atomic rename, so a crashed snapshot write can never leave a
// torn file behind.
class SnapshotStore {
public:
    explicit SnapshotStore(std::filesystem::path path);

    void store(const Snapshot& snapshot);
    [[nodiscard]] std::optional<Snapshot> load() const;
    void clear();

private:
    std::filesystem::path path_;
};

} // namespace bank