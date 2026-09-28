// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Checked 64-bit integer arithmetic. Authoritative accounting arithmetic never
// wraps and never silently clamps: every operation reports overflow so the
// caller can refuse the request.

#ifndef ENERGY_LEDGER_CHECKED_HPP
#define ENERGY_LEDGER_CHECKED_HPP

#include <cstdint>
#include <limits>

namespace energy_ledger {
namespace checked {

/// Two's-complement magnitude of x as an unsigned value (well defined for
/// INT64_MIN, whose magnitude is representable as uint64_t).
inline std::uint64_t magnitude(std::int64_t x) noexcept {
  const std::uint64_t raw = static_cast<std::uint64_t>(x);
  return x < 0 ? (0ULL - raw) : raw;
}

inline bool add(std::int64_t a, std::int64_t b, std::int64_t& out) noexcept {
  const std::uint64_t sum = static_cast<std::uint64_t>(a) + static_cast<std::uint64_t>(b);
  out = static_cast<std::int64_t>(sum);
  const bool same_sign = (a >= 0) == (b >= 0);
  return !same_sign || ((out >= 0) == (a >= 0));
}

inline bool sub(std::int64_t a, std::int64_t b, std::int64_t& out) noexcept {
  const std::uint64_t diff = static_cast<std::uint64_t>(a) - static_cast<std::uint64_t>(b);
  out = static_cast<std::int64_t>(diff);
  const bool same_sign = (a >= 0) != (b >= 0);
  return !same_sign || ((out >= 0) == (a >= 0));
}

inline bool negate(std::int64_t a, std::int64_t& out) noexcept {
  if (a == std::numeric_limits<std::int64_t>::min()) {
    return false;
  }
  out = -a;
  return true;
}

inline bool mul(std::int64_t a, std::int64_t b, std::int64_t& out) noexcept {
  if (a == 0 || b == 0) {
    out = 0;
    return true;
  }
  const bool negative = (a < 0) != (b < 0);
  const std::uint64_t ua = magnitude(a);
  const std::uint64_t ub = magnitude(b);
  if (ub > std::numeric_limits<std::uint64_t>::max() / ua) {
    return false;
  }
  const std::uint64_t product = ua * ub;
  const std::uint64_t limit = negative ? (std::uint64_t{1} << 63)
                                       : (std::numeric_limits<std::uint64_t>::max() >> 1);
  if (product > limit) {
    return false;
  }
  if (negative) {
    out = (product == (std::uint64_t{1} << 63))
              ? std::numeric_limits<std::int64_t>::min()
              : -static_cast<std::int64_t>(product);
  } else {
    out = static_cast<std::int64_t>(product);
  }
  return true;
}

/// Exact division: succeeds only when the quotient is representable and the
/// division has no remainder.
inline bool div_exact(std::int64_t value, std::int64_t divisor, std::int64_t& out) noexcept {
  if (divisor == 0) {
    return false;
  }
  if (value == std::numeric_limits<std::int64_t>::min() && divisor == -1) {
    return false;
  }
  if (value % divisor != 0) {
    return false;
  }
  out = value / divisor;
  return true;
}

}  // namespace checked
}  // namespace energy_ledger

#endif  // ENERGY_LEDGER_CHECKED_HPP
