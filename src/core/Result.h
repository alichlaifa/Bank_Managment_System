#pragma once

#include <cassert>
#include <optional>
#include <string>
#include <utility>

namespace bank {

// Service-layer return type. Domain rules are enforced with exceptions at the
// value-object boundary; services translate those into Result so callers never
// handle control flow through the exception machinery.
//
// Currently uninstantiated: the services layer that consumes this does not
// exist yet, so both Result<T> and Result<void> are kept for the upcoming
// Auth/API milestone. Note the ok()/failed() asymmetry between the two
// specializations: Result<T> keys off the optional value, while Result<void>
// keys off the error string, so Result<void>::failure("") reports success.
//
// TODO: consider std::expected once all target toolchains support it (C++23).
template <typename T>
class Result {
public:
    static Result success(T value)
    {
        Result r;
        r.value_.emplace(std::move(value));
        return r;
    }

    static Result failure(std::string error)
    {
        Result r;
        r.error_ = std::move(error);
        return r;
    }

    bool ok() const noexcept { return value_.has_value(); }
    bool failed() const noexcept { return !value_.has_value(); }

    T& value() { assert(value_.has_value()); return *value_; }
    const T& value() const { assert(value_.has_value()); return *value_; }

    const std::string& error() const noexcept { return error_; }

private:
    Result() = default;

    std::optional<T> value_;
    std::string error_;
};

template <>
class Result<void> {
public:
    static Result success() { return Result{}; }
    static Result failure(std::string error)
    {
        Result r;
        r.error_ = std::move(error);
        return r;
    }

    bool ok() const noexcept { return error_.empty(); }
    bool failed() const noexcept { return !error_.empty(); }
    const std::string& error() const noexcept { return error_; }

private:
    Result() = default;
    std::string error_;
};

} // namespace bank