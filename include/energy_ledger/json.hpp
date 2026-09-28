// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Small deterministic JSON reader/writer used by the CLI and the import path.
// The reader is strict and bounded: integers only (authoritative quantities are
// never floating point), no duplicate keys, bounded depth, bounded strings.

#ifndef ENERGY_LEDGER_JSON_HPP
#define ENERGY_LEDGER_JSON_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "energy_ledger/expected.hpp"

namespace energy_ledger {

struct JsonLimits {
  std::size_t max_depth = 16;
  std::size_t max_string_bytes = 4096;
  std::size_t max_nodes = 65536;
  std::size_t max_input_bytes = 8u * 1024u * 1024u;
};

class JsonValue {
 public:
  enum class Type : std::uint8_t { Null = 0, Boolean = 1, Integer = 2, String = 3, Array = 4, Object = 5 };

  JsonValue() = default;
  static JsonValue null();
  static JsonValue boolean(bool value);
  static JsonValue integer(std::int64_t value);
  static JsonValue string(std::string value);
  static JsonValue array();
  static JsonValue object();

  Type type() const noexcept { return type_; }
  bool is_null() const noexcept { return type_ == Type::Null; }
  bool is_object() const noexcept { return type_ == Type::Object; }
  bool is_array() const noexcept { return type_ == Type::Array; }
  bool is_string() const noexcept { return type_ == Type::String; }
  bool is_integer() const noexcept { return type_ == Type::Integer; }
  bool is_boolean() const noexcept { return type_ == Type::Boolean; }

  bool as_boolean() const noexcept { return boolean_; }
  std::int64_t as_integer() const noexcept { return integer_; }
  const std::string& as_string() const noexcept { return string_; }
  const std::vector<JsonValue>& items() const noexcept { return array_; }
  const std::vector<std::pair<std::string, JsonValue>>& members() const noexcept { return object_; }

  void push_back(JsonValue value) { array_.push_back(std::move(value)); }
  void set(std::string key, JsonValue value) { object_.emplace_back(std::move(key), std::move(value)); }

  const JsonValue* find(std::string_view key) const noexcept;
  /// Required member access with explicit errors.
  Expected<const JsonValue*> require(std::string_view key) const;
  Expected<std::int64_t> require_integer(std::string_view key) const;
  Expected<std::string> require_string(std::string_view key) const;
  Expected<std::int64_t> optional_integer(std::string_view key, std::int64_t fallback) const;

  /// Canonical serialization: members in insertion order, no insignificant
  /// whitespace, deterministic escaping.
  std::string dump() const;
  void dump_to(std::string& out, int indent, int depth) const;

 private:
  Type type_ = Type::Null;
  bool boolean_ = false;
  std::int64_t integer_ = 0;
  std::string string_;
  std::vector<JsonValue> array_;
  std::vector<std::pair<std::string, JsonValue>> object_;
};

/// Strict parser. Rejects trailing content, duplicate object keys, control
/// characters inside strings, invalid escapes, non-integer numbers and any
/// input that exceeds the configured limits.
Expected<JsonValue> parse_json(std::string_view text, const JsonLimits& limits = {});

/// Deterministic pretty printer used by the CLI (2-space indent, insertion
/// order preserved).
std::string to_pretty_json(const JsonValue& value);

/// Escapes a string as a JSON string literal (including the quotes).
std::string json_escape(std::string_view text);

}  // namespace energy_ledger

#endif  // ENERGY_LEDGER_JSON_HPP
