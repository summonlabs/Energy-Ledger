// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Model proof obligations: units, checked arithmetic, canonical encoding,
// strict parsing, identity derivation and deterministic validation precedence.

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "energy_ledger/canonical.hpp"
#include "energy_ledger/energy_ledger.hpp"
#include "support/harness.hpp"
#include "support/util.hpp"

namespace {

using namespace energy_ledger;

EntryContent valid_content() {
  EntryContent content = eltest::sample_entry(EntryKind::Delivered, 100, "interval-a", "meter-1");
  return content;
}

}  // namespace

EL_TEST(digest_known_answers) {
  EL_CHECK_EQ(Sha256::hash(std::string_view("")).to_hex(),
              std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
  EL_CHECK_EQ(Sha256::hash(std::string_view("abc")).to_hex(),
              std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
  EL_CHECK_EQ(
      Sha256::hash(std::string_view("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))
          .to_hex(),
      std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));

  // CRC-32 with a zero seed and no final inversion; the standard reflected
  // CRC-32 is this function with seed 0xFFFFFFFF and a final inversion.
  const char* check = "123456789";
  const std::uint32_t standard = crc32_update(0xFFFFFFFFu, check, 9) ^ 0xFFFFFFFFu;
  EL_CHECK_EQ(standard, 0xCBF43926u);
  EL_CHECK_EQ(crc32("", 0), 0u);
  const std::uint32_t first = crc32(check, 4);
  const std::uint32_t combined = crc32_update(first, check + 4, 5);
  EL_CHECK_EQ(combined, crc32(check, 9));
  EL_CHECK(crc32(check, 9) != crc32(check, 8));
}

EL_TEST(units_are_exact_or_refused) {
  auto watt_hours = to_canonical_energy(1, EnergyUnit::WattHour);
  EL_REQUIRE(watt_hours.has_value());
  EL_CHECK_EQ(watt_hours.value().joules(), 3600);
  EL_CHECK_EQ(to_canonical_energy(1, EnergyUnit::KilowattHour).value().joules(), 3600000);
  EL_CHECK_EQ(to_canonical_energy(-5, EnergyUnit::Joule).value().joules(), -5);

  // Millijoules convert exactly only in whole joules.
  EL_CHECK_EQ(to_canonical_energy(2000, EnergyUnit::Millijoule).value().joules(), 2);
  auto inexact = to_canonical_energy(2500, EnergyUnit::Millijoule);
  EL_REQUIRE(!inexact.has_value());
  EL_CHECK_EQ(inexact.error().code, StatusCode::InexactUnitConversion);

  // Overflow is refused, never wrapped.
  auto overflow = to_canonical_energy(std::numeric_limits<std::int64_t>::max(),
                                      EnergyUnit::TerawattHour);
  EL_REQUIRE(!overflow.has_value());
  EL_CHECK_EQ(overflow.error().code, StatusCode::ArithmeticOverflow);

  EL_CHECK_EQ(from_canonical_energy(Energy::from_joules(3600000).value(), EnergyUnit::KilowattHour)
                  .value(),
              1);
  auto lossy = from_canonical_energy(Energy::from_joules(1).value(), EnergyUnit::KilowattHour);
  EL_REQUIRE(!lossy.has_value());
  EL_CHECK_EQ(lossy.error().code, StatusCode::InexactUnitConversion);
}

EL_TEST(power_and_energy_are_distinct_types) {
  auto power = Power::from_watts(1000);
  EL_REQUIRE(power.has_value());
  auto duration = Duration::from_seconds(3600);
  EL_REQUIRE(duration.has_value());
  auto energy = integrate(power.value(), duration.value());
  EL_REQUIRE(energy.has_value());
  EL_CHECK_EQ(energy.value().joules(), 3600000);

  // Energy over a whole second count is always exact; a sub-second interval
  // that is not a whole number of joules is refused rather than truncated.
  EL_CHECK_EQ(integrate_seconds(Power::from_watts(3).value(), 5).value().joules(), 15);
  auto partial = Duration::from_milliseconds(1500);
  EL_REQUIRE(partial.has_value());
  auto inexact = integrate(Power::from_watts(3).value(), partial.value());
  EL_REQUIRE(!inexact.has_value());
  EL_CHECK_EQ(inexact.error().code, StatusCode::IntegrationInexact);

  auto overflow = integrate_seconds(Power::from_watts(std::numeric_limits<std::int64_t>::max()).value(),
                                    2);
  EL_REQUIRE(!overflow.has_value());
  EL_CHECK_EQ(overflow.error().code, StatusCode::ArithmeticOverflow);
}

EL_TEST(checked_arithmetic_boundaries) {
  const std::int64_t max = std::numeric_limits<std::int64_t>::max();
  const std::int64_t min = std::numeric_limits<std::int64_t>::min();
  std::int64_t out = 0;
  EL_CHECK(!checked::add(max, 1, out));
  EL_CHECK(!checked::add(min, -1, out));
  EL_CHECK(checked::add(max, -1, out) && out == max - 1);
  EL_CHECK(!checked::sub(min, 1, out));
  EL_CHECK(!checked::mul(max, 2, out));
  EL_CHECK(!checked::mul(min, -1, out));
  EL_CHECK(!checked::negate(min, out));
  EL_CHECK(checked::negate(min + 1, out) && out == max);
  EL_CHECK(!checked::div_exact(7, 2, out));
  EL_CHECK(checked::div_exact(min, 2, out) && out == min / 2);
  EL_CHECK(!checked::div_exact(min, -1, out));
  EL_CHECK(!checked::div_exact(5, 0, out));
  EL_CHECK_EQ(checked::magnitude(min), std::uint64_t{1} << 63);
}

EL_TEST(strong_identities_reject_bad_input) {
  EL_CHECK(!FacilityObjectRef::parse("").has_value());
  EL_CHECK(!FacilityObjectRef::parse(" leading").has_value());
  EL_CHECK(!FacilityObjectRef::parse("trailing ").has_value());
  EL_CHECK(!FacilityObjectRef::parse("has\ttab").has_value());
  EL_CHECK(!FacilityObjectRef::parse("non-ascii-\xC3\xA9").has_value());
  EL_CHECK(!FacilityObjectRef::parse(std::string(129, 'a')).has_value());
  EL_CHECK(FacilityObjectRef::parse(std::string(128, 'a')).has_value());
  EL_CHECK(FacilityObjectRef::parse("facility/line-1").has_value());

  const auto incarnation = generate_ledger_incarnation();
  EL_CHECK(!incarnation.is_zero());
  const auto parsed = LedgerIncarnation::from_hex(incarnation.to_hex());
  EL_REQUIRE(parsed.has_value());
  EL_CHECK(parsed.value() == incarnation);
  EL_CHECK(!LedgerIncarnation::from_hex("zz").has_value());
  EL_CHECK(!LedgerIncarnation::from_hex(incarnation.to_hex().substr(0, 30)).has_value());

  EL_CHECK(!AuthorityTier::create(16).has_value());
  EL_CHECK_EQ(AuthorityTier::create(15).value().value(), 15);

  EL_CHECK(TimeTicks::unknown().is_unknown());
  EL_CHECK(TimeTicks(0).is_known());
  EL_CHECK(TimeTicks::unknown() != TimeTicks(0));
  EL_CHECK_EQ(TimeTicks().value(), TimeTicks::kUnknownValue);
}

EL_TEST(enum_text_roundtrip) {
  const EntryKind kinds[] = {EntryKind::Delivered, EntryKind::Consumed, EntryKind::WastedLost,
                             EntryKind::Unclassified, EntryKind::ReservationCommit,
                             EntryKind::Curtailed, EntryKind::Correction, EntryKind::Supersession,
                             EntryKind::Compensating};
  for (EntryKind kind : kinds) {
    auto parsed = parse_entry_kind(to_string(kind));
    EL_REQUIRE(parsed.has_value());
    EL_CHECK_EQ(parsed.value(), kind);
  }
  EL_CHECK_EQ(domain_of(EntryKind::Delivered), AccountingDomain::MeasuredFlow);
  EL_CHECK_EQ(domain_of(EntryKind::ReservationCommit), AccountingDomain::Commitment);
  EL_CHECK_EQ(domain_of(EntryKind::Correction), AccountingDomain::Adjustment);
  EL_CHECK(asserts_quantity(EntryKind::Delivered));
  EL_CHECK(!asserts_quantity(EntryKind::Correction));
  EL_CHECK(is_adjustment(EntryKind::Supersession));

  const TimeBasis bases[] = {TimeBasis::Unspecified, TimeBasis::MonotonicTicks,
                             TimeBasis::UtcUnixMilliseconds, TimeBasis::UtcUnixSeconds,
                             TimeBasis::WallClockMinutes, TimeBasis::MeterRegisterTicks,
                             TimeBasis::IntervalIndex};
  for (TimeBasis basis : bases) {
    auto parsed = parse_time_basis(to_string(basis));
    EL_REQUIRE(parsed.has_value());
    EL_CHECK_EQ(parsed.value(), basis);
  }
  EL_CHECK(!parse_time_basis("nonsense").has_value());

  const EnergyUnit units[] = {EnergyUnit::Joule,     EnergyUnit::Kilojoule,   EnergyUnit::Megajoule,
                              EnergyUnit::Gigajoule, EnergyUnit::Millijoule,   EnergyUnit::Microjoule,
                              EnergyUnit::WattHour,  EnergyUnit::KilowattHour, EnergyUnit::MegawattHour,
                              EnergyUnit::GigawattHour, EnergyUnit::TerawattHour};
  for (EnergyUnit unit : units) {
    auto parsed = parse_energy_unit(to_string(unit));
    EL_REQUIRE(parsed.has_value());
    EL_CHECK_EQ(parsed.value(), unit);
  }
  EL_CHECK(!parse_energy_unit("furlongs").has_value());
}

EL_TEST(validation_precedence_is_deterministic) {
  // Bad kind dominates every other defect.
  {
    EntryContent content = valid_content();
    content.kind = static_cast<EntryKind>(99);
    content.object = FacilityObjectRef();
    content.declared_unit = static_cast<EnergyUnit>(200);
    auto result = validate_content(content);
    EL_REQUIRE(!result.has_value());
    EL_CHECK_EQ(result.error().code, StatusCode::InvalidArgument);
  }
  // Then quality, then time basis, then unit, then references.
  {
    EntryContent content = valid_content();
    content.quality = static_cast<ObservationQuality>(200);
    content.time_basis = static_cast<TimeBasis>(200);
    content.object = FacilityObjectRef();
    auto result = validate_content(content);
    EL_REQUIRE(!result.has_value());
    EL_CHECK_EQ(result.error().code, StatusCode::InvalidArgument);
  }
  {
    EntryContent content = valid_content();
    content.time_basis = static_cast<TimeBasis>(200);
    content.declared_unit = static_cast<EnergyUnit>(200);
    content.object = FacilityObjectRef();
    auto result = validate_content(content);
    EL_REQUIRE(!result.has_value());
    EL_CHECK_EQ(result.error().code, StatusCode::InvalidTimeBasis);
  }
  {
    EntryContent content = valid_content();
    content.declared_unit = static_cast<EnergyUnit>(200);
    content.object = FacilityObjectRef();
    auto result = validate_content(content);
    EL_REQUIRE(!result.has_value());
    EL_CHECK_EQ(result.error().code, StatusCode::InvalidUnit);
  }
  {
    EntryContent content = valid_content();
    content.object = FacilityObjectRef();
    content.interval = IntervalRef();
    auto result = validate_content(content);
    EL_REQUIRE(!result.has_value());
    EL_CHECK_EQ(result.error().code, StatusCode::InvalidRef);
  }
  // Unspecified time basis with known interval bounds.
  {
    EntryContent content = valid_content();
    content.time_basis = TimeBasis::Unspecified;
    auto result = validate_content(content);
    EL_REQUIRE(!result.has_value());
    EL_CHECK_EQ(result.error().code, StatusCode::InvalidInterval);
  }
  // Interval end before start.
  {
    EntryContent content = valid_content();
    content.interval_end = TimeTicks(content.interval_start.value() - 1);
    auto result = validate_content(content);
    EL_REQUIRE(!result.has_value());
    EL_CHECK_EQ(result.error().code, StatusCode::InvalidInterval);
  }
  // Negative physical quantity.
  {
    EntryContent content = valid_content();
    content.declared_value = -1;
    content.quantity = Energy::from_joules(-1).value();
    auto result = validate_content(content);
    EL_REQUIRE(!result.has_value());
    EL_CHECK_EQ(result.error().code, StatusCode::QuantitySignNotAllowed);
  }
  // Declared unit and canonical quantity disagree.
  {
    EntryContent content = valid_content();
    content.declared_value = 42;
    auto result = validate_content(content);
    EL_REQUIRE(!result.has_value());
    EL_CHECK_EQ(result.error().code, StatusCode::InconsistentDeclaredQuantity);
  }
  // Target on a non adjustment entry.
  {
    EntryContent content = valid_content();
    content.target = SequenceNumber(3);
    auto result = validate_content(content);
    EL_REQUIRE(!result.has_value());
    EL_CHECK_EQ(result.error().code, StatusCode::InvalidTarget);
  }
  // Adjustment without a target.
  {
    EntryContent content = valid_content();
    content.kind = EntryKind::Correction;
    auto result = validate_content(content);
    EL_REQUIRE(!result.has_value());
    EL_CHECK_EQ(result.error().code, StatusCode::InvalidTarget);
  }
  // Unsorted annotations.
  {
    EntryContent content = valid_content();
    content.annotations.push_back(Annotation{"b", "2"});
    content.annotations.push_back(Annotation{"a", "1"});
    auto result = validate_content(content);
    EL_REQUIRE(!result.has_value());
    EL_CHECK_EQ(result.error().code, StatusCode::InvalidAnnotation);
  }
  // Duplicate annotation keys.
  {
    EntryContent content = valid_content();
    content.annotations.push_back(Annotation{"a", "1"});
    content.annotations.push_back(Annotation{"a", "2"});
    auto result = validate_content(content);
    EL_REQUIRE(!result.has_value());
    EL_CHECK_EQ(result.error().code, StatusCode::InvalidAnnotation);
  }
  // Too many annotations.
  {
    EntryContent content = valid_content();
    for (std::size_t index = 0; index < ModelLimits::kMaxAnnotations + 1; ++index) {
      content.annotations.push_back(
          Annotation{"k" + std::to_string(index), "v"});
    }
    auto result = validate_content(content);
    EL_REQUIRE(!result.has_value());
    EL_CHECK_EQ(result.error().code, StatusCode::InvalidAnnotation);
  }

  // The same invalid request always produces the same primary code.
  EntryContent broken = valid_content();
  broken.object = FacilityObjectRef();
  const StatusCode first = validate_content(broken).error().code;
  for (int repeat = 0; repeat < 64; ++repeat) {
    EL_CHECK_EQ(validate_content(broken).error().code, first);
  }
}

EL_TEST(canonical_encoding_is_deterministic) {
  EntryContent content = valid_content();
  content.annotations.push_back(Annotation{"meter", "m-17"});
  content.annotations.push_back(Annotation{"note", "revenue-grade"});
  auto first = encode_content(content);
  EL_REQUIRE(first.has_value());
  for (int repeat = 0; repeat < 16; ++repeat) {
    auto again = encode_content(content);
    EL_REQUIRE(again.has_value());
    EL_CHECK(again.value() == first.value());
  }
  auto decoded = decode_content(first.value());
  EL_REQUIRE(decoded.has_value());
  EL_CHECK(decoded.value() == content);
  auto digest = content_digest(content);
  EL_REQUIRE(digest.has_value());
  auto identity = event_identity(content);
  EL_REQUIRE(identity.has_value());

  // A semantically different content produces a different digest and identity.
  EntryContent other = content;
  other.quality = ObservationQuality::Estimated;
  other.declared_value = content.declared_value + 1;
  other.quantity = to_canonical_energy(other.declared_value, other.declared_unit).value();
  EL_CHECK(content_digest(other).value() != digest.value());
  EL_CHECK(event_identity(other).value() != identity.value());

  // Event identity is a pure function of content: reproducing the same content
  // in a different process, ledger or incarnation yields the same identity.
  auto reproduced = event_identity(other);
  EL_REQUIRE(reproduced.has_value());
  EL_CHECK(reproduced.value() == event_identity(other).value());
}

EL_TEST(canonical_decode_rejects_malformed_input) {
  EntryContent content = valid_content();
  content.annotations.push_back(Annotation{"reason", "checked"});
  auto encoded = encode_content(content);
  EL_REQUIRE(encoded.has_value());
  const std::vector<std::uint8_t>& bytes = encoded.value();

  // Truncation at every length must be refused.
  for (std::size_t length = 0; length < bytes.size(); ++length) {
    auto decoded = decode_content(bytes.data(), length);
    EL_CHECK_MSG(!decoded.has_value(), "truncation to " + std::to_string(length) + " must be refused");
  }
  // Trailing byte.
  {
    std::vector<std::uint8_t> extended = bytes;
    extended.push_back(0);
    auto decoded = decode_content(extended.data(), extended.size());
    EL_REQUIRE(!decoded.has_value());
    EL_CHECK_EQ(decoded.error().code, StatusCode::MalformedInput);
  }
  // Version bump.
  {
    std::vector<std::uint8_t> mutated = bytes;
    mutated[0] = 2;
    auto decoded = decode_content(mutated.data(), mutated.size());
    EL_REQUIRE(!decoded.has_value());
    EL_CHECK_EQ(decoded.error().code, StatusCode::StoreFormatUnsupported);
  }
  // Non zero reserved byte.
  {
    std::vector<std::uint8_t> mutated = bytes;
    mutated[6] = 1;
    auto decoded = decode_content(mutated.data(), mutated.size());
    EL_REQUIRE(!decoded.has_value());
    EL_CHECK_EQ(decoded.error().code, StatusCode::MalformedInput);
  }
  // Unknown enumerators.
  {
    std::vector<std::uint8_t> mutated = bytes;
    mutated[1] = 0;
    EL_CHECK_EQ(decode_content(mutated.data(), mutated.size()).error().code,
                StatusCode::InvalidArgument);
    mutated = bytes;
    mutated[2] = 200;
    EL_CHECK_EQ(decode_content(mutated.data(), mutated.size()).error().code,
                StatusCode::InvalidArgument);
    mutated = bytes;
    mutated[3] = 200;
    EL_CHECK_EQ(decode_content(mutated.data(), mutated.size()).error().code,
                StatusCode::InvalidTimeBasis);
    mutated = bytes;
    mutated[5] = 200;
    EL_CHECK_EQ(decode_content(mutated.data(), mutated.size()).error().code, StatusCode::InvalidUnit);
    mutated = bytes;
    mutated[4] = 200;
    EL_CHECK_EQ(decode_content(mutated.data(), mutated.size()).error().code,
                StatusCode::InvalidAuthorityTier);
  }
  // Oversized declared annotation count.
  {
    std::vector<std::uint8_t> mutated = bytes;
    mutated[80] = 0xFF;
    mutated[81] = 0xFF;
    auto decoded = decode_content(mutated.data(), mutated.size());
    EL_REQUIRE(!decoded.has_value());
    EL_CHECK(decoded.error().code == StatusCode::LimitExceeded ||
             decoded.error().code == StatusCode::MalformedInput);
  }
  // Every single bit mutation is either refused outright or changes the
  // content the canonical bytes denote. Canonical encoding is a bijection:
  // whatever decodes must re-encode to exactly the same bytes, so a mutation
  // can never be invisible to the content digest.
  std::uint64_t accepted = 0;
  std::uint64_t rejected = 0;
  const Digest256 original_digest = content_digest(content).value();
  for (std::size_t offset = 0; offset < bytes.size(); ++offset) {
    for (int bit = 0; bit < 8; ++bit) {
      std::vector<std::uint8_t> mutated = bytes;
      mutated[offset] = static_cast<std::uint8_t>(mutated[offset] ^ (1u << bit));
      auto decoded = decode_content(mutated.data(), mutated.size());
      if (!decoded.has_value()) {
        ++rejected;
        continue;
      }
      ++accepted;
      auto reencoded = encode_content(decoded.value());
      EL_REQUIRE(reencoded.has_value());
      EL_CHECK_MSG(reencoded.value() == mutated,
                   "canonical round trip differs at " + std::to_string(offset) + ":" +
                       std::to_string(bit));
      auto mutated_digest = content_digest(decoded.value());
      EL_REQUIRE(mutated_digest.has_value());
      EL_CHECK_MSG(!(mutated_digest.value() == original_digest),
                   "mutation at " + std::to_string(offset) + ":" + std::to_string(bit) +
                       " is invisible to the content digest");
    }
  }
  EL_CHECK(rejected > 0);
  EL_CHECK(accepted > 0);
  EL_CHECK_EQ(accepted + rejected, static_cast<std::uint64_t>(bytes.size()) * 8);
}

EL_TEST(json_reader_is_strict_and_bounded) {
  auto ok = parse_json(R"({"a":1,"b":[true,false,null],"c":"x"})");
  EL_REQUIRE(ok.has_value());
  EL_CHECK(ok.value().is_object());
  EL_CHECK_EQ(ok.value().require_integer("a").value(), 1);
  EL_CHECK_EQ(ok.value().require_string("c").value(), std::string("x"));

  const char* rejected[] = {
      "",
      "{",
      "{\"a\":1,}",
      "{\"a\":1}{\"b\":2}",
      "{\"a\":1} trailing",
      "{\"a\":1,\"a\":2}",
      "{\"a\":01}",
      "{\"a\":1.5}",
      "{\"a\":1e3}",
      "{\"a\":9223372036854775808}",
      "{\"a\":-9223372036854775809}",
      "[1,2",
      "\"unterminated",
      "\"raw\ncontrol\"",
      "\"bad \\q escape\"",
      "\"\\uD800\"",
      "\"\\uDC00\"",
      "{\"a\":}",
      "nul",
      "tru",
      "\"\xC0\x80\"",
  };
  for (const char* text : rejected) {
    auto parsed = parse_json(text);
    EL_CHECK_MSG(!parsed.has_value(), std::string("must reject: ") + text);
  }

  // Depth limit.
  std::string deep;
  for (int index = 0; index < 40; ++index) {
    deep += "[";
  }
  for (int index = 0; index < 40; ++index) {
    deep += "]";
  }
  JsonLimits limits;
  limits.max_depth = 8;
  auto too_deep = parse_json(deep, limits);
  EL_REQUIRE(!too_deep.has_value());
  EL_CHECK_EQ(too_deep.error().code, StatusCode::LimitExceeded);

  // String length limit.
  std::string long_string = "\"";
  long_string.append(100, 'a');
  long_string += "\"";
  JsonLimits small;
  small.max_string_bytes = 16;
  auto too_long = parse_json(long_string, small);
  EL_REQUIRE(!too_long.has_value());
  EL_CHECK_EQ(too_long.error().code, StatusCode::LimitExceeded);

  // Input size limit.
  JsonLimits tiny;
  tiny.max_input_bytes = 4;
  auto too_large = parse_json("{\"a\":1}", tiny);
  EL_REQUIRE(!too_large.has_value());
  EL_CHECK_EQ(too_large.error().code, StatusCode::LimitExceeded);

  // Surrogate pairs are combined and re-encoded as UTF-8.
  auto pair = parse_json(R"("\uD83D\uDE00")");
  EL_REQUIRE(pair.has_value());
  EL_CHECK_EQ(pair.value().as_string().size(), std::size_t{4});
}

EL_TEST(json_writer_is_deterministic) {
  JsonValue object = JsonValue::object();
  object.set("b", JsonValue::integer(2));
  object.set("a", JsonValue::string("quote\" and \\ backslash\n"));
  object.set("c", JsonValue::array());
  const std::string compact = object.dump();
  EL_CHECK_EQ(compact, std::string(R"({"b":2,"a":"quote\" and \\ backslash\n","c":[]})"));
  for (int repeat = 0; repeat < 8; ++repeat) {
    EL_CHECK_EQ(object.dump(), compact);
  }
  JsonValue parsed = parse_json(compact).value();
  EL_CHECK_EQ(parsed.find("a")->as_string(), std::string("quote\" and \\ backslash\n"));
}

EL_TEST(accounting_key_ordering_is_total) {
  EntryContent a = valid_content();
  EntryContent b = valid_content();
  b.interval = IntervalRef::parse("interval-b").value();
  const AccountingKey key_a = accounting_key_of(a);
  const AccountingKey key_b = accounting_key_of(b);
  EL_CHECK(key_a == accounting_key_of(a));
  EL_CHECK(key_a != key_b);
  EL_CHECK((key_a < key_b) != (key_b < key_a));
}

EL_TEST(format_description_is_stable) {
  const FormatInfo info = format_info();
  EL_CHECK_EQ(info.descriptor_version, 1u);
  EL_CHECK_EQ(info.manifest_version, 1u);
  EL_CHECK_EQ(info.segment_version, 1u);
  EL_CHECK_EQ(info.lease_version, 1u);
  EL_CHECK_EQ(info.descriptor_bytes, std::size_t{96});
  EL_CHECK_EQ(info.manifest_header_bytes, std::size_t{224});
  EL_CHECK_EQ(info.segment_header_bytes, std::size_t{64});
  EL_CHECK_EQ(info.lease_bytes, std::size_t{128});
  EL_CHECK(info.little_endian_on_disk);
  EL_CHECK_EQ(kStoreFormatVersion, 1);
}
