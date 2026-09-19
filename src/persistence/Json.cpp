#include "persistence/Json.h"

#include <cstdio>
#include <limits>
#include <stdexcept>

namespace bank::json {

Value Value::integer(int64_t value)
{
    Value v;
    v.type_ = Type::Integer;
    v.integer_ = value;
    return v;
}

Value Value::string(std::string text)
{
    Value v;
    v.type_ = Type::String;
    v.string_ = std::move(text);
    return v;
}

std::optional<int64_t> Value::as_integer() const noexcept
{
    if (type_ != Type::Integer) return std::nullopt;
    return integer_;
}

std::optional<std::string_view> Value::as_string() const noexcept
{
    if (type_ != Type::String) return std::nullopt;
    return std::string_view{string_};
}

const Value* Value::find(std::string_view key) const noexcept
{
    for (const auto& [member_key, member] : object_) {
        if (member_key == key) return &member;
    }
    return nullptr;
}

Value& Value::set(std::string key, Value child)
{
    if (type_ != Type::Object) {
        throw std::logic_error{"Value::set on a non-object"};
    }
    for (auto& [member_key, member] : object_) {
        if (member_key == key) {
            member = std::move(child);
            return member;
        }
    }
    object_.emplace_back(std::move(key), std::move(child));
    return object_.back().second;
}

Value& Value::push(Value child)
{
    if (type_ != Type::Array) {
        throw std::logic_error{"Value::push on a non-array"};
    }
    array_.push_back(std::move(child));
    return array_.back();
}

std::size_t Value::size() const noexcept
{
    if (type_ == Type::Array) return array_.size();
    if (type_ == Type::Object) return object_.size();
    return 0;
}

namespace {

void append_escaped(std::string& out, std::string_view text)
{
    out.push_back('"');
    for (const char c : text) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[7] = {};
                    std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
    out.push_back('"');
}

void dump_into(const Value& value, std::string& out)
{
    switch (value.type()) {
        case Type::Null:
            out += "null";
            break;
        case Type::Integer:
            out += std::to_string(*value.as_integer());
            break;
        case Type::String:
            append_escaped(out, *value.as_string());
            break;
        case Type::Array: {
            out.push_back('[');
            bool first = true;
            for (const Value& item : value.items()) {
                if (!first) out.push_back(',');
                first = false;
                dump_into(item, out);
            }
            out.push_back(']');
            break;
        }
        case Type::Object: {
            out.push_back('{');
            bool first = true;
            for (const auto& [key, member] : value.members()) {
                if (!first) out.push_back(',');
                first = false;
                append_escaped(out, key);
                out.push_back(':');
                dump_into(member, out);
            }
            out.push_back('}');
            break;
        }
    }
}

void append_utf8(std::string& out, unsigned cp)
{
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

int hex_value(char c) noexcept
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool is_digit(char c) noexcept
{
    return c >= '0' && c <= '9';
}

class Parser {
public:
    explicit Parser(std::string_view text) : text_{text} {}

    std::optional<Value> run()
    {
        skip_whitespace();
        auto value = parse_value();
        if (!value) return std::nullopt;
        skip_whitespace();
        if (!text_.empty()) return std::nullopt; // trailing garbage
        return value;
    }

private:
    [[nodiscard]] bool peek(char c) const noexcept { return !text_.empty() && text_.front() == c; }

    void advance() noexcept
    {
        if (!text_.empty()) text_.remove_prefix(1);
    }

    void skip_whitespace() noexcept
    {
        while (!text_.empty() && (text_.front() == ' ' || text_.front() == '\t' ||
                                  text_.front() == '\n' || text_.front() == '\r')) {
            advance();
        }
    }

    std::optional<Value> parse_value()
    {
        if (text_.empty()) return std::nullopt;
        switch (text_.front()) {
            case '{': return parse_object();
            case '[': return parse_array();
            case '"': return parse_string();
            default:
                if (text_.front() == 'n') return parse_keyword("null", Value::null());
                if (is_digit(text_.front()) || text_.front() == '-') return parse_integer();
                return std::nullopt;
        }
    }

    std::optional<Value> parse_keyword(std::string_view keyword, Value value)
    {
        if (text_.size() < keyword.size() || text_.substr(0, keyword.size()) != keyword) {
            return std::nullopt;
        }
        text_ = text_.substr(keyword.size());
        return value;
    }

    std::optional<Value> parse_integer()
    {
        bool negative = false;
        if (peek('-')) {
            advance();
            negative = true;
        }
        if (text_.empty() || !is_digit(text_.front())) return std::nullopt;

        int64_t value = 0;
        while (!text_.empty() && is_digit(text_.front())) {
            const int64_t digit = text_.front() - '0';
            if (value > (std::numeric_limits<int64_t>::max() - digit) / 10) {
                return std::nullopt; // overflow: not a representable integer
            }
            value = value * 10 + digit;
            advance();
        }
        return Value::integer(negative ? -value : value);
    }

    std::optional<Value> parse_string()
    {
        advance(); // opening quote
        std::string out;
        while (!text_.empty()) {
            const char c = text_.front();
            advance();
            if (c == '"') return Value::string(std::move(out));

            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (text_.empty()) return std::nullopt;
            const char escaped = text_.front();
            advance();
            switch (escaped) {
                case '"':  out.push_back('"');  break;
                case '\\': out.push_back('\\'); break;
                case '/':  out.push_back('/');  break;
                case 'b':  out.push_back('\b'); break;
                case 'f':  out.push_back('\f'); break;
                case 'n':  out.push_back('\n'); break;
                case 'r':  out.push_back('\r'); break;
                case 't':  out.push_back('\t'); break;
                case 'u': {
                    if (text_.size() < 4) return std::nullopt;
                    unsigned cp = 0;
                    for (int i = 0; i < 4; ++i) {
                        const int h = hex_value(text_.front());
                        if (h < 0) return std::nullopt;
                        cp = cp * 16 + static_cast<unsigned>(h);
                        advance();
                    }
                    // Surrogate pairs are not expected: WAL payloads are UTF-8
                    // already, and \u is only emitted for control characters.
                    if (cp >= 0xD800 && cp <= 0xDFFF) return std::nullopt;
                    append_utf8(out, cp);
                    break;
                }
                default:
                    return std::nullopt;
            }
        }
        return std::nullopt; // unterminated string
    }

    std::optional<Value> parse_array()
    {
        advance(); // '['
        Value out = Value::array();
        skip_whitespace();
        if (peek(']')) {
            advance();
            return out;
        }
        for (;;) {
            skip_whitespace();
            auto item = parse_value();
            if (!item) return std::nullopt;
            out.push(std::move(*item));
            skip_whitespace();
            if (peek(',')) {
                advance();
                continue;
            }
            if (peek(']')) {
                advance();
                return out;
            }
            return std::nullopt;
        }
    }

    std::optional<Value> parse_object()
    {
        advance(); // '{'
        Value out = Value::object();
        skip_whitespace();
        if (peek('}')) {
            advance();
            return out;
        }
        for (;;) {
            skip_whitespace();
            if (!peek('"')) return std::nullopt;
            auto key = parse_string();
            if (!key || !key->as_string()) return std::nullopt;
            std::string key_text{*key->as_string()};
            skip_whitespace();
            if (!peek(':')) return std::nullopt;
            advance();
            skip_whitespace();
            auto value = parse_value();
            if (!value) return std::nullopt;
            out.set(std::move(key_text), std::move(*value));
            skip_whitespace();
            if (peek(',')) {
                advance();
                continue;
            }
            if (peek('}')) {
                advance();
                return out;
            }
            return std::nullopt;
        }
    }

    std::string_view text_;
};

} // namespace

std::string dump(const Value& value)
{
    std::string out;
    dump_into(value, out);
    return out;
}

std::optional<Value> parse(std::string_view text)
{
    return Parser{text}.run();
}

} // namespace bank::json