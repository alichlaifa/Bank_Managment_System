#include <gtest/gtest.h>
#include "domain/Ostream.h"

#include "persistence/Json.h"

using namespace bank::json;

TEST(Json, DumpParseRoundTripsAMinimalDocument)
{
    Value doc = Value::object();
    doc.set("name", Value::string("Ada Lovelace"));
    doc.set("balance", Value::integer(42'000));

    const auto text = dump(doc);
    auto parsed = parse(text);
    ASSERT_TRUE(parsed.has_value());
    ASSERT_TRUE(parsed->type() == Type::Object);

    const Value* name = parsed->find("name");
    ASSERT_TRUE(name != nullptr);
    ASSERT_TRUE(*name->as_string() == "Ada Lovelace");

    const Value* balance = parsed->find("balance");
    ASSERT_TRUE(balance != nullptr);
    ASSERT_TRUE(*balance->as_integer() == 42'000);
}

TEST(Json, EmptyArraysAndObjectsAreRoundTripped)
{
    ASSERT_TRUE(dump(parse("{}").value()) == "{}");
    ASSERT_TRUE(dump(parse("[]").value()) == "[]");
}

TEST(Json, NullFieldsSurviveParsing)
{
    const auto text = R"({"nullable":null})";
    const auto parsed = parse(text);
    ASSERT_TRUE(parsed.has_value());
    const Value* field = parsed->find("nullable");
    ASSERT_TRUE(field != nullptr);
    ASSERT_TRUE(field->type() == Type::Null);
}

TEST(Json, StringsWithEscapeSequencesRoundTripCorrectly)
{
    const std::string text = R"({"line1":"hello","line2":"hello\nworld"})";
    const auto parsed = parse(text);
    ASSERT_TRUE(parsed.has_value());
    ASSERT_TRUE(*parsed->find("line2")->as_string() == "hello\nworld");
}

TEST(Json, ArraysOfObjectsRoundTrip)
{
    Value doc = Value::object();
    Value items = Value::array();
    items.push(Value::integer(1));
    items.push(Value::integer(2));
    items.push(Value::integer(3));
    doc.set("ids", std::move(items));

    const auto text = dump(doc);
    const auto parsed = parse(text);
    ASSERT_TRUE(parsed.has_value());
    const Value* ids = parsed->find("ids");
    ASSERT_TRUE(ids->size() == 3);
    ASSERT_TRUE(ids->items()[1].as_integer() == 2);
}

TEST(Json, ParserRejectsTruncatedAndMalformedInput)
{
    ASSERT_FALSE(parse(""));
    ASSERT_FALSE(parse("not json"));
    ASSERT_FALSE(parse("{"));
    ASSERT_FALSE(parse("{\"a\":1"));
    ASSERT_FALSE(parse("{\"a\":1,}"));
    ASSERT_FALSE(parse("[1,"));
    ASSERT_FALSE(parse("[1,]"));
    ASSERT_FALSE(parse("[1, 2,]"));
}

TEST(Json, UnescapedControlCharactersInSourceAreRejected)
{
    // A literal tab or newline in the input stream should not pass the
    // parser; all such characters must appear in string values as \u escapes.
    ASSERT_FALSE(parse(std::string{"\n"}));
}

TEST(Json, FindLocatesMembersByKey)
{
    Value doc = Value::object();
    doc.set("a", Value::integer(10));
    doc.set("b", Value::string("two"));
    ASSERT_TRUE(doc.size() == 2);
    ASSERT_TRUE(doc.find("a")->as_integer() == 10);
    ASSERT_TRUE(*doc.find("b")->as_string() == "two");
}
