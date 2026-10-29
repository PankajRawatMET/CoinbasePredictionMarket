#pragma once

#include "cpm/common/fixed.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

// Hand-written JSON: no third-party library enters this project for the
// browser's sake (GUI-Q2) or for REST's, and both halves are testable without
// a socket.
//
// It lives in shared/ rather than gui/ because two callers need it now: the
// GUI writes it, and the REST instrument catalog reads it. Nothing about it
// is specific to either.
//
// Scaled integers are written as JSON *strings* - "0.58", not 0.58 - because
// a JSON number is a double in every browser, and this project keeps prices
// out of doubles from the wire to the screen (design §8). The same rule runs
// the other way: the reader leaves numbers as the text it was given.
namespace cpm {

// Builds a document. The caller is responsible for balancing the containers;
// str() on an unbalanced one returns what has been written so far, which is
// what makes a half-built document visible in a test failure.
class JsonWriter {
public:
    JsonWriter& object_begin();
    JsonWriter& object_end();
    JsonWriter& array_begin();
    JsonWriter& array_end();
    JsonWriter& key(std::string_view name);

    JsonWriter& string(std::string_view value);
    JsonWriter& number(std::int64_t value);
    JsonWriter& boolean(bool value);
    JsonWriter& null();

    JsonWriter& field(std::string_view name, std::string_view value);
    JsonWriter& field(std::string_view name, const char* value);
    JsonWriter& field(std::string_view name, bool value);
    // Any integer but bool, so field("n", 3) is a number rather than true.
    template <typename T, std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>, int> = 0>
    JsonWriter& field(std::string_view name, T value) {
        return key(name).number(static_cast<std::int64_t>(value));
    }

    // A scaled integer as its formatted text. The optional form writes null
    // when there is no value: "no bid" and "a bid of zero" are different
    // answers about a book and must not render alike.
    JsonWriter& field_scaled(std::string_view name, std::int64_t value, int scale);
    JsonWriter& field_scaled(std::string_view name, std::optional<std::int64_t> value, int scale);
    JsonWriter& field_price(std::string_view name, std::optional<Price> value) {
        return field_scaled(name, value, kPriceScale);
    }
    // A price as the percentage chance it is, which is what the GUI shows:
    // the same integer with one implied decimal instead of three, so 0.599
    // becomes "59.9" and 1.000 becomes "100". Still no arithmetic, and still
    // never a double.
    JsonWriter& field_percent(std::string_view name, std::optional<Price> value) {
        return field_scaled(name, value, 1);
    }
    JsonWriter& field_qty(std::string_view name, std::optional<Qty> value) {
        return field_scaled(name, value, kQtyScale);
    }

    const std::string& str() const { return out_; }
    std::string take() { return std::move(out_); }

private:
    void separate();

    std::string out_;
    // One flag per open container: nothing written into it yet, so no comma.
    std::vector<bool> first_;
};

std::string json_escape(std::string_view text);

// The reader. It exists for one message - the create request's {kind, params}
// - so it is strict rather than generous: no trailing commas, no trailing
// content, no comments, and a depth limit, because the input arrives from a
// socket and a recursive parser without one is a stack overflow waiting for a
// bored visitor.
struct JsonValue {
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool boolean = false;
    // String: the decoded text. Number: exactly the digits that were sent,
    // unparsed, so nothing passes through a double on the way in either.
    std::string text;
    std::vector<JsonValue> items;
    std::vector<std::pair<std::string, JsonValue>> fields;

    bool is_object() const { return type == Type::Object; }
    bool is_string() const { return type == Type::String; }
    // Null for a missing name, or for a value that is not an object.
    const JsonValue* find(std::string_view name) const;
};

struct JsonParse {
    JsonValue value;
    std::string error;  // empty on success, worded for whoever sent it

    bool ok() const { return error.empty(); }
};

JsonParse json_parse(std::string_view text);

}  // namespace cpm
