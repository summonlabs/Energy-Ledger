// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "energy_ledger/canonical.hpp"
#include "energy_ledger/entry.hpp"

namespace energy_ledger {

const char* to_string(EntryKind kind) noexcept {
  switch (kind) {
    case EntryKind::Delivered: return "delivered";
    case EntryKind::Consumed: return "consumed";
    case EntryKind::WastedLost: return "wasted-lost";
    case EntryKind::Unclassified: return "unclassified";
    case EntryKind::ReservationCommit: return "reservation-commit";
    case EntryKind::Curtailed: return "curtailed";
    case EntryKind::Correction: return "correction";
    case EntryKind::Supersession: return "supersession";
    case EntryKind::Compensating: return "compensating";
  }
  return "unknown";
}

bool is_valid_entry_kind(std::uint8_t value) noexcept {
  return value >= static_cast<std::uint8_t>(EntryKind::Delivered) &&
         value <= static_cast<std::uint8_t>(EntryKind::Compensating);
}

Expected<EntryKind> parse_entry_kind(std::string_view text) {
  if (text == "delivered") return EntryKind::Delivered;
  if (text == "consumed") return EntryKind::Consumed;
  if (text == "wasted-lost") return EntryKind::WastedLost;
  if (text == "unclassified") return EntryKind::Unclassified;
  if (text == "reservation-commit") return EntryKind::ReservationCommit;
  if (text == "curtailed") return EntryKind::Curtailed;
  if (text == "correction") return EntryKind::Correction;
  if (text == "supersession") return EntryKind::Supersession;
  if (text == "compensating") return EntryKind::Compensating;
  return make_error(StatusCode::InvalidArgument, "unknown entry kind '" + std::string(text) + "'");
}

const char* to_string(AccountingDomain domain) noexcept {
  switch (domain) {
    case AccountingDomain::MeasuredFlow: return "measured-flow";
    case AccountingDomain::Commitment: return "commitment";
    case AccountingDomain::Adjustment: return "adjustment";
  }
  return "unknown";
}

AccountingDomain domain_of(EntryKind kind) noexcept {
  switch (kind) {
    case EntryKind::Delivered:
    case EntryKind::Consumed:
    case EntryKind::WastedLost:
    case EntryKind::Unclassified:
      return AccountingDomain::MeasuredFlow;
    case EntryKind::ReservationCommit:
    case EntryKind::Curtailed:
      return AccountingDomain::Commitment;
    case EntryKind::Correction:
    case EntryKind::Supersession:
    case EntryKind::Compensating:
      return AccountingDomain::Adjustment;
  }
  return AccountingDomain::MeasuredFlow;
}

bool asserts_quantity(EntryKind kind) noexcept {
  return kind != EntryKind::Correction && kind != EntryKind::Compensating;
}

bool is_adjustment(EntryKind kind) noexcept {
  return kind == EntryKind::Correction || kind == EntryKind::Supersession ||
         kind == EntryKind::Compensating;
}

const char* to_string(ObservationQuality quality) noexcept {
  switch (quality) {
    case ObservationQuality::Unknown: return "unknown";
    case ObservationQuality::Verified: return "verified";
    case ObservationQuality::Estimated: return "estimated";
    case ObservationQuality::Synthetic: return "synthetic";
    case ObservationQuality::Rejected: return "rejected";
  }
  return "unknown";
}

bool is_valid_quality(std::uint8_t value) noexcept {
  return value <= static_cast<std::uint8_t>(ObservationQuality::Rejected);
}

Expected<ObservationQuality> parse_observation_quality(std::string_view text) {
  if (text == "unknown") return ObservationQuality::Unknown;
  if (text == "verified") return ObservationQuality::Verified;
  if (text == "estimated") return ObservationQuality::Estimated;
  if (text == "synthetic") return ObservationQuality::Synthetic;
  if (text == "rejected") return ObservationQuality::Rejected;
  return make_error(StatusCode::InvalidArgument,
                    "unknown observation quality '" + std::string(text) + "'");
}

const char* to_string(FreshnessState state) noexcept {
  switch (state) {
    case FreshnessState::NotEvaluated: return "not-evaluated";
    case FreshnessState::Fresh: return "fresh";
    case FreshnessState::Stale: return "stale";
    case FreshnessState::UnknownTimeBasis: return "unknown-time-basis";
    case FreshnessState::UnknownObservationTime: return "unknown-observation-time";
    case FreshnessState::DomainMismatch: return "clock-domain-mismatch";
  }
  return "unknown";
}

const char* to_string(ResolutionState state) noexcept {
  switch (state) {
    case ResolutionState::Authoritative: return "authoritative";
    case ResolutionState::DuplicateReplica: return "duplicate-replica";
    case ResolutionState::Shadowed: return "shadowed";
    case ResolutionState::Conflicted: return "conflicted";
    case ResolutionState::Superseded: return "superseded";
    case ResolutionState::Retired: return "retired";
    case ResolutionState::Unauthorized: return "unauthorized";
    case ResolutionState::AdjustmentApplied: return "adjustment-applied";
    case ResolutionState::AdjustmentUnapplied: return "adjustment-unapplied";
  }
  return "unknown";
}

bool operator==(const EntryContent& a, const EntryContent& b) {
  return a.kind == b.kind && a.quality == b.quality && a.authority_tier == b.authority_tier &&
         a.authority == b.authority && a.time_basis == b.time_basis &&
         a.source_revision == b.source_revision &&
         a.source_generation == b.source_generation && a.source_epoch == b.source_epoch &&
         a.target == b.target && a.quantity == b.quantity &&
         a.declared_value == b.declared_value && a.declared_unit == b.declared_unit &&
         a.interval_start == b.interval_start && a.interval_end == b.interval_end &&
         a.observed_at == b.observed_at && a.object == b.object && a.generation == b.generation &&
         a.interval == b.interval && a.clock_domain == b.clock_domain &&
         a.source_family == b.source_family && a.source_instance == b.source_instance &&
         a.annotations == b.annotations;
}

bool operator<(const AccountingKey& a, const AccountingKey& b) noexcept {
  if (a.object != b.object) return a.object < b.object;
  if (a.generation != b.generation) return a.generation < b.generation;
  if (a.interval != b.interval) return a.interval < b.interval;
  if (a.clock_domain != b.clock_domain) return a.clock_domain < b.clock_domain;
  return static_cast<std::uint8_t>(a.kind) < static_cast<std::uint8_t>(b.kind);
}

AccountingKey accounting_key_of(const EntryContent& content) {
  AccountingKey key;
  key.object = content.object;
  key.generation = content.generation;
  key.interval = content.interval;
  key.clock_domain = content.clock_domain;
  key.kind = content.kind;
  return key;
}

Expected<void> validate_content(const EntryContent& content) {
  // Validation precedence is fixed and documented; the first failing step
  // determines the reported status code.
  if (!is_valid_entry_kind(static_cast<std::uint8_t>(content.kind))) {
    return make_error(StatusCode::InvalidArgument, "entry kind is not a known enumerator");
  }
  if (!is_valid_quality(static_cast<std::uint8_t>(content.quality))) {
    return make_error(StatusCode::InvalidArgument, "observation quality is not a known enumerator");
  }
  if (!is_valid_time_basis(static_cast<std::uint8_t>(content.time_basis))) {
    return make_error(StatusCode::InvalidTimeBasis, "time basis is not a known enumerator");
  }
  if (!is_valid_energy_unit(static_cast<std::uint8_t>(content.declared_unit))) {
    return make_error(StatusCode::InvalidUnit, "declared unit is not a known enumerator");
  }

  struct RefCheck {
    const char* name;
    const std::string& value;
  };
  const RefCheck refs[] = {
      {"object", content.object.value()},
      {"generation", content.generation.value()},
      {"interval", content.interval.value()},
      {"clock-domain", content.clock_domain.value()},
      {"source-family", content.source_family.value()},
      {"source-instance", content.source_instance.value()},
      {"authority", content.authority.value()},
  };
  for (const RefCheck& check : refs) {
    auto validated = validate_opaque_ref(check.value);
    if (!validated) {
      return make_error(StatusCode::InvalidRef,
                        std::string(check.name) + " reference: " + validated.error().detail);
    }
  }

  if (content.time_basis == TimeBasis::Unspecified) {
    if (content.interval_start.is_known() || content.interval_end.is_known()) {
      return make_error(StatusCode::InvalidInterval,
                        "interval bounds must be unknown when the time basis is unspecified");
    }
    if (content.observed_at.is_known()) {
      return make_error(StatusCode::InvalidTimeBasis,
                        "observation time must be unknown when the time basis is unspecified");
    }
  } else {
    if (content.interval_start.is_unknown() || content.interval_end.is_unknown()) {
      return make_error(StatusCode::InvalidInterval,
                        "interval bounds must be observed when a time basis is declared");
    }
    if (content.interval_end < content.interval_start) {
      return make_error(StatusCode::InvalidInterval, "interval end precedes interval start");
    }
  }

  if (asserts_quantity(content.kind) && content.quantity.is_negative()) {
    return make_error(StatusCode::QuantitySignNotAllowed,
                      std::string("entry kind '") + to_string(content.kind) +
                          "' must not carry a negative quantity");
  }

  auto declared = to_canonical_energy(content.declared_value, content.declared_unit);
  if (!declared) {
    return declared.error();
  }
  if (declared.value() != content.quantity) {
    return make_error(StatusCode::InconsistentDeclaredQuantity,
                      "declared value " + std::to_string(content.declared_value) + " " +
                          to_string(content.declared_unit) + " does not equal the canonical quantity " +
                          std::to_string(content.quantity.joules()) + " J");
  }

  if (is_adjustment(content.kind)) {
    if (content.target.is_zero()) {
      return make_error(StatusCode::InvalidTarget,
                        std::string("entry kind '") + to_string(content.kind) +
                            "' requires a target sequence");
    }
  } else if (!content.target.is_zero()) {
    return make_error(StatusCode::InvalidTarget,
                      std::string("entry kind '") + to_string(content.kind) +
                          "' must not carry a target sequence");
  }

  if (content.annotations.size() > ModelLimits::kMaxAnnotations) {
    return make_error(StatusCode::InvalidAnnotation,
                      "annotation count exceeds " +
                          std::to_string(ModelLimits::kMaxAnnotations));
  }
  for (std::size_t index = 0; index < content.annotations.size(); ++index) {
    const Annotation& annotation = content.annotations[index];
    if (annotation.key.empty() || annotation.key.size() > ModelLimits::kMaxAnnotationKeyBytes) {
      return make_error(StatusCode::InvalidAnnotation,
                        "annotation key must be 1.." +
                            std::to_string(ModelLimits::kMaxAnnotationKeyBytes) + " bytes");
    }
    if (annotation.value.size() > ModelLimits::kMaxAnnotationValueBytes) {
      return make_error(StatusCode::InvalidAnnotation,
                        "annotation value exceeds " +
                            std::to_string(ModelLimits::kMaxAnnotationValueBytes) + " bytes");
    }
    for (char character : annotation.key) {
      const auto byte = static_cast<unsigned char>(character);
      if (byte < 0x20u || byte > 0x7Eu) {
        return make_error(StatusCode::InvalidAnnotation,
                          "annotation key must be printable ASCII");
      }
    }
    for (char character : annotation.value) {
      const auto byte = static_cast<unsigned char>(character);
      if (byte < 0x20u || byte > 0x7Eu) {
        return make_error(StatusCode::InvalidAnnotation,
                          "annotation value must be printable ASCII");
      }
    }
    if (index > 0) {
      const Annotation& previous = content.annotations[index - 1];
      if (previous.key == annotation.key) {
        return make_error(StatusCode::InvalidAnnotation,
                          "annotation key '" + annotation.key + "' appears more than once");
      }
      if (annotation.key < previous.key) {
        return make_error(StatusCode::InvalidAnnotation,
                          "annotations must be sorted by key");
      }
    }
  }

  return Expected<void>();
}

Expected<Digest256> content_digest(const EntryContent& content) {
  auto encoded = encode_content(content);
  if (!encoded) {
    return encoded.error();
  }
  const std::vector<std::uint8_t>& bytes = encoded.value();
  return Sha256::hash_domain(kContentDomain, bytes.data(), bytes.size());
}

Expected<EventId> event_identity(const EntryContent& content) {
  auto digest = content_digest(content);
  if (!digest) {
    return digest.error();
  }
  auto encoded = encode_content(content);
  if (!encoded) {
    return encoded.error();
  }
  const std::vector<std::uint8_t>& bytes = encoded.value();
  const Digest256 identity = Sha256::hash_domain(kEventIdDomain, bytes.data(), bytes.size());
  EventId::Storage storage{};
  for (std::size_t index = 0; index < storage.size(); ++index) {
    storage[index] = identity.storage()[index];
  }
  return EventId(storage);
}

}  // namespace energy_ledger
