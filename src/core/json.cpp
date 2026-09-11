#include "ssstudio/json.h"

#include <cctype>
#include <cstdlib>

namespace ssstudio {
namespace {

/// How deep a document may nest.
///
/// The parser recurses, so this is what stands between a hostile or simply
/// silly document and the stack running out. Any real scene description is a
/// handful of levels; sixty-four is far past anything meant.
constexpr int kMaxDepth = 64;

const JsonValue& null_value() {
    static const JsonValue none;
    return none;
}

}  // namespace

bool JsonValue::as_bool(bool fallback) const {
    if (kind_ == Kind::Bool) return boolean_;
    // Exporters write booleans quoted more often than you would hope.
    if (kind_ == Kind::String) {
        if (text_ == "true") return true;
        if (text_ == "false") return false;
    }
    if (kind_ == Kind::Number) return number_ != 0.0;
    return fallback;
}

double JsonValue::as_number(double fallback) const {
    if (kind_ == Kind::Number) return number_;
    if (kind_ == Kind::String && !text_.empty()) {
        char* end = nullptr;
        const double parsed = std::strtod(text_.c_str(), &end);
        if (end != text_.c_str()) return parsed;
    }
    if (kind_ == Kind::Bool) return boolean_ ? 1.0 : 0.0;
    return fallback;
}

std::string JsonValue::as_string(std::string_view fallback) const {
    if (kind_ == Kind::String) return text_;
    return std::string(fallback);
}

const JsonValue& JsonValue::operator[](std::string_view key) const {
    if (kind_ != Kind::Object) return null_value();
    const auto it = members_.find(key);
    return it == members_.end() ? null_value() : it->second;
}

bool JsonValue::has(std::string_view key) const {
    return kind_ == Kind::Object && members_.find(key) != members_.end();
}

/// The parser proper. A class rather than free functions so the position, the
/// depth and the error travel together instead of through five parameters.
class JsonParser {
public:
    JsonParser(std::string_view text, std::string& error) : text_(text), error_(error) {}

    bool parse(JsonValue& out) {
        skip_space();
        if (!parse_value(out, 0)) return false;
        skip_space();
        if (at_ != text_.size()) {
            fail("unexpected text after the document");
            return false;
        }
        return true;
    }

private:
    bool parse_value(JsonValue& out, int depth) {
        if (depth > kMaxDepth) {
            fail("nested too deeply");
            return false;
        }
        skip_space();
        if (at_ >= text_.size()) {
            fail("the document ends where a value was expected");
            return false;
        }

        switch (text_[at_]) {
            case '{': return parse_object(out, depth);
            case '[': return parse_array(out, depth);
            case '"': {
                out.kind_ = JsonValue::Kind::String;
                return parse_string(out.text_);
            }
            case 't':
            case 'f':
            case 'n': return parse_keyword(out);
            default: return parse_number(out);
        }
    }

    bool parse_object(JsonValue& out, int depth) {
        out.kind_ = JsonValue::Kind::Object;
        ++at_;  // past '{'
        skip_space();
        if (at_ < text_.size() && text_[at_] == '}') {
            ++at_;
            return true;
        }

        for (;;) {
            skip_space();
            if (at_ >= text_.size() || text_[at_] != '"') {
                fail("expected a key");
                return false;
            }
            std::string key;
            if (!parse_string(key)) return false;

            skip_space();
            if (at_ >= text_.size() || text_[at_] != ':') {
                fail("expected ':' after key '" + key + "'");
                return false;
            }
            ++at_;

            JsonValue member;
            if (!parse_value(member, depth + 1)) return false;
            // A repeated key keeps the last, which is what every reader does.
            out.members_[std::move(key)] = std::move(member);

            skip_space();
            if (at_ < text_.size() && text_[at_] == ',') {
                ++at_;
                continue;
            }
            if (at_ < text_.size() && text_[at_] == '}') {
                ++at_;
                return true;
            }
            fail("expected ',' or '}'");
            return false;
        }
    }

    bool parse_array(JsonValue& out, int depth) {
        out.kind_ = JsonValue::Kind::Array;
        ++at_;  // past '['
        skip_space();
        if (at_ < text_.size() && text_[at_] == ']') {
            ++at_;
            return true;
        }

        for (;;) {
            JsonValue element;
            if (!parse_value(element, depth + 1)) return false;
            out.elements_.push_back(std::move(element));

            skip_space();
            if (at_ < text_.size() && text_[at_] == ',') {
                ++at_;
                continue;
            }
            if (at_ < text_.size() && text_[at_] == ']') {
                ++at_;
                return true;
            }
            fail("expected ',' or ']'");
            return false;
        }
    }

