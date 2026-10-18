#include "concurrency/TransactionProcessor.h"

#include <algorithm>
#include <exception>
#include <span>
#include <stdexcept>
#include <utility>

#include "core/TimeUtil.h"
#include "domain/AuditEntry.h"
#include "domain/TransactionOps.h"

namespace bank {

TransactionProcessor::TransactionProcessor(std::size_t worker_count,
                                           AccountRepository& accounts,
                                           TransactionRepository& transactions,
                                           AuditRepository& audit,
                                           WalLogger& wal)
    : accounts_{accounts},
      transactions_{transactions},
      audit_{audit},
      wal_{wal},
      worker_count_{worker_count}
{
    // Never reuse an id that already exists in either store.
    uint64_t max_id = 0;
    for (const TransactionRecord& record : transactions_.all()) {
        max_id = std::max(max_id, record.id.value());
    }
    for (const TransactionRecord& record : wal_.load_all()) {
        max_id = std::max(max_id, record.id.value());
    }
    next_id_.store(max_id + 1);
}

TransactionProcessor::~TransactionProcessor()
{
    stop();
}

Result<TransactionId> TransactionProcessor::submit(Transaction txn)
{
    std::lock_guard<std::mutex> guard{submit_mutex_};
    if (!accepting_) {
        return Result<TransactionId>::failure("processor stopped");
    }
    const TransactionId id{next_id_.fetch_add(1)};
    {
        std::lock_guard<std::mutex> reserved_guard{reserved_mutex_};
        reserved_.push(id);
    }
    if (!queue_.enqueue(std::move(txn))) {
        // Unreachable while the processor owns the queue exclusively (it is
        // never closed); kept so a future close() path cannot corrupt the
        // reservation pairing.
        std::lock_guard<std::mutex> reserved_guard{reserved_mutex_};
        if (!reserved_.empty() && reserved_.front() == id) {
            reserved_.pop();
        }
        return Result<TransactionId>::failure("queue closed");
    }
    return Result<TransactionId>::success(id);
}

Result<TransactionRecord> TransactionProcessor::process_one(const Transaction& txn)
{
    return process_impl(txn, TransactionId{next_id_.fetch_add(1)});
}

std::optional<Result<TransactionRecord>> TransactionProcessor::process_next()
{
    std::optional<Transaction> txn = queue_.try_pop();
    if (!txn.has_value()) {
        return std::nullopt;
    }
    return process_impl(*txn, take_reserved_id());
}

std::vector<Result<TransactionRecord>> TransactionProcessor::process_until_empty()
{
    std::vector<Result<TransactionRecord>> out;
    while (auto result = process_next()) {
        out.push_back(std::move(*result));
    }
    return out;
}

void TransactionProcessor::start()
{
    std::lock_guard<std::mutex> guard{submit_mutex_};
    if (started_) {
        return;
    }
    started_ = true;
    accepting_ = true;
    shutdown_ = std::stop_source{};
    pool_ = std::make_unique<ThreadPool>(worker_count_);
    for (std::size_t i = 0; i < worker_count_; ++i) {
        const bool submitted = pool_->submit([this] { consumer_loop(); });
        (void)submitted;  // pool cannot be stopping: it was just created
    }
}

void TransactionProcessor::stop()
{
    // Must not be called from one of this processor's own consumers.
    std::lock_guard<std::mutex> guard{submit_mutex_};
    if (!started_) {
        accepting_ = false;
        return;
    }
    accepting_ = false;
    shutdown_.request_stop();
    // Join consumers while holding submit_mutex_: consumers never take it,
    // so this only serializes concurrent stop()/start() calls.
    if (pool_) {
        pool_->stop();
        pool_.reset();
    }
    started_ = false;
}

std::size_t TransactionProcessor::pending() const
{
    return queue_.size();
}

Result<TransactionRecord> TransactionProcessor::process_impl(const Transaction& txn,
                                                             TransactionId id)
{
    const std::vector<AccountId> involved = txn::involved_accounts(txn);
    if (involved.empty()) {
        append_audit(CustomerId{0}, "txn.failed", "transaction names no accounts");
        return Result<TransactionRecord>::failure("validation failed: no accounts involved");
    }

    try {
        // Acquire every account mutex in ascending id order (deadlock-free).
        std::vector<AccountId> sorted = involved;
        std::sort(sorted.begin(), sorted.end());
        std::vector<ScopedAccountLock> guards;
        guards.reserve(sorted.size());
        for (AccountId account_id : sorted) {
            guards.push_back(locks_.lock(account_id));
        }

        // Load the involved account snapshots (repo access is serialized).
        std::vector<Account> loaded;
        loaded.reserve(sorted.size());
        std::string missing_message;
        {
            std::lock_guard<std::mutex> repo_guard{repos_mutex_};
            for (AccountId account_id : sorted) {
                std::optional<Account> account = accounts_.find_by_id(account_id);
                if (!account.has_value()) {
                    missing_message =
                        "account not found: " + std::to_string(account_id.value());
                    break;
                }
                loaded.push_back(std::move(*account));
            }
        }
        if (!missing_message.empty()) {
            append_audit(CustomerId{0}, "txn.failed", missing_message);
            return Result<TransactionRecord>::failure(missing_message);
        }

        if (!txn::validate(txn, loaded)) {
            const std::string message =
                "validation failed: " + describe(txn);
            append_audit(CustomerId{0}, "txn.failed", message);
            return Result<TransactionRecord>::failure(message);
        }

        // WAL intent, then apply, then WAL commit — all inside the account
        // locks so journal order and observed state order agree.
        TransactionRecord record = project_transaction(id, txn);
        record.state = TransactionState::Validated;
        wal_.append(record);

        txn::apply(txn, loaded);
        {
            std::lock_guard<std::mutex> repo_guard{repos_mutex_};
            for (const Account& account : loaded) {
                accounts_.save(account);
            }
        }

        wal_.mark_committed(id);
        record.state = TransactionState::Committed;
        {
            std::lock_guard<std::mutex> repo_guard{repos_mutex_};
            transactions_.append(record);
        }
        append_audit(CustomerId{0}, "txn.completed", describe(txn));
        return Result<TransactionRecord>::success(std::move(record));
    } catch (const std::exception& ex) {
        std::string message = ex.what();
        if (message.empty()) {
            message = "unknown error";
        }
        append_audit(CustomerId{0}, "txn.failed", message);
        return Result<TransactionRecord>::failure(message);
    }
}

void TransactionProcessor::consumer_loop()
{
    while (true) {
        std::optional<Transaction> txn = queue_.wait_pop(shutdown_.get_token());
        if (!txn.has_value()) {
            return;  // shutdown requested and queue drained
        }
        // Errors are already recorded in the audit trail by process_impl;
        // consumers just keep draining.
        (void)process_impl(*txn, take_reserved_id());
    }
}

TransactionId TransactionProcessor::take_reserved_id()
{
    {
        std::lock_guard<std::mutex> guard{reserved_mutex_};
        if (!reserved_.empty()) {
            const TransactionId id = reserved_.front();
            reserved_.pop();
            return id;
        }
    }
    return TransactionId{next_id_.fetch_add(1)};
}

void TransactionProcessor::append_audit(CustomerId actor, std::string_view action,
                                        std::string_view details)
{
    AuditEntry entry;
    entry.id = next_audit_id_.fetch_add(1);
    entry.timestamp = timeutil::now();
    entry.actor_id = actor;
    entry.action = std::string{action};
    entry.details = std::string{details};

    std::lock_guard<std::mutex> guard{repos_mutex_};
    audit_.append(entry);
}

} // namespace bank
