// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Units and dimensional types. Energy and power are distinct types: energy can
// never be produced from power without an explicit interval duration, and every
// conversion is exact-or-refused.

#ifndef ENERGY_LEDGER_UNITS_HPP
#define ENERGY_LEDGER_UNITS_HPP

#include <cstdint>
#include <string_view>

#include "energy_ledger/checked.hpp"
#include "energy_ledger/expected.hpp"
#include "energy_ledger/strong.hpp"

namespace energy_ledger {

/// Units accepted on the wire. The documented canonical unit of the ledger is
/// the joule; every quantity is stored as an exact signed 64-bit joule count.
enum class EnergyUnit : std::uint8_t {
  Joule = 1,
  Kilojoule = 2,
  Megajoule = 3,
  Gigajoule = 4,
  Millijoule = 5,
  Microjoule = 6,
  WattHour = 7,
  KilowattHour = 8,
  MegawattHour = 9,
  GigawattHour = 10,
  TerawattHour = 11,
};

const char* to_string(EnergyUnit unit) noexcept;
Expected<EnergyUnit> parse_energy_unit(std::string_view text);
bool is_valid_energy_unit(std::uint8_t value) noexcept;

/// Exact authoritative energy quantity in canonical joules.
class Energy {
 public:
  constexpr Energy() noexcept = default;

  static Expected<Energy> from_joules(std::int64_t joules);
  static constexpr Energy zero() noexcept { return Energy(0); }

  constexpr std::int64_t joules() const noexcept { return joules_; }
  constexpr bool is_zero() const noexcept { return joules_ == 0; }
  constexpr bool is_negative() const noexcept { return joules_ < 0; }

  static Expected<Energy> add(Energy a, Energy b);
  static Expected<Energy> subtract(Energy a, Energy b);
  static Expected<Energy> negate(Energy a);

  friend constexpr bool operator==(Energy a, Energy b) noexcept { return a.joules_ == b.joules_; }
  friend constexpr bool operator!=(Energy a, Energy b) noexcept { return a.joules_ != b.joules_; }
  friend constexpr bool operator<(Energy a, Energy b) noexcept { return a.joules_ < b.joules_; }
  friend constexpr bool operator>(Energy a, Energy b) noexcept { return b < a; }
  friend constexpr bool operator<=(Energy a, Energy b) noexcept { return !(b < a); }
  friend constexpr bool operator>=(Energy a, Energy b) noexcept { return !(a < b); }

 private:
  explicit constexpr Energy(std::int64_t joules) noexcept : joules_(joules) {}
  std::int64_t joules_ = 0;
};

/// Exact active power in canonical watts. Distinct from Energy by type.
class Power {
 public:
  constexpr Power() noexcept = default;

  static Expected<Power> from_watts(std::int64_t watts);
  static Expected<Power> from_milliwatts(std::int64_t milliwatts);

  constexpr std::int64_t watts() const noexcept { return watts_; }
  constexpr bool is_negative() const noexcept { return watts_ < 0; }

  friend constexpr bool operator==(Power a, Power b) noexcept { return a.watts_ == b.watts_; }
  friend constexpr bool operator!=(Power a, Power b) noexcept { return !(a == b); }
  friend constexpr bool operator<(Power a, Power b) noexcept { return a.watts_ < b.watts_; }

 private:
  explicit constexpr Power(std::int64_t watts) noexcept : watts_(watts) {}
  std::int64_t watts_ = 0;
};

/// Converts a declared value in a declared unit into canonical joules. The
/// conversion is exact: a value that cannot be represented exactly in whole
/// joules is refused with InexactUnitConversion rather than truncated.
Expected<Energy> to_canonical_energy(std::int64_t value, EnergyUnit unit);

/// Converts canonical joules back into a declared unit, exactly or not at all.
Expected<std::int64_t> from_canonical_energy(Energy energy, EnergyUnit unit);

/// Energy delivered by a constant power over an explicit duration:
/// joules = watts * milliseconds / 1000, checked, exact-or-refused.
Expected<Energy> integrate(Power power, Duration duration);

/// Energy delivered by a constant power over an explicit whole-second
/// duration: joules = watts * seconds, checked. Always exact.
Expected<Energy> integrate_seconds(Power power, std::int64_t seconds);

}  // namespace energy_ledger

#endif  // ENERGY_LEDGER_UNITS_HPP