    bool parse_string(std::string& out) {
        ++at_;  // past the opening quote
        out.clear();
        while (at_ < text_.size()) {
            const char c = text_[at_++];
            if (c == '"') return true;
            if (c != '\\') {
                if (c == '\n') ++line_;
                out.push_back(c);
                continue;
            }
            if (at_ >= text_.size()) break;

            const char escape = text_[at_++];
            switch (escape) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    if (at_ + 4 > text_.size()) {
                        fail("a \\u escape ran off the end");
                        return false;
                    }
                    unsigned int code = 0;
                    for (int i = 0; i < 4; ++i) {
                        const char digit = text_[at_++];
                        code <<= 4;
                        if (digit >= '0' && digit <= '9') {
                            code |= static_cast<unsigned>(digit - '0');
                        } else if (digit >= 'a' && digit <= 'f') {
                            code |= static_cast<unsigned>(digit - 'a' + 10);
                        } else if (digit >= 'A' && digit <= 'F') {
                            code |= static_cast<unsigned>(digit - 'A' + 10);
                        } else {
                            fail("a \\u escape is not four hex digits");
                            return false;
                        }
                    }
                    // Encoded as UTF-8. Surrogate halves are passed through as
                    // themselves rather than paired: nothing read here is ever
                    // outside the basic plane, and a lone half is better kept
                    // than turned into a question mark.
                    append_utf8(out, code);
                    break;
                }
                default:
                    fail("unknown escape");
                    return false;
            }
        }
        fail("a string is not closed");
        return false;
    }

    bool parse_keyword(JsonValue& out) {
        if (text_.compare(at_, 4, "true") == 0) {
            at_ += 4;
            out.kind_ = JsonValue::Kind::Bool;
            out.boolean_ = true;
            return true;
        }
        if (text_.compare(at_, 5, "false") == 0) {
            at_ += 5;
            out.kind_ = JsonValue::Kind::Bool;
            out.boolean_ = false;
            return true;
        }
        if (text_.compare(at_, 4, "null") == 0) {
            at_ += 4;
            out.kind_ = JsonValue::Kind::Null;
            return true;
        }
        fail("not a value");
        return false;
    }

    bool parse_number(JsonValue& out) {
        const std::size_t start = at_;
        if (at_ < text_.size() && (text_[at_] == '-' || text_[at_] == '+')) ++at_;
        bool digits = false;
        while (at_ < text_.size() &&
               (std::isdigit(static_cast<unsigned char>(text_[at_])) || text_[at_] == '.' ||
                text_[at_] == 'e' || text_[at_] == 'E' ||
                ((text_[at_] == '-' || text_[at_] == '+') &&
                 (text_[at_ - 1] == 'e' || text_[at_ - 1] == 'E')))) {
            if (std::isdigit(static_cast<unsigned char>(text_[at_]))) digits = true;
            ++at_;
        }
        if (!digits) {
            fail("not a value");
            return false;
        }
        out.kind_ = JsonValue::Kind::Number;
        out.number_ = std::strtod(std::string(text_.substr(start, at_ - start)).c_str(), nullptr);
        return true;
    }

    static void append_utf8(std::string& out, unsigned int code) {
        if (code < 0x80) {
            out.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }

    void skip_space() {
        while (at_ < text_.size()) {
            const char c = text_[at_];
            if (c == '\n') ++line_;
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++at_;
                continue;
            }
            return;
        }
    }

    void fail(const std::string& message) {
        if (!error_.empty()) return;  // the first thing that went wrong is the useful one
        error_ = "line " + std::to_string(line_) + ": " + message;
    }

    std::string_view text_;
    std::string& error_;
    std::size_t at_ = 0;
    int line_ = 1;
};

bool parse_json(std::string_view text, JsonValue& out, std::string& out_error) {
    out = JsonValue{};
    out_error.clear();
    JsonParser parser(text, out_error);
    if (parser.parse(out)) return true;
    if (out_error.empty()) out_error = "could not be read as JSON";
    out = JsonValue{};
    return false;
}

}  // namespace ssstudio
