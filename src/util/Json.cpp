#include "Json.h"

#include <charconv>
#include <limits>
#include <system_error>

namespace cheburnet::json {
namespace {

bool IsDigit(char c) { return c >= '0' && c <= '9'; }
bool IsHex(char c) {
    return IsDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
unsigned HexValue(char c) {
    if (IsDigit(c)) return static_cast<unsigned>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<unsigned>(c - 'a' + 10);
    return static_cast<unsigned>(c - 'A' + 10);
}

void AppendUtf8(std::string& out, unsigned codepoint) {
    if (codepoint <= 0x7Fu) {
        out.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7FFu) {
        out.push_back(static_cast<char>(0xC0u | (codepoint >> 6u)));
        out.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
    } else if (codepoint <= 0xFFFFu) {
        out.push_back(static_cast<char>(0xE0u | (codepoint >> 12u)));
        out.push_back(static_cast<char>(0x80u | ((codepoint >> 6u) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
    } else {
        out.push_back(static_cast<char>(0xF0u | (codepoint >> 18u)));
        out.push_back(static_cast<char>(0x80u | ((codepoint >> 12u) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((codepoint >> 6u) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
    }
}

class Parser {
public:
    Parser(std::string_view input, ParseOptions options) : input_(input), options_(options) {}

    ParseResult Run() {
        ParseResult result;
        if (input_.size() > options_.maxBytes) {
            result.error = "JSON exceeds byte limit";
            return result;
        }
        if (input_.size() >= 3 && static_cast<unsigned char>(input_[0]) == 0xEFu &&
            static_cast<unsigned char>(input_[1]) == 0xBBu &&
            static_cast<unsigned char>(input_[2]) == 0xBFu) {
            pos_ = 3;
        }
        SkipWhitespace();
        auto value = ParseValue(0);
        if (!value) return Failure();
        SkipWhitespace();
        if (pos_ != input_.size()) {
            SetError("trailing data after JSON value");
            return Failure();
        }
        result.ok = true;
        result.value = std::move(*value);
        return result;
    }

private:
    ParseResult Failure() const {
        ParseResult result;
        result.error = error_.empty() ? "invalid JSON" : error_;
        result.errorOffset = errorPos_;
        return result;
    }

    void SetError(std::string message) {
        if (!error_.empty()) return;
        error_ = std::move(message);
        errorPos_ = pos_;
    }

    void SkipWhitespace() {
        while (pos_ < input_.size()) {
            const char c = input_[pos_];
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
            ++pos_;
        }
    }

    std::optional<Value> ParseValue(std::size_t depth) {
        if (depth > options_.maxDepth) {
            SetError("JSON nesting limit exceeded");
            return std::nullopt;
        }
        if (++values_ > options_.maxValues) {
            SetError("JSON value count limit exceeded");
            return std::nullopt;
        }
        if (pos_ >= input_.size()) {
            SetError("unexpected end of JSON");
            return std::nullopt;
        }
        switch (input_[pos_]) {
            case '{': return ParseObject(depth);
            case '[': return ParseArray(depth);
            case '"': {
                auto string = ParseString();
                if (!string) return std::nullopt;
                return Value(std::move(*string));
            }
            case 't': return ParseLiteral("true", Value(true));
            case 'f': return ParseLiteral("false", Value(false));
            case 'n': return ParseLiteral("null", Value(nullptr));
            default:
                if (input_[pos_] == '-' || IsDigit(input_[pos_])) return ParseNumber();
                SetError("unexpected JSON token");
                return std::nullopt;
        }
    }

    std::optional<Value> ParseLiteral(std::string_view literal, Value value) {
        if (input_.substr(pos_, literal.size()) != literal) {
            SetError("invalid JSON literal");
            return std::nullopt;
        }
        pos_ += literal.size();
        return value;
    }

    std::optional<Value> ParseObject(std::size_t depth) {
        ++pos_;
        SkipWhitespace();
        Value::Object object;
        if (Consume('}')) return Value(std::move(object));
        for (;;) {
            if (pos_ >= input_.size() || input_[pos_] != '"') {
                SetError("object key must be a string");
                return std::nullopt;
            }
            auto key = ParseString();
            if (!key) return std::nullopt;
            SkipWhitespace();
            if (!Consume(':')) {
                SetError("missing colon after object key");
                return std::nullopt;
            }
            SkipWhitespace();
            auto value = ParseValue(depth + 1);
            if (!value) return std::nullopt;
            auto inserted = object.emplace(std::move(*key), std::move(*value));
            if (!inserted.second) {
                SetError("duplicate object key");
                return std::nullopt;
            }
            SkipWhitespace();
            if (Consume('}')) break;
            if (!Consume(',')) {
                SetError("missing comma in object");
                return std::nullopt;
            }
            SkipWhitespace();
        }
        return Value(std::move(object));
    }

    std::optional<Value> ParseArray(std::size_t depth) {
        ++pos_;
        SkipWhitespace();
        Value::Array array;
        if (Consume(']')) return Value(std::move(array));
        for (;;) {
            auto value = ParseValue(depth + 1);
            if (!value) return std::nullopt;
            array.push_back(std::move(*value));
            SkipWhitespace();
            if (Consume(']')) break;
            if (!Consume(',')) {
                SetError("missing comma in array");
                return std::nullopt;
            }
            SkipWhitespace();
        }
        return Value(std::move(array));
    }

    bool Consume(char c) {
        if (pos_ >= input_.size() || input_[pos_] != c) return false;
        ++pos_;
        return true;
    }

    std::optional<unsigned> ParseHex4() {
        if (input_.size() - pos_ < 4) {
            SetError("truncated unicode escape");
            return std::nullopt;
        }
        unsigned value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = input_[pos_++];
            if (!IsHex(c)) {
                SetError("invalid unicode escape");
                return std::nullopt;
            }
            value = (value << 4u) | HexValue(c);
        }
        return value;
    }

    bool CopyValidatedUtf8(std::string& out) {
        const std::size_t start = pos_;
        const unsigned char lead = static_cast<unsigned char>(input_[pos_++]);
        unsigned codepoint = 0;
        std::size_t continuation = 0;
        unsigned minimum = 0;
        if (lead >= 0xC2u && lead <= 0xDFu) {
            continuation = 1; codepoint = lead & 0x1Fu; minimum = 0x80u;
        } else if (lead >= 0xE0u && lead <= 0xEFu) {
            continuation = 2; codepoint = lead & 0x0Fu; minimum = 0x800u;
        } else if (lead >= 0xF0u && lead <= 0xF4u) {
            continuation = 3; codepoint = lead & 0x07u; minimum = 0x10000u;
        } else {
            SetError("invalid UTF-8 in JSON string");
            return false;
        }
        if (input_.size() - pos_ < continuation) {
            SetError("truncated UTF-8 in JSON string");
            return false;
        }
        for (std::size_t i = 0; i < continuation; ++i) {
            const unsigned char c = static_cast<unsigned char>(input_[pos_++]);
            if ((c & 0xC0u) != 0x80u) {
                SetError("invalid UTF-8 continuation byte");
                return false;
            }
            codepoint = (codepoint << 6u) | (c & 0x3Fu);
        }
        if (codepoint < minimum || codepoint > 0x10FFFFu ||
            (codepoint >= 0xD800u && codepoint <= 0xDFFFu)) {
            SetError("invalid UTF-8 code point");
            return false;
        }
        out.append(input_.substr(start, pos_ - start));
        return true;
    }

    std::optional<std::string> ParseString() {
        ++pos_; // opening quote
        std::string out;
        while (pos_ < input_.size()) {
            const unsigned char c = static_cast<unsigned char>(input_[pos_++]);
            if (c == static_cast<unsigned char>('"')) return out;
            if (c < 0x20u) {
                SetError("unescaped control character in JSON string");
                return std::nullopt;
            }
            if (c >= 0x80u) {
                --pos_;
                if (!CopyValidatedUtf8(out)) return std::nullopt;
                continue;
            }
            if (c != static_cast<unsigned char>('\\')) {
                out.push_back(static_cast<char>(c));
                continue;
            }
            if (pos_ >= input_.size()) {
                SetError("truncated JSON escape");
                return std::nullopt;
            }
            const char escaped = input_[pos_++];
            switch (escaped) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    auto first = ParseHex4();
                    if (!first) return std::nullopt;
                    unsigned codepoint = *first;
                    if (codepoint >= 0xD800u && codepoint <= 0xDBFFu) {
                        if (input_.size() - pos_ < 6 || input_[pos_] != '\\' ||
                            input_[pos_ + 1] != 'u') {
                            SetError("high surrogate without low surrogate");
                            return std::nullopt;
                        }
                        pos_ += 2;
                        auto second = ParseHex4();
                        if (!second) return std::nullopt;
                        if (*second < 0xDC00u || *second > 0xDFFFu) {
                            SetError("invalid low surrogate");
                            return std::nullopt;
                        }
                        codepoint = 0x10000u + ((codepoint - 0xD800u) << 10u) +
                                    (*second - 0xDC00u);
                    } else if (codepoint >= 0xDC00u && codepoint <= 0xDFFFu) {
                        SetError("unexpected low surrogate");
                        return std::nullopt;
                    }
                    AppendUtf8(out, codepoint);
                    break;
                }
                default:
                    SetError("invalid JSON escape");
                    return std::nullopt;
            }
        }
        SetError("unterminated JSON string");
        return std::nullopt;
    }

    std::optional<Value> ParseNumber() {
        const std::size_t start = pos_;
        if (Consume('-') && pos_ >= input_.size()) {
            SetError("truncated JSON number");
            return std::nullopt;
        }
        if (Consume('0')) {
            if (pos_ < input_.size() && IsDigit(input_[pos_])) {
                SetError("leading zero in JSON number");
                return std::nullopt;
            }
        } else {
            if (pos_ >= input_.size() || input_[pos_] < '1' || input_[pos_] > '9') {
                SetError("invalid JSON number");
                return std::nullopt;
            }
            while (pos_ < input_.size() && IsDigit(input_[pos_])) ++pos_;
        }
        if (Consume('.')) {
            if (pos_ >= input_.size() || !IsDigit(input_[pos_])) {
                SetError("missing digits after decimal point");
                return std::nullopt;
            }
            while (pos_ < input_.size() && IsDigit(input_[pos_])) ++pos_;
        }
        if (pos_ < input_.size() && (input_[pos_] == 'e' || input_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < input_.size() && (input_[pos_] == '+' || input_[pos_] == '-')) ++pos_;
            if (pos_ >= input_.size() || !IsDigit(input_[pos_])) {
                SetError("missing exponent digits");
                return std::nullopt;
            }
            while (pos_ < input_.size() && IsDigit(input_[pos_])) ++pos_;
        }
        return Value(Number{std::string(input_.substr(start, pos_ - start))});
    }

    std::string_view input_;
    ParseOptions     options_;
    std::size_t      pos_ = 0;
    std::size_t      values_ = 0;
    std::string      error_;
    std::size_t      errorPos_ = 0;
};

} // namespace

std::optional<std::uint64_t> Number::AsUInt64() const {
    if (raw.empty() || raw.front() == '-' || raw.find_first_of(".eE") != std::string::npos)
        return std::nullopt;
    std::uint64_t value = 0;
    const auto result = std::from_chars(raw.data(), raw.data() + raw.size(), value);
    if (result.ec != std::errc{} || result.ptr != raw.data() + raw.size()) return std::nullopt;
    return value;
}

std::optional<std::int64_t> Number::AsInt64() const {
    if (raw.empty() || raw.find_first_of(".eE") != std::string::npos) return std::nullopt;
    std::int64_t value = 0;
    const auto result = std::from_chars(raw.data(), raw.data() + raw.size(), value);
    if (result.ec != std::errc{} || result.ptr != raw.data() + raw.size()) return std::nullopt;
    return value;
}

const Value* Value::Find(std::string_view key) const {
    const Object* object = AsObject();
    if (!object) return nullptr;
    const auto it = object->find(key);
    return it == object->end() ? nullptr : &it->second;
}

ParseResult Parse(std::string_view input, const ParseOptions& options) {
    return Parser(input, options).Run();
}

std::string Quote(std::string_view input) {
    std::string out;
    out.reserve(input.size() + 2);
    out.push_back('"');
    constexpr char hex[] = "0123456789abcdef";
    for (const unsigned char c : input) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20u) {
                    out += "\\u00";
                    out.push_back(hex[c >> 4u]);
                    out.push_back(hex[c & 0x0Fu]);
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out.push_back('"');
    return out;
}

} // namespace cheburnet::json
