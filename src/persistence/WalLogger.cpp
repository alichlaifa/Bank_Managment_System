#include "persistence/WalLogger.h"

#include "persistence/Serializer.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace bank {
namespace {

constexpr std::size_t kIdFieldWidth  = 16;
constexpr std::size_t kStateFieldPos = kIdFieldWidth; // state byte sits right after the id
constexpr std::size_t kHeaderSize    = kIdFieldWidth + 1;

char state_byte(TransactionState state)
{
    return static_cast<char>('0' + static_cast<int>(state));
}

std::optional<TransactionState> state_from_byte(char c)
{
    const int value = c - '0';
    if (value < 0 || value > static_cast<int>(TransactionState::RolledBack)) {
        return std::nullopt;
    }
    return static_cast<TransactionState>(value);
}

std::string id_to_field(TransactionId id)
{
    char buf[kIdFieldWidth + 1];
    std::snprintf(buf, sizeof buf, "%016llx",
                  static_cast<unsigned long long>(id.value()));
    return std::string{buf, kIdFieldWidth};
}

std::optional<uint64_t> id_from_field(std::string_view field)
{
    uint64_t value = 0;
    for (const char c : field) {
        const int nibble = [](char ch) -> int {
            if (ch >= '0' && ch <= '9') return ch - '0';
            if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
            return -1;
        }(c);
        if (nibble < 0) return std::nullopt;
        value = value * 16 + static_cast<uint64_t>(nibble);
    }
    return value;
}

} // namespace

WalLogger::WalLogger(std::filesystem::path path) : path_{std::move(path)}
{
    // Ensure the file exists; a fresh journal is simply empty.
    {
        std::ofstream touch{path_, std::ios::app | std::ios::binary};
    }

    std::ifstream in{path_, std::ios::binary};
    std::stringstream buffer;
    buffer << in.rdbuf();
    const std::string data = buffer.str();

    std::size_t line_start = 0;
    std::size_t pos = data.find('\n');
    while (pos != std::string::npos) {
        const std::string_view line{data.data() + line_start, pos - line_start};
        if (line.size() >= kHeaderSize) {
            if (const auto id = id_from_field(line.substr(0, kIdFieldWidth))) {
                if (const auto state = state_from_byte(line[kStateFieldPos])) {
                    Entry parsed;
                    parsed.id = TransactionId{*id};
                    parsed.state = *state;
                    parsed.offset = line_start;
                    parsed.payload = std::string{line.substr(kHeaderSize)};
                    entries_.push_back(std::move(parsed));
                }
            }
        }
        line_start = pos + 1;
        pos = data.find('\n', line_start);
    }

    if (line_start < data.size()) {
        // A partial tail = an append that never completed. The committed prefix
        // is intact; the trailing fragment is dropped on the next write.
        drop_tail_ = true;
        valid_length_ = line_start;
        next_offset_ = line_start;
    } else {
        next_offset_ = data.size();
    }
}

void WalLogger::truncate_interrupted_tail()
{
    std::error_code ec;
    std::filesystem::resize_file(path_, valid_length_, ec);
    if (ec) {
        throw std::runtime_error{"WalLogger: tail truncation failed: " + ec.message()};
    }
    drop_tail_ = false;
    next_offset_ = valid_length_;
}

// TODO: replace flushed stream with fsync for true durability.
//       See Known Limitations in docs/DESIGN_DECISIONS.md.
void WalLogger::append(const TransactionRecord& record)
{
    if (drop_tail_) truncate_interrupted_tail();

    const std::string payload = codec::serialize(record);
    const std::string line = id_to_field(record.id) +
                             std::string(1, state_byte(record.state)) + payload + '\n';

    // A flushed stream is a durability gap, not true durability: on a power
    // failure the OS may still lose the last writes. Closing it needs an
    // open(2) descriptor and an fsync on append, which belongs with the
    // transaction processor milestone. See "Known limitations" in
    // docs/DESIGN_DECISIONS.md.
    std::ofstream out{path_, std::ios::app | std::ios::binary};
    out.write(line.data(), static_cast<std::streamsize>(line.size()));
    out.flush();
    if (!out.good()) {
        throw std::runtime_error{"WalLogger: append failed for " + path_.string()};
    }

    Entry entry;
    entry.id = record.id;
    entry.state = record.state;
    entry.offset = next_offset_;
    entry.payload = payload;
    next_offset_ += line.size();
    entries_.push_back(std::move(entry));
}

void WalLogger::mark_committed(TransactionId id)
{
    for (Entry& entry : entries_) {
        if (entry.id != id) continue;
        if (entry.state == TransactionState::Committed) return; // idempotent

        std::fstream handle{path_, std::ios::in | std::ios::out | std::ios::binary};
        handle.seekp(static_cast<std::streamoff>(entry.offset + kStateFieldPos));
        handle.put(state_byte(TransactionState::Committed));
        handle.flush();
        if (!handle.good()) {
            throw std::runtime_error{"WalLogger: commit marker write failed"};
        }

        entry.state = TransactionState::Committed;
        return;
    }
    throw std::logic_error{"WalLogger: mark_committed on unknown transaction id"};
}

std::vector<TransactionRecord> WalLogger::load_all() const
{
    std::vector<TransactionRecord> out;
    out.reserve(entries_.size());
    for (const auto& entry : entries_) {
        if (auto record = codec::deserialize_transaction(entry.payload)) {
            record->state = entry.state; // header is authoritative
            out.push_back(std::move(*record));
        }
    }
    return out;
}

std::vector<TransactionRecord> WalLogger::replay_from(std::size_t from_lsn) const
{
    if (from_lsn >= entries_.size()) return {};

    std::vector<TransactionRecord> out;
    out.reserve(entries_.size() - from_lsn);
    for (std::size_t i = from_lsn; i < entries_.size(); ++i) {
        const auto& entry = entries_[i];
        if (auto record = codec::deserialize_transaction(entry.payload)) {
            record->state = entry.state;
            out.push_back(std::move(*record));
        }
    }
    return out;
}

} // namespace bank