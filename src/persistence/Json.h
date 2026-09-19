#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bank::json {

// A deliberately small JSON model tailored to the WAL and snapshot formats.
// Money is integer-based by design, so floats and booleans are not modelled:
// they would only be a foot-gun here. Objects keep insertion order for stable,
// diff-friendly output.
enum class Type { Null, Integer, String, Array, Object };

class Value {
public:
    Value() = default;

    static Value null() { return Value{}; }
    static Value integer(int64_t value);
    static Value string(std::string text);
    static Value array()  { Value v; v.type_ = Type::Array;  return v; }
    static Value object() { Value v; v.type_ = Type::Object; return v; }

    [[nodiscard]] Type type() const noexcept { return type_; }

    [[nodiscard]] std::optional<int64_t> as_integer() const noexcept;
    [[nodiscard]] std::optional<std::string_view> as_string() const noexcept;

    [[nodiscard]] const Value* find(std::string_view key) const noexcept;
    Value& set(std::string key, Value child);
    Value& push(Value child);

    [[nodiscard]] const std::vector<Value>& items() const noexcept { return array_; }
    [[nodiscard]] const std::vector<std::pair<std::string, Value>>& members() const noexcept { return object_; }

    [[nodiscard]] std::size_t size() const noexcept;

private:
    Type type_ = Type::Null;
    int64_t integer_ = 0;
    std::string string_;
    std::vector<Value> array_;
    std::vector<std::pair<std::string, Value>> object_;
};

// Canonical compact dump. Always ends without a trailing newline.
std::string dump(const Value& value);

// Recursive-descent parser. Returns nullopt on any malformed input; there is
// no partial-success mode, so callers can trust a returned Value entirely.
std::optional<Value> parse(std::string_view text);

} // namespace bank::json