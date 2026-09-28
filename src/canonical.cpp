// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "byte_io.hpp"
#include "energy_ledger/canonical.hpp"

namespace energy_ledger {
namespace {

Expected<void> write_ref(detail::ByteWriter& writer, const char* name, const std::string& value) {
  if (!writer.sized_bytes(value.data(), value.size())) {
    return make_error(StatusCode::LimitExceeded,
                      std::string(name) + " reference exceeds the encodable length");
  }
  return Expected<void>();
}

Expected<std::string> read_ref(detail::ByteReader& reader, const char* name, std::size_t max_length) {
  const std::uint8_t* data = nullptr;
  std::size_t length = 0;
  if (!reader.sized_bytes(max_length, data, length)) {
    return make_error(StatusCode::MalformedInput,
                      std::string(name) + " reference is truncated or exceeds the length limit");
  }
  return std::string(reinterpret_cast<const char*>(data), length);
}

}  // namespace

Expected<std::vector<std::uint8_t>> encode_content(const EntryContent& content) {
  auto valid = validate_content(content);
  if (!valid) {
    return valid.error();
  }

  std::vector<std::uint8_t> bytes;
  bytes.reserve(256);
  detail::ByteWriter writer(bytes);

  writer.u8(kContentEncodingVersion);
  writer.u8(static_cast<std::uint8_t>(content.kind));
  writer.u8(static_cast<std::uint8_t>(content.quality));
  writer.u8(static_cast<std::uint8_t>(content.time_basis));
  writer.u8(content.authority_tier.value());
  writer.u8(static_cast<std::uint8_t>(content.declared_unit));
  writer.u8(0);  // reserved, must be zero
  writer.u8(0);  // reserved, must be zero

  writer.i64(content.quantity.joules());
  writer.i64(content.declared_value);
  writer.u64(content.source_revision.value());
  writer.u64(content.source_generation.value());
  writer.u64(content.source_epoch.value());
  writer.u64(content.target.value());
  writer.i64(content.interval_start.value());
  writer.i64(content.interval_end.value());
  writer.i64(content.observed_at.value());
  writer.u16(static_cast<std::uint16_t>(content.annotations.size()));

  const struct {
    const char* name;
    const std::string& value;
  } refs[] = {
      {"object", content.object.value()},
      {"generation", content.generation.value()},
      {"interval", content.interval.value()},
      {"clock-domain", content.clock_domain.value()},
      {"source-family", content.source_family.value()},
      {"source-instance", content.source_instance.value()},
      {"authority", content.authority.value()},
  };
  for (const auto& ref : refs) {
    auto written = write_ref(writer, ref.name, ref.value);
    if (!written) {
      return written.error();
    }
  }

  for (const Annotation& annotation : content.annotations) {
    auto key = write_ref(writer, "annotation key", annotation.key);
    if (!key) {
      return key.error();
    }
    auto value = write_ref(writer, "annotation value", annotation.value);
    if (!value) {
      return value.error();
    }
  }

  if (bytes.size() > ModelLimits::kMaxContentBytes) {
    return make_error(StatusCode::LimitExceeded,
                      "canonical content exceeds " + std::to_string(ModelLimits::kMaxContentBytes) +
                          " bytes");
  }
  return bytes;
}

Expected<EntryContent> decode_content(const std::uint8_t* data, std::size_t length) {
  if (length > ModelLimits::kMaxContentBytes) {
    return make_error(StatusCode::LimitExceeded, "canonical content exceeds the content size limit");
  }
  detail::ByteReader reader(data, length);

  std::uint8_t version = 0;
  std::uint8_t kind = 0;
  std::uint8_t quality = 0;
  std::uint8_t time_basis = 0;
  std::uint8_t authority_tier = 0;
  std::uint8_t declared_unit = 0;
  std::uint8_t reserved0 = 0;
  std::uint8_t reserved1 = 0;

  if (!reader.u8(version) || !reader.u8(kind) || !reader.u8(quality) || !reader.u8(time_basis) ||
      !reader.u8(authority_tier) || !reader.u8(declared_unit) || !reader.u8(reserved0) ||
      !reader.u8(reserved1)) {
    return make_error(StatusCode::MalformedInput, "canonical content header is truncated");
  }
  if (version != kContentEncodingVersion) {
    return make_error(StatusCode::StoreFormatUnsupported,
                      "unsupported canonical content version " + std::to_string(version));
  }
  if (reserved0 != 0 || reserved1 != 0) {
    return make_error(StatusCode::MalformedInput, "reserved header bytes must be zero");
  }
  if (!is_valid_entry_kind(kind)) {
    return make_error(StatusCode::InvalidArgument, "entry kind byte is not a known enumerator");
  }
  if (!is_valid_quality(quality)) {
    return make_error(StatusCode::InvalidArgument, "quality byte is not a known enumerator");
  }
  if (!is_valid_time_basis(time_basis)) {
    return make_error(StatusCode::InvalidTimeBasis, "time basis byte is not a known enumerator");
  }
  if (!is_valid_energy_unit(declared_unit)) {
    return make_error(StatusCode::InvalidUnit, "declared unit byte is not a known enumerator");
  }
  if (authority_tier > AuthorityTier::kMaxValue) {
    return make_error(StatusCode::InvalidAuthorityTier, "authority tier byte is out of range");
  }

  std::int64_t quantity = 0;
  std::int64_t declared_value = 0;
  std::uint64_t revision = 0;
  std::uint64_t source_generation = 0;
  std::uint64_t source_epoch = 0;
  std::uint64_t target = 0;
  std::int64_t interval_start = 0;
  std::int64_t interval_end = 0;
  std::int64_t observed_at = 0;
  std::uint16_t annotation_count = 0;

  if (!reader.i64(quantity) || !reader.i64(declared_value) || !reader.u64(revision) ||
      !reader.u64(source_generation) || !reader.u64(source_epoch) || !reader.u64(target) ||
      !reader.i64(interval_start) || !reader.i64(interval_end) || !reader.i64(observed_at) ||
      !reader.u16(annotation_count)) {
    return make_error(StatusCode::MalformedInput, "canonical content body is truncated");
  }
  if (annotation_count > ModelLimits::kMaxAnnotations) {
    return make_error(StatusCode::LimitExceeded, "annotation count exceeds the model limit");
  }

  EntryContent content;

  auto object = read_ref(reader, "object", OpaqueRef<FacilityObjectTag>::kMaxLength);
  if (!object) return object.error();
  auto generation = read_ref(reader, "generation", OpaqueRef<GenerationTag>::kMaxLength);
  if (!generation) return generation.error();
  auto interval = read_ref(reader, "interval", OpaqueRef<IntervalTag>::kMaxLength);
  if (!interval) return interval.error();
  auto clock_domain = read_ref(reader, "clock-domain", OpaqueRef<ClockDomainTag>::kMaxLength);
  if (!clock_domain) return clock_domain.error();
  auto source_family = read_ref(reader, "source-family", OpaqueRef<SourceFamilyTag>::kMaxLength);
  if (!source_family) return source_family.error();
  auto source_instance = read_ref(reader, "source-instance", OpaqueRef<SourceInstanceTag>::kMaxLength);
  if (!source_instance) return source_instance.error();
  auto authority = read_ref(reader, "authority", OpaqueRef<AuthorityTag>::kMaxLength);
  if (!authority) return authority.error();

  auto object_ref = FacilityObjectRef::parse(object.value());
  if (!object_ref) return object_ref.error();
  auto generation_ref = GenerationRef::parse(generation.value());
  if (!generation_ref) return generation_ref.error();
  auto interval_ref = IntervalRef::parse(interval.value());
  if (!interval_ref) return interval_ref.error();
  auto clock_ref = ClockDomainRef::parse(clock_domain.value());
  if (!clock_ref) return clock_ref.error();
  auto family_ref = SourceFamilyRef::parse(source_family.value());
  if (!family_ref) return family_ref.error();
  auto instance_ref = SourceInstanceRef::parse(source_instance.value());
  if (!instance_ref) return instance_ref.error();
  auto authority_ref = AuthorityRef::parse(authority.value());
  if (!authority_ref) return authority_ref.error();

  content.object = object_ref.value();
  content.generation = generation_ref.value();
  content.interval = interval_ref.value();
  content.clock_domain = clock_ref.value();
  content.source_family = family_ref.value();
  content.source_instance = instance_ref.value();
  content.authority = authority_ref.value();

  content.annotations.reserve(annotation_count);
  for (std::uint16_t index = 0; index < annotation_count; ++index) {
    auto key = read_ref(reader, "annotation key", ModelLimits::kMaxAnnotationKeyBytes);
    if (!key) return key.error();
    auto value = read_ref(reader, "annotation value", ModelLimits::kMaxAnnotationValueBytes);
    if (!value) return value.error();
    Annotation annotation;
    annotation.key = std::move(key).value();
    annotation.value = std::move(value).value();
    content.annotations.push_back(std::move(annotation));
  }

  if (reader.remaining() != 0) {
    return make_error(StatusCode::MalformedInput, "trailing bytes after the canonical content");
  }

  content.kind = static_cast<EntryKind>(kind);
  content.quality = static_cast<ObservationQuality>(quality);
  content.time_basis = static_cast<TimeBasis>(time_basis);
  content.declared_unit = static_cast<EnergyUnit>(declared_unit);
  auto tier = AuthorityTier::create(authority_tier);
  if (!tier) return tier.error();
  content.authority_tier = tier.value();

  auto energy = Energy::from_joules(quantity);
  if (!energy) return energy.error();
  content.quantity = energy.value();
  content.declared_value = declared_value;
  content.source_revision = SourceRevision(revision);
  content.source_generation = SourceGeneration(source_generation);
  content.source_epoch = SourceEpoch(source_epoch);
  content.target = SequenceNumber(target);
  content.interval_start = TimeTicks(interval_start);
  content.interval_end = TimeTicks(interval_end);
  content.observed_at = TimeTicks(observed_at);

  auto valid = validate_content(content);
  if (!valid) {
    return valid.error();
  }
  return content;
}

Expected<EntryContent> decode_content(const std::vector<std::uint8_t>& bytes) {
  return decode_content(bytes.data(), bytes.size());
}

}  // namespace energy_ledger
