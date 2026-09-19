#pragma once

#include <chrono>
#include <cstdint>

namespace bank::timeutil {

using Clock = std::chrono::system_clock;

// Wall-clock helpers. All persisted timestamps are unix epoch milliseconds
// (int64), a representation that sorts lexicographically and survives every
// database and serialization layer we use.
inline int64_t to_unix_ms(Clock::time_point t) noexcept
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count();
}

inline Clock::time_point from_unix_ms(int64_t ms) noexcept
{
    return Clock::time_point{std::chrono::milliseconds{ms}};
}

inline Clock::time_point now() noexcept
{
    return Clock::now();
}

} // namespace bank::timeutil