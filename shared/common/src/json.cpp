#include "cpm/common/json.hpp"

#include <array>
#include <cstdio>

namespace cpm {
namespace {

// Deep enough for anything this API sends or receives, shallow enough that a
// hostile document cannot walk the parser off the stack.
constexpr int kMaxDepth = 16;

void append_utf8(std::string& out, std::uint32_t code_point) {
    if (code_point < 0x80) {
        out.push_back(static_cast<char>(code_point));
    } else if (code_point < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
        out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else if (code_point < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (code_point >> 12)));
        out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (code_point >> 18)));
        out.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    }
}

class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    JsonParse run() {
        JsonParse result;
        skip_space();
        result.value = parse_value(0, result.error);
        if (!result.error.empty()) return result;
        skip_space();
        if (pos_ != text_.size()) result.error = "trailing content after the JSON value";
        return result;
    }

private:
    bool done() const { return pos_ >= text_.size(); }
    char peek() const { return text_[pos_]; }

    void skip_space() {
        while (!done() && (peek() == ' ' || peek() == '\t' || peek() == '\n' || peek() == '\r')) ++pos_;
    }

    bool literal(std::string_view word) {
        if (text_.compare(pos_, word.size(), word) != 0) return false;
        pos_ += word.size();
        return true;
    }

    JsonValue parse_value(int depth, std::string& error) {
        JsonValue value;
        if (depth > kMaxDepth) {
            error = "the JSON is nested too deeply";
            return value;
        }
        if (done()) {
            error = "the JSON ended early";
            return value;
        }
        switch (peek()) {
            case '{': return parse_object(depth, error);
            case '[': return parse_array(depth, error);
            case '"':
                value.type = JsonValue::Type::String;
                value.text = parse_string(error);
                return value;
            case 't':
                if (!literal("true")) { error = "expected true"; return value; }
                value.type = JsonValue::Type::Bool;
                value.boolean = true;
                return value;
            case 'f':
                if (!literal("false")) { error = "expected false"; return value; }
                value.type = JsonValue::Type::Bool;
                return value;
            case 'n':
                if (!literal("null")) { error = "expected null"; return value; }
                value.type = JsonValue::Type::Null;
                return value;
            default:
                value.type = JsonValue::Type::Number;
                value.text = parse_number(error);
                return value;
        }
    }

    JsonValue parse_object(int depth, std::string& error) {
        JsonValue value;
        value.type = JsonValue::Type::Object;
        ++pos_;  // '{'
        skip_space();
        if (!done() && peek() == '}') { ++pos_; return value; }
        while (true) {
            skip_space();
            if (done() || peek() != '"') { error = "expected a field name"; return value; }
            const auto name = parse_string(error);
            if (!error.empty()) return value;
            skip_space();
            if (done() || peek() != ':') { error = "expected ':' after " + name; return value; }
            ++pos_;
            skip_space();
            auto field = parse_value(depth + 1, error);
            if (!error.empty()) return value;
            value.fields.emplace_back(name, std::move(field));
            skip_space();
            if (done()) { error = "the object was never closed"; return value; }
            if (peek() == ',') { ++pos_; continue; }
            if (peek() == '}') { ++pos_; return value; }
            error = "expected ',' or '}' in the object";
            return value;
        }
    }

    JsonValue parse_array(int depth, std::string& error) {
        JsonValue value;
        value.type = JsonValue::Type::Array;
        ++pos_;  // '['
        skip_space();
        if (!done() && peek() == ']') { ++pos_; return value; }
        while (true) {
            skip_space();
            auto item = parse_value(depth + 1, error);
            if (!error.empty()) return value;
            value.items.push_back(std::move(item));
            skip_space();
            if (done()) { error = "the array was never closed"; return value; }
            if (peek() == ',') { ++pos_; continue; }
            if (peek() == ']') { ++pos_; return value; }
            error = "expected ',' or ']' in the array";
            return value;
        }
    }

    std::string parse_string(std::string& error) {
        std::string out;
        ++pos_;  // the opening quote
        while (!done()) {
            const char c = text_[pos_++];
            if (c == '"') return out;
            if (static_cast<unsigned char>(c) < 0x20) {
                error = "a control character inside a string";
                return out;
            }
            if (c != '\\') { out.push_back(c); continue; }
            if (done()) break;
            const char escape = text_[pos_++];
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
                    const auto first = parse_hex4(error);
                    if (!error.empty()) return out;
                    std::uint32_t code_point = first;
                    // A surrogate pair is two escapes for one character; a
                    // lone half would be mangled into invalid UTF-8.
                    if (first >= 0xD800 && first <= 0xDBFF) {
                        if (text_.compare(pos_, 2, "\\u") != 0) {
                            error = "a lone high surrogate in a \\u escape";
                            return out;
                        }
                        pos_ += 2;
                        const auto second = parse_hex4(error);
                        if (!error.empty()) return out;
                        if (second < 0xDC00 || second > 0xDFFF) {
                            error = "a \\u escape pair that is not a surrogate pair";
                            return out;
                        }
                        code_point = 0x10000 + ((first - 0xD800) << 10) + (second - 0xDC00);
                    } else if (first >= 0xDC00 && first <= 0xDFFF) {
                        error = "a lone low surrogate in a \\u escape";
                        return out;
                    }
                    append_utf8(out, code_point);
                    break;
                }
                default:
                    error = std::string("unknown escape \\") + escape;
                    return out;
            }
        }
        error = "the string was never closed";
        return out;
    }

    std::uint32_t parse_hex4(std::string& error) {
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            if (done()) { error = "a short \\u escape"; return 0; }
            const char c = text_[pos_++];
            value <<= 4;
            if (c >= '0' && c <= '9') value |= static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') value |= static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') value |= static_cast<std::uint32_t>(c - 'A' + 10);
            else { error = "a \\u escape that is not four hex digits"; return 0; }
        }
        return value;
    }

    // Validates the grammar and hands back the digits as written. Nothing here
    // calls strtod: what the caller does with the text is the caller's choice,
    // and in this project it is parse_scaled.
    std::string parse_number(std::string& error) {
        const std::size_t start = pos_;
        if (!done() && peek() == '-') ++pos_;
        const std::size_t digits_start = pos_;
        while (!done() && peek() >= '0' && peek() <= '9') ++pos_;
        if (pos_ == digits_start) { error = "expected a value"; return {}; }
        // Leading zeros are not JSON: 00 and 01 are typos, not numbers.
        if (text_[digits_start] == '0' && pos_ - digits_start > 1) {
            error = "a number with a leading zero";
            return {};
        }
        if (!done() && peek() == '.') {
            ++pos_;
            const std::size_t fraction_start = pos_;
            while (!done() && peek() >= '0' && peek() <= '9') ++pos_;
            if (pos_ == fraction_start) { error = "a number with no digits after the '.'"; return {}; }
        }
        if (!done() && (peek() == 'e' || peek() == 'E')) {
            ++pos_;
            if (!done() && (peek() == '+' || peek() == '-')) ++pos_;
            const std::size_t exponent_start = pos_;
            while (!done() && peek() >= '0' && peek() <= '9') ++pos_;
            if (pos_ == exponent_start) { error = "a number with no digits in the exponent"; return {}; }
        }
        return std::string(text_.substr(start, pos_ - start));
    }

    std::string_view text_;
    std::size_t pos_ = 0;
};

}  // namespace

