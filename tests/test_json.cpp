// The JSON reader.
//
// It exists to read scene descriptions written by somebody else, so most of what
// matters is what it does with input it did not expect: truncated, too deep,
// quoted where a number was meant, or simply not JSON.
#include <string>

#include "ssstudio/json.h"
#include "test.h"

using namespace ssstudio;

namespace {

JsonValue parsed(const std::string& text) {
    JsonValue value;
    std::string error;
    parse_json(text, value, error);
    return value;
}

bool rejects(const std::string& text, std::string& out_error) {
    JsonValue value;
    return !parse_json(text, value, out_error);
}

}  // namespace

TEST(json_reads_the_shapes) {
    const JsonValue value = parsed(R"({
        "name": "chain",
        "count": 3,
        "ratio": -1.5e2,
        "on": true,
        "off": false,
        "nothing": null,
        "list": [1, "two", {"three": 3}]
    })");
    CHECK(value.is_object());
    CHECK_STREQ(value["name"].as_string(), "chain");
    CHECK_EQ(static_cast<int>(value["count"].as_number()), 3);
    CHECK(value["ratio"].as_number() < -149.0 && value["ratio"].as_number() > -151.0);
    CHECK(value["on"].as_bool());
    CHECK(!value["off"].as_bool(true));
    CHECK(value["nothing"].is_null());
    CHECK_EQ(value["list"].elements().size(), static_cast<std::size_t>(3));
    CHECK_EQ(static_cast<int>(value["list"].elements()[2]["three"].as_number()), 3);
}

TEST(json_missing_keys_read_as_null_rather_than_failing) {
    // A chain of lookups should need no checks between the links: a branch that
    // is not there reads the same as one that is empty.
    const JsonValue value = parsed(R"({"a": {"b": 1}})");
    CHECK(value["nope"].is_null());
    CHECK(value["nope"]["deeper"]["deeper still"].is_null());
    CHECK_EQ(value["nope"].elements().size(), static_cast<std::size_t>(0));
    CHECK(value.has("a"));
    CHECK(!value.has("nope"));
}

TEST(json_reads_a_number_or_a_boolean_that_was_quoted) {
    // Exporters write these quoted more often than you would hope, and refusing
    // them would only mean every caller doing this itself.
    const JsonValue value = parsed(R"({"vflip": "true", "channel": "2", "srgb": "false"})");
    CHECK(value["vflip"].as_bool());
    CHECK(!value["srgb"].as_bool(true));
    CHECK_EQ(static_cast<int>(value["channel"].as_number()), 2);
}

TEST(json_reads_escapes) {
    const JsonValue value = parsed(R"({"text": "a\"b\\c\ndé"})");
    const std::string text = value["text"].as_string();
    CHECK(text.find('"') != std::string::npos);
    CHECK(text.find('\\') != std::string::npos);
    CHECK(text.find('\n') != std::string::npos);
    // e-acute, as two bytes of UTF-8.
    CHECK(text.find("\xc3\xa9") != std::string::npos);
}

TEST(json_reports_the_line_it_gave_up_on) {
    std::string error;
    CHECK(rejects("{\n  \"a\": 1,\n  \"b\": \n}", error));
    CHECK(error.find("line") != std::string::npos);
}

TEST(json_refuses_what_is_not_json) {
    std::string error;
    for (const char* text : {"", "   ", "{", "[1, 2", "{\"a\" 1}", "\"unterminated",
                             "{\"a\": }", "tru", "{}extra"}) {
        error.clear();
        CHECK(rejects(text, error));
        CHECK(!error.empty());
    }
}

TEST(json_survives_being_nested_far_too_deep) {
    // The parser recurses, so a document like this is the thing standing
    // between it and the stack running out. It has to refuse, not fall over.
    std::string text;
    for (int i = 0; i < 5000; ++i) text += '[';
    for (int i = 0; i < 5000; ++i) text += ']';

    std::string error;
    CHECK(rejects(text, error));
    CHECK(error.find("deep") != std::string::npos);
}

TEST(json_reads_a_large_document) {
    std::string text = "{\"items\":[";
    for (int i = 0; i < 20000; ++i) {
        if (i) text += ',';
        text += "{\"i\":" + std::to_string(i) + ",\"s\":\"value\"}";
    }
    text += "]}";

    const JsonValue value = parsed(text);
    CHECK_EQ(value["items"].elements().size(), static_cast<std::size_t>(20000));
    CHECK_EQ(static_cast<int>(value["items"].elements().back()["i"].as_number()), 19999);
}

TEST(json_a_repeated_key_keeps_the_last) {
    const JsonValue value = parsed(R"({"a": 1, "a": 2})");
    CHECK_EQ(static_cast<int>(value["a"].as_number()), 2);
}
