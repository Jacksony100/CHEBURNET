#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace cheburnet::json {

struct Number {
    std::string raw;

    std::optional<std::uint64_t> AsUInt64() const;
    std::optional<std::int64_t>  AsInt64() const;
};

class Value {
public:
    using Array = std::vector<Value>;
    using Object = std::map<std::string, Value, std::less<>>;

    Value() = default;
    explicit Value(std::nullptr_t) : data_(nullptr) {}
    explicit Value(bool value) : data_(value) {}
    explicit Value(Number value) : data_(std::move(value)) {}
    explicit Value(std::string value) : data_(std::move(value)) {}
    explicit Value(Array value) : data_(std::move(value)) {}
    explicit Value(Object value) : data_(std::move(value)) {}

    bool IsNull() const { return std::holds_alternative<std::nullptr_t>(data_); }
    const bool* AsBool() const { return std::get_if<bool>(&data_); }
    const Number* AsNumber() const { return std::get_if<Number>(&data_); }
    const std::string* AsString() const { return std::get_if<std::string>(&data_); }
    const Array* AsArray() const { return std::get_if<Array>(&data_); }
    const Object* AsObject() const { return std::get_if<Object>(&data_); }

    const Value* Find(std::string_view key) const;

private:
    std::variant<std::nullptr_t, bool, Number, std::string, Array, Object> data_{nullptr};
};

struct ParseOptions {
    std::size_t maxBytes = 1024 * 1024;
    std::size_t maxDepth = 32;
    std::size_t maxValues = 8192;
};

struct ParseResult {
    bool        ok = false;
    Value       value;
    std::string error;
    std::size_t errorOffset = 0;
};

// Strict RFC 8259 parser: full nesting, duplicate-key rejection, UTF-8 and
// surrogate validation, depth/value/byte limits, and no trailing garbage.
ParseResult Parse(std::string_view input, const ParseOptions& options = {});

// Quote one UTF-8 string for deterministic JSON serialization.
std::string Quote(std::string_view input);

} // namespace cheburnet::json
