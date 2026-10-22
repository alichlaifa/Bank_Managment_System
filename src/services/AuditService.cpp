#include "services/AuditService.h"

#include <atomic>
#include <cstdint>
#include <exception>
#include <mutex>
#include <string>
#include <utility>

namespace bank {

namespace {

std::mutex audit_mutex;
std::atomic<uint64_t> next_audit_id{1'000'000'000'000'000ULL};

} // namespace

namespace audit_detail {

Result<void> append(AuditRepository& audit, CustomerId actor,
                    std::string action, std::string details)
{
    try {
        std::lock_guard lk{audit_mutex};
        AuditEntry entry;
        entry.id = next_audit_id.fetch_add(1);
        entry.timestamp = timeutil::now();
        entry.actor_id = actor;
        entry.action = std::move(action);
        entry.details = std::move(details);
        audit.append(entry);
        return Result<void>::success();
    } catch (const std::exception& ex) {
        return Result<void>::failure(
            std::string{"audit append failed: "} + ex.what());
    }
}

} // namespace audit_detail

AuditService::AuditService(AuditRepository& audit) : audit_{audit}
{
}

Result<std::vector<AuditEntry>>
AuditService::find(const AuditFilter& filter) const
{
    try {
        return Result<std::vector<AuditEntry>>::success(audit_.find(filter));
    } catch (const std::exception& ex) {
        return Result<std::vector<AuditEntry>>::failure(
            std::string{"audit query failed: "} + ex.what());
    }
}

Result<std::vector<AuditEntry>>
AuditService::find_by_actor(CustomerId actor) const
{
    AuditFilter filter;
    filter.actor = actor;
    return find(filter);
}

Result<std::vector<AuditEntry>>
AuditService::find_by_action(std::string action) const
{
    AuditFilter filter;
    filter.action = std::move(action);
    return find(filter);
}

Result<std::vector<AuditEntry>>
AuditService::find_since(timeutil::Clock::time_point since) const
{
    AuditFilter filter;
    filter.since = since;
    return find(filter);
}

} // namespace bank