std::string json_escape(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 2);
    for (const char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    std::array<char, 8> buffer{};
                    std::snprintf(buffer.data(), buffer.size(), "\\u%04x",
                                  static_cast<unsigned>(static_cast<unsigned char>(c)));
                    out += buffer.data();
                } else {
                    // Everything else, UTF-8 included, goes through as bytes.
                    out.push_back(c);
                }
        }
    }
    return out;
}

void JsonWriter::separate() {
    if (first_.empty()) return;
    if (first_.back()) {
        first_.back() = false;
    } else {
        out_.push_back(',');
    }
}

JsonWriter& JsonWriter::object_begin() {
    separate();
    out_.push_back('{');
    first_.push_back(true);
    return *this;
}

JsonWriter& JsonWriter::object_end() {
    if (!first_.empty()) first_.pop_back();
    out_.push_back('}');
    return *this;
}

JsonWriter& JsonWriter::array_begin() {
    separate();
    out_.push_back('[');
    first_.push_back(true);
    return *this;
}

JsonWriter& JsonWriter::array_end() {
    if (!first_.empty()) first_.pop_back();
    out_.push_back(']');
    return *this;
}

JsonWriter& JsonWriter::key(std::string_view name) {
    separate();
    out_.push_back('"');
    out_ += json_escape(name);
    out_ += "\":";
    // The value that follows is this member's, not a sibling: no comma before it.
    if (!first_.empty()) first_.back() = true;
    return *this;
}

JsonWriter& JsonWriter::string(std::string_view value) {
    separate();
    out_.push_back('"');
    out_ += json_escape(value);
    out_.push_back('"');
    return *this;
}

JsonWriter& JsonWriter::number(std::int64_t value) {
    separate();
    out_ += std::to_string(value);
    return *this;
}

JsonWriter& JsonWriter::boolean(bool value) {
    separate();
    out_ += value ? "true" : "false";
    return *this;
}

JsonWriter& JsonWriter::null() {
    separate();
    out_ += "null";
    return *this;
}

JsonWriter& JsonWriter::field(std::string_view name, std::string_view value) {
    return key(name).string(value);
}

JsonWriter& JsonWriter::field(std::string_view name, const char* value) {
    return key(name).string(value == nullptr ? std::string_view() : std::string_view(value));
}

JsonWriter& JsonWriter::field(std::string_view name, bool value) {
    return key(name).boolean(value);
}

JsonWriter& JsonWriter::field_scaled(std::string_view name, std::int64_t value, int scale) {
    return key(name).string(format_signed(value, scale));
}

JsonWriter& JsonWriter::field_scaled(std::string_view name, std::optional<std::int64_t> value,
                                     int scale) {
    key(name);
    if (!value) return null();
    return string(format_signed(*value, scale));
}

const JsonValue* JsonValue::find(std::string_view name) const {
    if (type != Type::Object) return nullptr;
    for (const auto& field : fields) {
        if (field.first == name) return &field.second;
    }
    return nullptr;
}

JsonParse json_parse(std::string_view text) {
    Parser parser(text);
    return parser.run();
}

}  // namespace cpm
