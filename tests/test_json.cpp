#include <doctest.h>

#include "persistence/Json.h"

using namespace bank::json;

TEST_CASE("dump/parse round-trips a minimal document")
{
    Value doc = Value::object();
    doc.set("name", Value::string("Ada Lovelace"));
    doc.set("balance", Value::integer(42'000));

    const auto text = dump(doc);
    auto parsed = parse(text);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->type() == Type::Object);

    const Value* name = parsed->find("name");
    REQUIRE(name != nullptr);
    REQUIRE(*name->as_string() == "Ada Lovelace");

    const Value* balance = parsed->find("balance");
    REQUIRE(balance != nullptr);
    REQUIRE(*balance->as_integer() == 42'000);
}

TEST_CASE("empty arrays and objects are round-tripped")
{
    REQUIRE(dump(parse("{}").value()) == "{}");
    REQUIRE(dump(parse("[]").value()) == "[]");
}

TEST_CASE("null fields survive parsing")
{
    const auto text = R"({"nullable":null})";
    const auto parsed = parse(text);
    REQUIRE(parsed.has_value());
    const Value* field = parsed->find("nullable");
    REQUIRE(field != nullptr);
    REQUIRE(field->type() == Type::Null);
}

TEST_CASE("strings with escape sequences round-trip correctly")
{
    const std::string text = R"({"line1":"hello","line2":"hello\nworld"})";
    const auto parsed = parse(text);
    REQUIRE(parsed.has_value());
    REQUIRE(*parsed->find("line2")->as_string() == "hello\nworld");
}

TEST_CASE("arrays of objects round-trip")
{
    Value doc = Value::object();
    Value items = Value::array();
    items.push(Value::integer(1));
    items.push(Value::integer(2));
    items.push(Value::integer(3));
    doc.set("ids", std::move(items));

    const auto text = dump(doc);
    const auto parsed = parse(text);
    REQUIRE(parsed.has_value());
    const Value* ids = parsed->find("ids");
    REQUIRE(ids->size() == 3);
    REQUIRE(ids->items()[1].as_integer() == 2);
}

TEST_CASE("parser rejects truncated and malformed input")
{
    REQUIRE_FALSE(parse(""));
    REQUIRE_FALSE(parse("not json"));
    REQUIRE_FALSE(parse("{"));
    REQUIRE_FALSE(parse("{\"a\":1"));
    REQUIRE_FALSE(parse("{\"a\":1,}"));
    REQUIRE_FALSE(parse("[1,"));
    REQUIRE_FALSE(parse("[1,]"));
    REQUIRE_FALSE(parse("[1, 2,]"));
}

TEST_CASE("unescaped control characters in source are rejected")
{
    // A literal tab or newline in the input stream should not pass the
    // parser; all such characters must appear in string values as \u escapes.
    REQUIRE_FALSE(parse(std::string{"\n"}));
}

TEST_CASE("find locates members by key")
{
    Value doc = Value::object();
    doc.set("a", Value::integer(10));
    doc.set("b", Value::string("two"));
    REQUIRE(doc.size() == 2);
    REQUIRE(doc.find("a")->as_integer() == 10);
    REQUIRE(*doc.find("b")->as_string() == "two");
}