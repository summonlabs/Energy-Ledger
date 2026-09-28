// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include <cstddef>
#include <cstdint>
#include <string>

#include "energy_ledger/units.hpp"

namespace energy_ledger {
namespace {

struct UnitFactor {
  EnergyUnit unit;
  std::int64_t numerator;
  std::int64_t denominator;
};

// Exact rational factor from the declared unit to the canonical joule.
constexpr UnitFactor kFactors[] = {
    {EnergyUnit::Joule, 1, 1},
    {EnergyUnit::Kilojoule, 1000, 1},
    {EnergyUnit::Megajoule, 1000000, 1},
    {EnergyUnit::Gigajoule, 1000000000, 1},
    {EnergyUnit::Millijoule, 1, 1000},
    {EnergyUnit::Microjoule, 1, 1000000},
    {EnergyUnit::WattHour, 3600, 1},
    {EnergyUnit::KilowattHour, 3600000, 1},
    {EnergyUnit::MegawattHour, 3600000000LL, 1},
    {EnergyUnit::GigawattHour, 3600000000000LL, 1},
    {EnergyUnit::TerawattHour, 3600000000000000LL, 1},
};

const UnitFactor* find_factor(EnergyUnit unit) noexcept {
  for (const UnitFactor& factor : kFactors) {
    if (factor.unit == unit) {
      return &factor;
    }
  }
  return nullptr;
}

}  // namespace

const char* to_string(EnergyUnit unit) noexcept {
  switch (unit) {
    case EnergyUnit::Joule: return "J";
    case EnergyUnit::Kilojoule: return "kJ";
    case EnergyUnit::Megajoule: return "MJ";
    case EnergyUnit::Gigajoule: return "GJ";
    case EnergyUnit::Millijoule: return "mJ";
    case EnergyUnit::Microjoule: return "uJ";
    case EnergyUnit::WattHour: return "Wh";
    case EnergyUnit::KilowattHour: return "kWh";
    case EnergyUnit::MegawattHour: return "MWh";
    case EnergyUnit::GigawattHour: return "GWh";
    case EnergyUnit::TerawattHour: return "TWh";
  }
  return "unknown";
}

bool is_valid_energy_unit(std::uint8_t value) noexcept {
  return value >= static_cast<std::uint8_t>(EnergyUnit::Joule) &&
         value <= static_cast<std::uint8_t>(EnergyUnit::TerawattHour);
}

Expected<EnergyUnit> parse_energy_unit(std::string_view text) {
  if (text == "J" || text == "joule" || text == "joules") return EnergyUnit::Joule;
  if (text == "kJ" || text == "kilojoule") return EnergyUnit::Kilojoule;
  if (text == "MJ" || text == "megajoule") return EnergyUnit::Megajoule;
  if (text == "GJ" || text == "gigajoule") return EnergyUnit::Gigajoule;
  if (text == "mJ" || text == "millijoule") return EnergyUnit::Millijoule;
  if (text == "uJ" || text == "microjoule") return EnergyUnit::Microjoule;
  if (text == "Wh" || text == "watthour") return EnergyUnit::WattHour;
  if (text == "kWh" || text == "kilowatthour") return EnergyUnit::KilowattHour;
  if (text == "MWh" || text == "megawatthour") return EnergyUnit::MegawattHour;
  if (text == "GWh" || text == "gigawatthour") return EnergyUnit::GigawattHour;
  if (text == "TWh" || text == "terawatthour") return EnergyUnit::TerawattHour;
  return make_error(StatusCode::InvalidUnit, "unknown energy unit '" + std::string(text) + "'");
}

Expected<Energy> Energy::from_joules(std::int64_t joules) {
  return Energy(joules);
}

Expected<Energy> Energy::add(Energy a, Energy b) {
  std::int64_t sum = 0;
  if (!checked::add(a.joules_, b.joules_, sum)) {
    return make_error(StatusCode::ArithmeticOverflow, "energy addition overflows int64 joules");
  }
  return Energy(sum);
}

Expected<Energy> Energy::subtract(Energy a, Energy b) {
  std::int64_t difference = 0;
  if (!checked::sub(a.joules_, b.joules_, difference)) {
    return make_error(StatusCode::ArithmeticOverflow, "energy subtraction overflows int64 joules");
  }
  return Energy(difference);
}

Expected<Energy> Energy::negate(Energy a) {
  std::int64_t value = 0;
  if (!checked::negate(a.joules_, value)) {
    return make_error(StatusCode::ArithmeticOverflow, "energy negation overflows int64 joules");
  }
  return Energy(value);
}

Expected<Power> Power::from_watts(std::int64_t watts) { return Power(watts); }

Expected<Power> Power::from_milliwatts(std::int64_t milliwatts) {
  std::int64_t watts = 0;
  if (!checked::div_exact(milliwatts, 1000, watts)) {
    return make_error(StatusCode::InexactUnitConversion,
                      "active power in milliwatts is not a whole number of watts");
  }
  return Power(watts);
}

Expected<Energy> to_canonical_energy(std::int64_t value, EnergyUnit unit) {
  const UnitFactor* factor = find_factor(unit);
  if (factor == nullptr) {
    return make_error(StatusCode::InvalidUnit, "unrecognized energy unit code");
  }
  std::int64_t scaled = 0;
  if (!checked::mul(value, factor->numerator, scaled)) {
    return make_error(StatusCode::ArithmeticOverflow,
                      "declared quantity overflows while converting to canonical joules");
  }
  std::int64_t joules = 0;
  if (!checked::div_exact(scaled, factor->denominator, joules)) {
    return make_error(StatusCode::InexactUnitConversion,
                      "declared quantity is not an exact whole number of joules");
  }
  return Energy::from_joules(joules);
}

Expected<std::int64_t> from_canonical_energy(Energy energy, EnergyUnit unit) {
  const UnitFactor* factor = find_factor(unit);
  if (factor == nullptr) {
    return make_error(StatusCode::InvalidUnit, "unrecognized energy unit code");
  }
  std::int64_t scaled = 0;
  if (!checked::mul(energy.joules(), factor->denominator, scaled)) {
    return make_error(StatusCode::ArithmeticOverflow,
                      "canonical quantity overflows while converting from joules");
  }
  std::int64_t value = 0;
  if (!checked::div_exact(scaled, factor->numerator, value)) {
    return make_error(StatusCode::InexactUnitConversion,
                      "canonical quantity is not an exact whole number of the requested unit");
  }
  return value;
}

Expected<Energy> integrate(Power power, Duration duration) {
  if (duration.is_negative()) {
    return make_error(StatusCode::InvalidArgument, "integration duration must not be negative");
  }
  std::int64_t watt_milliseconds = 0;
  if (!checked::mul(power.watts(), duration.milliseconds(), watt_milliseconds)) {
    return make_error(StatusCode::ArithmeticOverflow, "power * duration overflows");
  }
  std::int64_t joules = 0;
  if (!checked::div_exact(watt_milliseconds, 1000, joules)) {
    return make_error(StatusCode::IntegrationInexact,
                      "power integrated over the interval is not a whole number of joules");
  }
  return Energy::from_joules(joules);
}

Expected<Energy> integrate_seconds(Power power, std::int64_t seconds) {
  if (seconds < 0) {
    return make_error(StatusCode::InvalidArgument, "integration duration must not be negative");
  }
  std::int64_t joules = 0;
  if (!checked::mul(power.watts(), seconds, joules)) {
    return make_error(StatusCode::ArithmeticOverflow, "power * seconds overflows");
  }
  return Energy::from_joules(joules);
}

}  // namespace energy_ledger
