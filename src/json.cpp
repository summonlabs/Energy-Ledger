// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "energy_ledger/json.hpp"
#include "energy_ledger/strong.hpp"

namespace energy_ledger {
namespace {

constexpr std::uint64_t kInt64MinMagnitude = std::uint64_t{1} << 63;
constexpr std::uint64_t kInt64MaxMagnitude = kInt64MinMagnitude - 1;

void append_utf8(std::string& out, std::uint32_t code_point) {
  if (code_point < 0x80u) {
    out.push_back(static_cast<char>(code_point));
  } else if (code_point < 0x800u) {
    out.push_back(static_cast<char>(0xC0u | (code_point >> 6)));
    out.push_back(static_cast<char>(0x80u | (code_point & 0x3Fu)));
  } else if (code_point < 0x10000u) {
    out.push_back(static_cast<char>(0xE0u | (code_point >> 12)));
    out.push_back(static_cast<char>(0x80u | ((code_point >> 6) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | (code_point & 0x3Fu)));
  } else {
    out.push_back(static_cast<char>(0xF0u | (code_point >> 18)));
    out.push_back(static_cast<char>(0x80u | ((code_point >> 12) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | ((code_point >> 6) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | (code_point & 0x3Fu)));
  }
}

class Parser {
 public:
  Parser(std::string_view text, const JsonLimits& limits) noexcept : text_(text), limits_(limits) {}

  Expected<JsonValue> run() {
    if (text_.size() > limits_.max_input_bytes) {
      return make_error(StatusCode::LimitExceeded, "JSON input exceeds the configured byte limit");
    }
    skip_whitespace();
    auto value = parse_value(0);
    if (!value) {
      return value.error();
    }
    skip_whitespace();
    if (index_ != text_.size()) {
      return make_error(StatusCode::MalformedInput, "trailing content after the JSON document");
    }
    return value;
  }

 private:
  void skip_whitespace() noexcept {
    while (index_ < text_.size()) {
      const char character = text_[index_];
      if (character == ' ' || character == '\t' || character == '\n' || character == '\r') {
        ++index_;
      } else {
        break;
      }
    }
  }

  bool consume(char expected) noexcept {
    if (index_ < text_.size() && text_[index_] == expected) {
      ++index_;
      return true;
    }
    return false;
  }

  bool match_literal(std::string_view literal) noexcept {
    if (text_.size() - index_ >= literal.size() &&
        text_.compare(index_, literal.size(), literal) == 0) {
      index_ += literal.size();
      return true;
    }
    return false;
  }

  Expected<std::uint32_t> parse_hex4() {
    if (text_.size() - index_ < 4) {
      return make_error(StatusCode::MalformedInput, "truncated \u escape");
    }
    std::uint32_t value = 0;
    for (int digit = 0; digit < 4; ++digit) {
      const char character = text_[index_ + static_cast<std::size_t>(digit)];
      std::uint32_t nibble = 0;
      if (character >= '0' && character <= '9') {
        nibble = static_cast<std::uint32_t>(character - '0');
      } else if (character >= 'a' && character <= 'f') {
        nibble = static_cast<std::uint32_t>(character - 'a' + 10);
      } else if (character >= 'A' && character <= 'F') {
        nibble = static_cast<std::uint32_t>(character - 'A' + 10);
      } else {
        return make_error(StatusCode::MalformedInput, "invalid hexadecimal digit in \u escape");
      }
      value = (value << 4) | nibble;
    }
    index_ += 4;
    return value;
  }

  Expected<std::string> parse_string() {
    if (!consume('"')) {
      return make_error(StatusCode::MalformedInput, "expected a JSON string");
    }
    std::string out;
    while (true) {
      if (index_ >= text_.size()) {
        return make_error(StatusCode::MalformedInput, "unterminated JSON string");
      }
      const char character = text_[index_++];
      if (character == '"') {
        break;
      }
      if (static_cast<unsigned char>(character) < 0x20u) {
        return make_error(StatusCode::MalformedInput,
                          "unescaped control character inside a JSON string");
      }
      if (character != '\\') {
        out.push_back(character);
      } else {
        if (index_ >= text_.size()) {
          return make_error(StatusCode::MalformedInput, "truncated escape sequence");
        }
        const char escape = text_[index_++];
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
            auto high = parse_hex4();
            if (!high) {
              return high.error();
            }
            std::uint32_t code_point = high.value();
            if (code_point >= 0xD800u && code_point <= 0xDBFFu) {
              if (text_.size() - index_ >= 2 && text_[index_] == '\\' && text_[index_ + 1] == 'u') {
                index_ += 2;
                auto low = parse_hex4();
                if (!low) {
                  return low.error();
                }
                if (low.value() < 0xDC00u || low.value() > 0xDFFFu) {
                  return make_error(StatusCode::MalformedInput, "invalid low surrogate in \\u pair");
                }
                code_point = 0x10000u + ((code_point - 0xD800u) << 10) + (low.value() - 0xDC00u);
              } else {
                return make_error(StatusCode::MalformedInput, "unpaired high surrogate in JSON string");
              }
            } else if (code_point >= 0xDC00u && code_point <= 0xDFFFu) {
              return make_error(StatusCode::MalformedInput, "unpaired low surrogate in JSON string");
            }
            append_utf8(out, code_point);
            break;
          }
          default:
            return make_error(StatusCode::MalformedInput, "invalid escape sequence in JSON string");
        }
      }
      if (out.size() > limits_.max_string_bytes) {
        return make_error(StatusCode::LimitExceeded, "JSON string exceeds the configured limit");
      }
    }
    if (!is_valid_utf8(out)) {
      return make_error(StatusCode::MalformedInput, "JSON string is not well formed UTF-8");
    }
    return out;
  }

  Expected<JsonValue> parse_number() {
    bool negative = false;
    if (index_ < text_.size() && text_[index_] == '-') {
      negative = true;
      ++index_;
    }
    if (index_ >= text_.size() || text_[index_] < '0' || text_[index_] > '9') {
      return make_error(StatusCode::MalformedInput, "invalid JSON number");
    }
    if (text_[index_] == '0' && index_ + 1 < text_.size() && text_[index_ + 1] >= '0' &&
        text_[index_ + 1] <= '9') {
      return make_error(StatusCode::MalformedInput, "leading zeros are not allowed in JSON numbers");
    }
    const std::uint64_t limit = negative ? kInt64MinMagnitude : kInt64MaxMagnitude;
    std::uint64_t magnitude = 0;
    while (index_ < text_.size() && text_[index_] >= '0' && text_[index_] <= '9') {
      const std::uint64_t digit = static_cast<std::uint64_t>(text_[index_] - '0');
      if (magnitude > limit / 10) {
        return make_error(StatusCode::ArithmeticOverflow, "JSON integer is out of range");
      }
      magnitude *= 10;
      if (magnitude > limit - digit) {
        return make_error(StatusCode::ArithmeticOverflow, "JSON integer is out of range");
      }
      magnitude += digit;
      ++index_;
    }
    if (index_ < text_.size()) {
      const char character = text_[index_];
      if (character == '.' || character == 'e' || character == 'E') {
        return make_error(StatusCode::MalformedInput,
                          "fractional and exponent notation are not supported: authoritative "
                          "quantities are integers");
      }
    }
    if (negative) {
      if (magnitude == kInt64MinMagnitude) {
        return JsonValue::integer(INT64_MIN);
      }
      return JsonValue::integer(-static_cast<std::int64_t>(magnitude));
    }
    return JsonValue::integer(static_cast<std::int64_t>(magnitude));
  }

  Expected<JsonValue> parse_array(std::size_t depth) {
    consume('[');
    JsonValue array = JsonValue::array();
    skip_whitespace();
    if (consume(']')) {
      return array;
    }
    while (true) {
      skip_whitespace();
      auto element = parse_value(depth + 1);
      if (!element) {
        return element.error();
      }
      array.push_back(std::move(element).value());
      skip_whitespace();
      if (consume(',')) {
        continue;
      }
      if (consume(']')) {
        return array;
      }
      return make_error(StatusCode::MalformedInput, "expected ',' or ']' in JSON array");
    }
  }

  Expected<JsonValue> parse_object(std::size_t depth) {
    consume('{');
    JsonValue object = JsonValue::object();
    skip_whitespace();
    if (consume('}')) {
      return object;
    }
    while (true) {
      skip_whitespace();
      auto key = parse_string();
      if (!key) {
        return key.error();
      }
      skip_whitespace();
      if (!consume(':')) {
        return make_error(StatusCode::MalformedInput, "expected ':' after a JSON object key");
      }
      skip_whitespace();
      auto value = parse_value(depth + 1);
      if (!value) {
        return value.error();
      }
      for (const auto& member : object.members()) {
        if (member.first == key.value()) {
          return make_error(StatusCode::MalformedInput,
                            "duplicate JSON object key '" + key.value() + "'");
        }
      }
      object.set(std::move(key).value(), std::move(value).value());
      skip_whitespace();
      if (consume(',')) {
        continue;
      }
      if (consume('}')) {
        return object;
      }
      return make_error(StatusCode::MalformedInput, "expected ',' or '}' in JSON object");
    }
  }

  Expected<JsonValue> parse_value(std::size_t depth) {
    if (depth > limits_.max_depth) {
      return make_error(StatusCode::LimitExceeded, "JSON nesting depth exceeds the configured limit");
    }
    if (++nodes_ > limits_.max_nodes) {
      return make_error(StatusCode::LimitExceeded, "JSON node count exceeds the configured limit");
    }
    if (index_ >= text_.size()) {
      return make_error(StatusCode::MalformedInput, "unexpected end of JSON input");
    }
    const char character = text_[index_];
    switch (character) {
      case '{': return parse_object(depth);
      case '[': return parse_array(depth);
      case '"': {
        auto text = parse_string();
        if (!text) {
          return text.error();
        }
        return JsonValue::string(std::move(text).value());
      }
      case 't':
        if (match_literal("true")) {
          return JsonValue::boolean(true);
        }
        break;
      case 'f':
        if (match_literal("false")) {
          return JsonValue::boolean(false);
        }
        break;
      case 'n':
        if (match_literal("null")) {
          return JsonValue::null();
        }
        break;
      default:
        break;
    }
    if (character == '-' || (character >= '0' && character <= '9')) {
      return parse_number();
    }
    return make_error(StatusCode::MalformedInput, "unexpected character in JSON document");
  }

  std::string_view text_;
  const JsonLimits& limits_;
  std::size_t index_ = 0;
  std::size_t nodes_ = 0;
};

}  // namespace

JsonValue JsonValue::null() { return JsonValue(); }

JsonValue JsonValue::boolean(bool value) {
  JsonValue result;
  result.type_ = Type::Boolean;
  result.boolean_ = value;
  return result;
}

JsonValue JsonValue::integer(std::int64_t value) {
  JsonValue result;
  result.type_ = Type::Integer;
  result.integer_ = value;
  return result;
}

JsonValue JsonValue::string(std::string value) {
  JsonValue result;
  result.type_ = Type::String;
  result.string_ = std::move(value);
  return result;
}

JsonValue JsonValue::array() {
  JsonValue result;
  result.type_ = Type::Array;
  return result;
}

JsonValue JsonValue::object() {
  JsonValue result;
  result.type_ = Type::Object;
  return result;
}

const JsonValue* JsonValue::find(std::string_view key) const noexcept {
  if (type_ != Type::Object) {
    return nullptr;
  }
  for (const auto& member : object_) {
    if (member.first == key) {
      return &member.second;
    }
  }
  return nullptr;
}

Expected<const JsonValue*> JsonValue::require(std::string_view key) const {
  const JsonValue* value = find(key);
  if (value == nullptr) {
    return make_error(StatusCode::MalformedInput, "missing required field '" + std::string(key) + "'");
  }
  return value;
}

Expected<std::int64_t> JsonValue::require_integer(std::string_view key) const {
  auto value = require(key);
  if (!value) {
    return value.error();
  }
  if (!value.value()->is_integer()) {
    return make_error(StatusCode::MalformedInput,
                      "field '" + std::string(key) + "' must be an integer");
  }
  return value.value()->as_integer();
}

Expected<std::string> JsonValue::require_string(std::string_view key) const {
  auto value = require(key);
  if (!value) {
    return value.error();
  }
  if (!value.value()->is_string()) {
    return make_error(StatusCode::MalformedInput,
                      "field '" + std::string(key) + "' must be a string");
  }
  return value.value()->as_string();
}

Expected<std::int64_t> JsonValue::optional_integer(std::string_view key, std::int64_t fallback) const {
  const JsonValue* value = find(key);
  if (value == nullptr || value->is_null()) {
    return fallback;
  }
  if (!value->is_integer()) {
    return make_error(StatusCode::MalformedInput,
                      "field '" + std::string(key) + "' must be an integer");
  }
  return value->as_integer();
}

std::string json_escape(std::string_view text) {
  static const char* kHex = "0123456789abcdef";
  std::string out;
  out.reserve(text.size() + 2);
  out.push_back('"');
  for (char character : text) {
    const auto byte = static_cast<unsigned char>(character);
    switch (character) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (byte < 0x20u) {
          out += "\\u00";
          out.push_back(kHex[(byte >> 4) & 0x0Fu]);
          out.push_back(kHex[byte & 0x0Fu]);
        } else {
          out.push_back(character);
        }
        break;
    }
  }
  out.push_back('"');
  return out;
}

void JsonValue::dump_to(std::string& out, int indent, int depth) const {
  const auto newline_indent = [&out, indent, depth]() {
    if (indent > 0) {
      out.push_back('\n');
      out.append(static_cast<std::size_t>(indent * depth), ' ');
    }
  };

  switch (type_) {
    case Type::Null:
      out += "null";
      return;
    case Type::Boolean:
      out += boolean_ ? "true" : "false";
      return;
    case Type::Integer:
      out += std::to_string(integer_);
      return;
    case Type::String:
      out += json_escape(string_);
      return;
    case Type::Array: {
      if (array_.empty()) {
        out += "[]";
        return;
      }
      out.push_back('[');
      bool first = true;
      for (const JsonValue& element : array_) {
        if (!first) {
          out.push_back(',');
        }
        first = false;
        newline_indent();
        element.dump_to(out, indent, depth + 1);
      }
      newline_indent();
      out.push_back(']');
      return;
    }
    case Type::Object: {
      if (object_.empty()) {
        out += "{}";
        return;
      }
      out.push_back('{');
      bool first = true;
      for (const auto& member : object_) {
        if (!first) {
          out.push_back(',');
        }
        first = false;
        newline_indent();
        out += json_escape(member.first);
        out.push_back(':');
        if (indent > 0) {
          out.push_back(' ');
        }
        member.second.dump_to(out, indent, depth + 1);
      }
      newline_indent();
      out.push_back('}');
      return;
    }
  }
}

std::string JsonValue::dump() const {
  std::string out;
  dump_to(out, 0, 0);
  return out;
}

std::string to_pretty_json(const JsonValue& value) {
  std::string out;
  value.dump_to(out, 2, 0);
  return out;
}

Expected<JsonValue> parse_json(std::string_view text, const JsonLimits& limits) {
  Parser parser(text, limits);
  return parser.run();
}

}  // namespace energy_ledger
