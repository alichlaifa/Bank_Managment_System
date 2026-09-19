#pragma once

#include "domain/Transaction.h"

#include <cstddef>
#include <filesystem>
#include <vector>

namespace bank {

// Append-only transaction journal.
//
// Record framing (one line per record):
//
//   <16-digit hex tx id><1 hex digit state><JSON payload>\n
//
// The fixed-width header exists so `mark_committed` can flip the state byte in
// place without rewriting the payload. The header state is authoritative; the
// "state" field embedded in the JSON payload is informational, and reads
// always prefer the header.
//
// Append semantics are single-threaded by contract: the transaction processor
// funnels writes through its own locking. On construction the journal is read
// once into memory; a partial final line (interrupted append) is truncated on
// the next write.
class WalLogger {
public:
    explicit WalLogger(std::filesystem::path path);

    // Appends a record with the state it carries (Pending/WalWritten/...).
    void append(const TransactionRecord& record);

    // Flips the in-place state byte of the matching record to Committed.
    // Idempotent for already-committed records.
    void mark_committed(TransactionId id);

    // Number of records in the journal; equals the next LSN (log sequence
    // number) a snapshot would fence to.
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

    // All records, in append order, with header state applied on top of the
    // payload. Unparsable payloads are skipped.
    [[nodiscard]] std::vector<TransactionRecord> load_all() const;

    // Records from logical LSN `from_lsn` onward. Recovery replays these,
    // applying only the Committed ones.
    [[nodiscard]] std::vector<TransactionRecord> replay_from(std::size_t from_lsn) const;

private:
    struct Entry {
        TransactionId id;
        TransactionState state;
        std::size_t offset;   // byte offset of this line in the file
        std::string payload;  // JSON text after the header
    };

    void truncate_interrupted_tail();

    std::filesystem::path path_;
    std::vector<Entry> entries_;
    std::size_t next_offset_{0};
    bool drop_tail_{false};
    std::size_t valid_length_{0};
};

} // namespace bank