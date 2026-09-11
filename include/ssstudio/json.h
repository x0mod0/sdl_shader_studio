// A small JSON reader.
//
// Hand-rolled rather than brought in, for the same reason the writer in
// reflection.cpp is: the core has no JSON dependency and one file that needs to
// read a scene description is not enough reason to acquire one. It parses what
// the format actually is - objects, arrays, strings with escapes, numbers,
// booleans and null - and nothing it does not need.
//
// Reading only. Nothing here writes JSON.
#ifndef SSSTUDIO_JSON_H
#define SSSTUDIO_JSON_H

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ssstudio {

/// One JSON value.
///
/// Held by value with children in vectors and maps rather than by pointer, so a
/// document owns itself and freeing it is not a traversal. Depth is bounded at
/// parse time, so a deeply nested document cannot turn destruction into a stack
/// overflow either.
class JsonValue {
public:
    enum class Kind : std::uint8_t { Null, Bool, Number, String, Array, Object };

    JsonValue() = default;

    Kind kind() const { return kind_; }
    bool is_null() const { return kind_ == Kind::Null; }
    bool is_object() const { return kind_ == Kind::Object; }
    bool is_array() const { return kind_ == Kind::Array; }

    /// Readers that never throw and never assert. A value of the wrong kind, or
    /// a key that is not there, gives the fallback - which is what reading
    /// somebody else's document calls for, since every field in it is optional
    /// as far as this code is concerned.
    bool as_bool(bool fallback = false) const;
    double as_number(double fallback = 0.0) const;
    /// A number written as a string is still read as a number here. Some
    /// exporters write booleans and numbers quoted, and refusing them would
    /// only mean the caller doing this itself at every use.
    std::string as_string(std::string_view fallback = {}) const;

    /// Elements of an array. Empty for anything else.
    const std::vector<JsonValue>& elements() const { return elements_; }

    /// A member of an object by name, or a null value when it is absent - so a
    /// missing branch reads the same as an empty one and a chain of lookups
    /// needs no checks between the links.
    const JsonValue& operator[](std::string_view key) const;

    /// True when an object actually has this key, for telling absent from empty
    /// on the rare occasion it matters.
    bool has(std::string_view key) const;

private:
    friend class JsonParser;

    Kind kind_ = Kind::Null;
    bool boolean_ = false;
    double number_ = 0.0;
    std::string text_;
    std::vector<JsonValue> elements_;
    std::map<std::string, JsonValue, std::less<>> members_;
};

/// Parses a document. Returns false and fills `out_error` with a message naming
/// the line it gave up on.
bool parse_json(std::string_view text, JsonValue& out, std::string& out_error);

}  // namespace ssstudio

#endif  // SSSTUDIO_JSON_H
