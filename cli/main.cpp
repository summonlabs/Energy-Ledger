// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// energy-ledger: inspection and administration CLI. Every command calls the
// library; the CLI never reimplements accounting, framing or verification.

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "energy_ledger/energy_ledger.hpp"
#include "energy_ledger/test_support.hpp"

namespace {

using namespace energy_ledger;

constexpr int kExitOk = 0;
constexpr int kExitUsage = 1;
constexpr int kExitLibraryError = 2;
constexpr int kExitBusy = 3;
constexpr int kExitVerificationFailed = 4;

struct Args {
  std::vector<std::string> positional;
  std::map<std::string, std::string> options;
  std::set<std::string> flags;

  bool has(const std::string& name) const {
    return options.find(name) != options.end() || flags.find(name) != flags.end();
  }
  std::string get(const std::string& name, const std::string& fallback = std::string()) const {
    auto found = options.find(name);
    return found == options.end() ? fallback : found->second;
  }
  std::int64_t get_int(const std::string& name, std::int64_t fallback) const {
    auto found = options.find(name);
    if (found == options.end()) {
      return fallback;
    }
    try {
      std::size_t consumed = 0;
      const long long value = std::stoll(found->second, &consumed);
      if (consumed != found->second.size()) {
        return fallback;
      }
      return static_cast<std::int64_t>(value);
    } catch (...) {
      return fallback;
    }
  }
  bool get_flag(const std::string& name) const { return flags.find(name) != flags.end(); }
};

Args parse_args(int argc, char** argv, int start) {
  Args args;
  for (int index = start; index < argc; ++index) {
    const std::string token = argv[index];
    if (token.size() > 2 && token.compare(0, 2, "--") == 0) {
      const std::string name = token.substr(2);
      if (index + 1 < argc) {
        const std::string next = argv[index + 1];
        if (!(next.size() > 2 && next.compare(0, 2, "--") == 0)) {
          args.options[name] = next;
          ++index;
          continue;
        }
      }
      args.flags.insert(name);
      continue;
    }
    args.positional.push_back(token);
  }
  return args;
}

int fail(const Error& error) {
  std::cerr << "error: " << to_string(error.code) << ": " << error.detail << "\n";
  return error.code == StatusCode::StoreBusy ? kExitBusy : kExitLibraryError;
}

void print_usage() {
  std::cout <<
      R"(energy-ledger - durable accounting ledger for facility electrical energy

usage: energy-ledger <command> [options]

store lifecycle
  init <store> [--force]                  create a store (--force opens an existing one)
  open <store> [--read-only]              open, replay and report the committed head
  verify <store> [--shallow] [--json]     verify integrity without holding writer authority
  audit <store> [--json]                  store inventory: slots, segments, lease, residue
  format [--json]                         print the durable format description

accounting
  append <store> --record <file.json>     append one entry from a JSON record
  import <store> --records <file.jsonl>   append every JSON record on stdin or in a file
  inspect <store> --sequence N | --event-id HEX [--json]
  rollup <store> --object REF [--generation G] [--interval I] [--json]
  reconcile <store> --object REF [--generation G] [--interval I] [--json]
  provenance <store> --sequence N [--json]
  history <store> [--object REF] [--event-id HEX] [--from N --to M] [--limit N] [--json]
  correct <store> --target N --record <file.json> [--mode correction|supersession|compensating]

common options
  --eval-time T --max-age MS --clock-domain D --ticks-per-second N
      evaluate observation freshness for the query
  --request-id ID        idempotency key for append/import
  --expected-head N      refuse the append unless the committed head is N
  --expected-generation N
  --quiet                suppress informational output

exit codes: 0 ok, 1 usage, 2 library error, 3 store busy, 4 verification failed)";
  std::cout << "\n";
}

Expected<EnergyUnit> unit_from(std::string_view text) { return parse_energy_unit(text); }

Expected<EntryContent> content_from_json(const JsonValue& json) {
  if (!json.is_object()) {
    return make_error(StatusCode::MalformedInput, "record must be a JSON object");
  }
  EntryContent content;

  auto kind_text = json.require_string("kind");
  if (!kind_text) return kind_text.error();
  auto kind = parse_entry_kind(kind_text.value());
  if (!kind) return kind.error();
  content.kind = kind.value();

  auto object = json.require_string("object");
  if (!object) return object.error();
  auto object_ref = FacilityObjectRef::parse(object.value());
  if (!object_ref) return object_ref.error();
  content.object = object_ref.value();

  auto generation = json.require_string("generation");
  if (!generation) return generation.error();
  auto generation_ref = GenerationRef::parse(generation.value());
  if (!generation_ref) return generation_ref.error();
  content.generation = generation_ref.value();

  auto interval = json.require_string("interval");
  if (!interval) return interval.error();
  auto interval_ref = IntervalRef::parse(interval.value());
  if (!interval_ref) return interval_ref.error();
  content.interval = interval_ref.value();

  auto clock_domain = json.require_string("clock-domain");
  if (!clock_domain) return clock_domain.error();
  auto clock_ref = ClockDomainRef::parse(clock_domain.value());
  if (!clock_ref) return clock_ref.error();
  content.clock_domain = clock_ref.value();

  auto family = json.require_string("source-family");
  if (!family) return family.error();
  auto family_ref = SourceFamilyRef::parse(family.value());
  if (!family_ref) return family_ref.error();
  content.source_family = family_ref.value();

  auto instance = json.require_string("source-instance");
  if (!instance) return instance.error();
  auto instance_ref = SourceInstanceRef::parse(instance.value());
  if (!instance_ref) return instance_ref.error();
  content.source_instance = instance_ref.value();

  auto authority = json.require_string("authority");
  if (!authority) return authority.error();
  auto authority_ref = AuthorityRef::parse(authority.value());
  if (!authority_ref) return authority_ref.error();
  content.authority = authority_ref.value();

  const JsonValue* basis = json.find("time-basis");
  if (basis != nullptr && !basis->is_null()) {
    if (!basis->is_string()) {
      return make_error(StatusCode::MalformedInput, "time-basis must be a string");
    }
    auto parsed = parse_time_basis(basis->as_string());
    if (!parsed) return parsed.error();
    content.time_basis = parsed.value();
  }

  const JsonValue* quality = json.find("quality");
  if (quality != nullptr && !quality->is_null()) {
    if (!quality->is_string()) {
      return make_error(StatusCode::MalformedInput, "quality must be a string");
    }
    auto parsed = parse_observation_quality(quality->as_string());
    if (!parsed) return parsed.error();
    content.quality = parsed.value();
  }

  auto tier = json.optional_integer("authority-tier", 0);
  if (!tier) return tier.error();
  auto authority_tier = AuthorityTier::create(static_cast<std::uint32_t>(tier.value()));
  if (!authority_tier) return authority_tier.error();
  content.authority_tier = authority_tier.value();

  auto revision = json.optional_integer("source-revision", 0);
  if (!revision) return revision.error();
  content.source_revision = SourceRevision(static_cast<std::uint64_t>(revision.value()));
  auto source_generation = json.optional_integer("source-generation", 0);
  if (!source_generation) return source_generation.error();
  content.source_generation = SourceGeneration(static_cast<std::uint64_t>(source_generation.value()));
  auto source_epoch = json.optional_integer("source-epoch", 0);
  if (!source_epoch) return source_epoch.error();
  content.source_epoch = SourceEpoch(static_cast<std::uint64_t>(source_epoch.value()));
  auto target = json.optional_integer("target", 0);
  if (!target) return target.error();
  content.target = SequenceNumber(static_cast<std::uint64_t>(target.value()));
  auto start = json.optional_integer("interval-start", TimeTicks::kUnknownValue);
  if (!start) return start.error();
  content.interval_start = TimeTicks(start.value());
  auto end = json.optional_integer("interval-end", TimeTicks::kUnknownValue);
  if (!end) return end.error();
  content.interval_end = TimeTicks(end.value());
  auto observed = json.optional_integer("observed-at", TimeTicks::kUnknownValue);
  if (!observed) return observed.error();
  content.observed_at = TimeTicks(observed.value());

  auto declared = json.require_integer("quantity");
  if (!declared) return declared.error();
  auto unit_text = json.require_string("unit");
  if (!unit_text) return unit_text.error();
  auto unit = unit_from(unit_text.value());
  if (!unit) return unit.error();
  content.declared_value = declared.value();
  content.declared_unit = unit.value();
  auto canonical = to_canonical_energy(declared.value(), unit.value());
  if (!canonical) return canonical.error();
  content.quantity = canonical.value();

  const JsonValue* annotations = json.find("annotations");
  if (annotations != nullptr && !annotations->is_null()) {
    if (!annotations->is_object()) {
      return make_error(StatusCode::MalformedInput, "annotations must be an object");
    }
    for (const auto& member : annotations->members()) {
      if (!member.second.is_string()) {
        return make_error(StatusCode::MalformedInput, "annotation values must be strings");
      }
      Annotation annotation;
      annotation.key = member.first;
      annotation.value = member.second.as_string();
      content.annotations.push_back(std::move(annotation));
    }
    std::sort(content.annotations.begin(), content.annotations.end());
    for (std::size_t index = 1; index < content.annotations.size(); ++index) {
      if (content.annotations[index].key == content.annotations[index - 1].key) {
        return make_error(StatusCode::InvalidAnnotation, "duplicate annotation key");
      }
    }
  }

  auto valid = validate_content(content);
  if (!valid) {
    return valid.error();
  }
  return content;
}

Expected<RollupQuery> query_from_json_args(const Args& args, const std::string& object) {
  RollupQuery query;
  auto object_ref = FacilityObjectRef::parse(object);
  if (!object_ref) {
    return object_ref.error();
  }
  query.object = object_ref.value();
  if (args.has("generation")) {
    auto generation = GenerationRef::parse(args.get("generation"));
    if (!generation) return generation.error();
    query.all_generations = false;
    query.generation = generation.value();
  }
  if (args.has("interval")) {
    auto interval = IntervalRef::parse(args.get("interval"));
    if (!interval) return interval.error();
    query.all_intervals = false;
    query.interval = interval.value();
  }
  query.options.max_contributors = static_cast<std::size_t>(args.get_int("max-contributors", 64));
  if (args.has("eval-time")) {
    query.options.has_evaluation_time = true;
    query.options.evaluation_time = TimeTicks(args.get_int("eval-time", 0));
    if (args.has("max-age")) {
      auto age = Duration::from_milliseconds(args.get_int("max-age", 0));
      if (!age) return age.error();
      query.options.has_max_age = true;
      query.options.max_age = age.value();
    }
    if (args.has("clock-domain")) {
      auto domain = ClockDomainRef::parse(args.get("clock-domain"));
      if (!domain) return domain.error();
      query.options.clock_domain = domain.value();
    }
    if (args.has("time-basis")) {
      auto basis = parse_time_basis(args.get("time-basis"));
      if (!basis) return basis.error();
      query.options.time_basis = basis.value();
    }
    query.options.ticks_per_second =
        static_cast<std::uint64_t>(std::max<std::int64_t>(args.get_int("ticks-per-second", 0), 0));
  }
  return query;
}

JsonValue digest_json(const Digest256& digest) { return JsonValue::string(digest.to_hex()); }

JsonValue content_json(const EntryContent& content) {
  JsonValue json = JsonValue::object();
  json.set("kind", JsonValue::string(to_string(content.kind)));
  json.set("domain", JsonValue::string(to_string(domain_of(content.kind))));
  json.set("object", JsonValue::string(content.object.value()));
  json.set("generation", JsonValue::string(content.generation.value()));
  json.set("interval", JsonValue::string(content.interval.value()));
  json.set("clock-domain", JsonValue::string(content.clock_domain.value()));
  json.set("time-basis", JsonValue::string(to_string(content.time_basis)));
  json.set("interval-start", JsonValue::integer(content.interval_start.value()));
  json.set("interval-end", JsonValue::integer(content.interval_end.value()));
  json.set("observed-at", JsonValue::integer(content.observed_at.value()));
  json.set("quantity-joules", JsonValue::integer(content.quantity.joules()));
  json.set("declared-value", JsonValue::integer(content.declared_value));
  json.set("declared-unit", JsonValue::string(to_string(content.declared_unit)));
  json.set("quality", JsonValue::string(to_string(content.quality)));
  json.set("authority", JsonValue::string(content.authority.value()));
  json.set("authority-tier", JsonValue::integer(content.authority_tier.value()));
  json.set("source-family", JsonValue::string(content.source_family.value()));
  json.set("source-instance", JsonValue::string(content.source_instance.value()));
  json.set("source-revision", JsonValue::integer(static_cast<std::int64_t>(content.source_revision.value())));
  json.set("source-generation", JsonValue::integer(static_cast<std::int64_t>(content.source_generation.value())));
  json.set("source-epoch", JsonValue::integer(static_cast<std::int64_t>(content.source_epoch.value())));
  json.set("target", JsonValue::integer(static_cast<std::int64_t>(content.target.value())));
  JsonValue annotations = JsonValue::object();
  for (const Annotation& annotation : content.annotations) {
    annotations.set(annotation.key, JsonValue::string(annotation.value));
  }
  json.set("annotations", std::move(annotations));
  return json;
}

void print_content(const EntryContent& content) {
  std::cout << to_pretty_json(content_json(content)) << "\n";
}

JsonValue result_json(const AppendResult& result) {
  JsonValue json = JsonValue::object();
  json.set("disposition", JsonValue::string(to_string(result.disposition)));
  json.set("sequence", JsonValue::integer(static_cast<std::int64_t>(result.sequence.value())));
  json.set("event-id", JsonValue::string(result.event_id.to_hex()));
  json.set("content-digest", digest_json(result.content_digest));
  json.set("chain-hash", digest_json(result.chain_hash));
  json.set("generation", JsonValue::integer(static_cast<std::int64_t>(result.generation.value())));
  json.set("head", JsonValue::integer(static_cast<std::int64_t>(result.head.value())));
  json.set("replayed", JsonValue::boolean(result.replayed()));
  return json;
}

void print_header_line(const std::string& store) {
  auto audit = audit_store(store);
  if (!audit) {
    return;
  }
  std::cout << "store=" << store << "\n";
  std::cout << "generation=" << audit.value().generation.value() << "\n";
  std::cout << "head-sequence=" << audit.value().head_sequence.value() << "\n";
  std::cout << "entries=" << audit.value().entry_count << "\n";
  std::cout << "incarnation=" << audit.value().incarnation.to_hex() << "\n";
}

int command_init(const Args& args) {
  if (args.positional.empty()) {
    std::cerr << "error: init requires a store path\n";
    return kExitUsage;
  }
  CreateOptions options;
  options.fail_if_exists = !args.get_flag("force");
  auto ledger = Ledger::create(args.positional[0], options);
  if (!ledger) {
    return fail(ledger.error());
  }
  std::cout << "initialized store=" << ledger.value().path()
            << " incarnation=" << ledger.value().incarnation().to_hex()
            << " store-uuid=" << ledger.value().store_uuid().to_hex()
            << " generation=" << ledger.value().generation().value() << "\n";
  auto closed = ledger.value().close();
  if (!closed) {
    return fail(closed.error());
  }
  return kExitOk;
}

int command_open(const Args& args) {
  if (args.positional.empty()) {
    std::cerr << "error: open requires a store path\n";
    return kExitUsage;
  }
  OpenOptions options;
  options.mode = args.get_flag("read-only") ? AccessMode::ReadOnly : AccessMode::ReadWrite;
  options.recover_lease = args.get_flag("recover-lease");
  auto ledger = Ledger::open(args.positional[0], options);
  if (!ledger) {
    return fail(ledger.error());
  }
  auto summary = ledger.value().replay();
  if (!summary) {
    return fail(summary.error());
  }
  if (args.get_flag("json")) {
    JsonValue json = JsonValue::object();
    json.set("store", JsonValue::string(ledger.value().path()));
    json.set("mode", JsonValue::string(options.mode == AccessMode::ReadOnly ? "read-only" : "read-write"));
    json.set("incarnation", JsonValue::string(summary.value().incarnation.to_hex()));
    json.set("generation", JsonValue::integer(static_cast<std::int64_t>(summary.value().generation.value())));
    json.set("head-sequence", JsonValue::integer(static_cast<std::int64_t>(summary.value().head_sequence.value())));
    json.set("entries-replayed", JsonValue::integer(static_cast<std::int64_t>(summary.value().entries_replayed)));
    json.set("chain-head", digest_json(summary.value().chain_head));
    json.set("conflicted-keys", JsonValue::integer(static_cast<std::int64_t>(summary.value().conflicted_keys)));
    json.set("retired-entries", JsonValue::integer(static_cast<std::int64_t>(summary.value().retired_entries)));
    json.set("writer-epoch", JsonValue::integer(static_cast<std::int64_t>(ledger.value().writer_epoch().value())));
    std::cout << to_pretty_json(json) << "\n";
  } else {
    print_header_line(ledger.value().path());
    std::cout << "mode=" << (options.mode == AccessMode::ReadOnly ? "read-only" : "read-write") << "\n";
    std::cout << "entries-replayed=" << summary.value().entries_replayed << "\n";
    std::cout << "chain-head=" << summary.value().chain_head.to_hex() << "\n";
    std::cout << "conflicted-keys=" << summary.value().conflicted_keys << "\n";
    std::cout << "writer-epoch=" << ledger.value().writer_epoch().value() << "\n";
    auto audit = ledger.value().audit();
    if (audit.has_value()) {
      std::cout << "lease-recovered=" << (audit.value().lease_recovered ? "true" : "false") << "\n";
    }
  }
  auto closed = ledger.value().close();
  if (!closed) {
    return fail(closed.error());
  }
  return kExitOk;
}

Expected<AppendRequest> request_from_file(const Args& args, const std::string& path) {
  auto bytes = test_support::read_file_bytes(path);
  if (!bytes) {
    return bytes.error();
  }
  const std::string text(reinterpret_cast<const char*>(bytes.value().data()), bytes.value().size());
  auto json = parse_json(text);
  if (!json) {
    return json.error();
  }
  auto content = content_from_json(json.value());
  if (!content) {
    return content.error();
  }
  AppendRequest request;
  request.content = content.value();
  request.request_id = args.get("request-id");
  if (!args.get("attempt").empty()) {
    request.attempt = AttemptNumber(static_cast<std::uint64_t>(args.get_int("attempt", 0)));
  }
  if (args.has("expected-head")) {
    request.has_expected_head = true;
    request.expected_head = SequenceNumber(static_cast<std::uint64_t>(args.get_int("expected-head", 0)));
  }
  if (args.has("expected-generation")) {
    request.has_expected_generation = true;
    request.expected_generation =
        ManifestGeneration(static_cast<std::uint64_t>(args.get_int("expected-generation", 0)));
  }
  if (args.has("crash-stage")) {
    auto stage = parse_fault_stage(args.get("crash-stage"));
    if (!stage) {
      return stage.error();
    }
    request.fault.terminate_at = stage.value();
    request.fault.exit_code = static_cast<int>(args.get_int("crash-code", test_support::kCrashExitCode));
  }
  return request;
}

int command_append(const Args& args) {
  if (args.positional.empty() || !args.has("record")) {
    std::cerr << "error: append requires a store path and --record <file.json>\n";
    return kExitUsage;
  }
  auto request = request_from_file(args, args.get("record"));
  if (!request) {
    return fail(request.error());
  }
  OpenOptions options;
  options.mode = AccessMode::ReadWrite;
  auto ledger = Ledger::open(args.positional[0], options);
  if (!ledger) {
    return fail(ledger.error());
  }
  auto result = ledger.value().append(request.value());
  if (!result) {
    return fail(result.error());
  }
  if (args.get_flag("json")) {
    std::cout << to_pretty_json(result_json(result.value())) << "\n";
  } else {
    std::cout << "disposition=" << to_string(result.value().disposition) << "\n";
    std::cout << "sequence=" << result.value().sequence.value() << "\n";
    std::cout << "event-id=" << result.value().event_id.to_hex() << "\n";
    std::cout << "content-digest=" << result.value().content_digest.to_hex() << "\n";
    std::cout << "chain-hash=" << result.value().chain_hash.to_hex() << "\n";
    std::cout << "generation=" << result.value().generation.value() << "\n";
  }
  auto closed = ledger.value().close();
  if (!closed) {
    return fail(closed.error());
  }
  return kExitOk;
}

int command_import(const Args& args) {
  if (args.positional.empty() || !args.has("records")) {
    std::cerr << "error: import requires a store path and --records <file.jsonl>\n";
    return kExitUsage;
  }
  auto bytes = test_support::read_file_bytes(args.get("records"));
  if (!bytes) {
    return fail(bytes.error());
  }
  const std::string text(reinterpret_cast<const char*>(bytes.value().data()), bytes.value().size());
  OpenOptions options;
  options.mode = AccessMode::ReadWrite;
  auto ledger = Ledger::open(args.positional[0], options);
  if (!ledger) {
    return fail(ledger.error());
  }
  std::size_t line_number = 0;
  std::size_t accepted = 0;
  std::size_t replayed = 0;
  std::size_t start = 0;
  while (start <= text.size()) {
    std::size_t end = text.find('\n', start);
    if (end == std::string::npos) {
      end = text.size();
    }
    std::string line = text.substr(start, end - start);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
      line.pop_back();
    }
    start = end + 1;
    if (line.empty()) {
      if (end >= text.size()) {
        break;
      }
      continue;
    }
    ++line_number;
    auto json = parse_json(line);
    if (!json) {
      std::cerr << "error: line " << line_number << ": " << json.error().detail << "\n";
      return kExitLibraryError;
    }
    auto content = content_from_json(json.value());
    if (!content) {
      std::cerr << "error: line " << line_number << ": " << content.error().detail << "\n";
      return kExitLibraryError;
    }
    AppendRequest request;
    request.content = content.value();
    const JsonValue* request_id = json.value().find("request-id");
    if (request_id != nullptr && request_id->is_string()) {
      request.request_id = request_id->as_string();
    } else if (args.has("request-id")) {
      request.request_id = args.get("request-id") + "-" + std::to_string(line_number);
    }
    auto result = ledger.value().append(request);
    if (!result) {
      std::cerr << "error: line " << line_number << ": " << to_string(result.error().code) << ": "
                << result.error().detail << "\n";
      return kExitLibraryError;
    }
    if (result.value().replayed()) {
      ++replayed;
    } else {
      ++accepted;
    }
    if (!args.get_flag("quiet")) {
      std::cout << "line=" << line_number << " sequence=" << result.value().sequence.value()
                << " disposition=" << to_string(result.value().disposition) << "\n";
    }
    if (end >= text.size()) {
      break;
    }
  }
  std::cout << "accepted=" << accepted << " replayed=" << replayed
            << " head=" << ledger.value().head_sequence().value() << "\n";
  auto closed = ledger.value().close();
  if (!closed) {
    return fail(closed.error());
  }
  return kExitOk;
}

int command_inspect(const Args& args) {
  if (args.positional.empty()) {
    std::cerr << "error: inspect requires a store path\n";
    return kExitUsage;
  }
  OpenOptions options;
  options.mode = AccessMode::ReadOnly;
  auto ledger = Ledger::open(args.positional[0], options);
  if (!ledger) {
    return fail(ledger.error());
  }
  Expected<EntryView> view = make_error(StatusCode::InvalidArgument, "unreachable");
  if (args.has("sequence")) {
    view = ledger.value().inspect(
        SequenceNumber(static_cast<std::uint64_t>(args.get_int("sequence", 0))));
  } else if (args.has("event-id")) {
    auto id = EventId::from_hex(args.get("event-id"));
    if (!id) {
      return fail(id.error());
    }
    view = ledger.value().find_by_event_id(id.value());
  } else {
    std::cerr << "error: inspect requires --sequence or --event-id\n";
    return kExitUsage;
  }
  if (!view) {
    return fail(view.error());
  }
  JsonValue json = JsonValue::object();
  json.set("sequence", JsonValue::integer(static_cast<std::int64_t>(view.value().sequence.value())));
  json.set("incarnation", JsonValue::string(view.value().incarnation.to_hex()));
  json.set("event-id", JsonValue::string(view.value().event_id.to_hex()));
  json.set("content-digest", digest_json(view.value().content_digest));
  json.set("entry-hash", digest_json(view.value().entry_hash));
  json.set("prev-chain-hash", digest_json(view.value().prev_chain_hash));
  json.set("chain-hash", digest_json(view.value().chain_hash));
  json.set("resolution", JsonValue::string(to_string(view.value().state)));
  json.set("segment-id", JsonValue::integer(static_cast<std::int64_t>(view.value().segment_id)));
  json.set("record-offset", JsonValue::integer(static_cast<std::int64_t>(view.value().record_offset)));
  json.set("record-length", JsonValue::integer(view.value().record_length));
  json.set("content", content_json(view.value().content));
  std::cout << to_pretty_json(json) << "\n";
  auto closed = ledger.value().close();
  if (!closed) {
    return fail(closed.error());
  }
  return kExitOk;
}

JsonValue category_json(const CategoryTotal& category) {
  JsonValue json = JsonValue::object();
  json.set("kind", JsonValue::string(to_string(category.kind)));
  json.set("domain", JsonValue::string(to_string(category.domain)));
  json.set("authoritative-joules", JsonValue::integer(category.authoritative.joules()));
  json.set("adjustments-joules", JsonValue::integer(category.adjustments.joules()));
  json.set("total-joules", JsonValue::integer(category.total.joules()));
  json.set("conflicted-keys", JsonValue::integer(static_cast<std::int64_t>(category.conflicted_keys)));
  json.set("conflicted-entries", JsonValue::integer(static_cast<std::int64_t>(category.conflicted_entries)));
  json.set("conflicted-min-joules", JsonValue::integer(category.conflicted_min.joules()));
  json.set("conflicted-max-joules", JsonValue::integer(category.conflicted_max.joules()));
  json.set("shadowed-joules", JsonValue::integer(category.shadowed.joules()));
  json.set("shadowed-entries", JsonValue::integer(static_cast<std::int64_t>(category.shadowed_entries)));
  json.set("duplicate-replicas", JsonValue::integer(static_cast<std::int64_t>(category.duplicate_replicas)));
  json.set("authoritative-keys", JsonValue::integer(static_cast<std::int64_t>(category.authoritative_keys)));
  json.set("stale-entries", JsonValue::integer(static_cast<std::int64_t>(category.stale_entries)));
  json.set("unknown-quality-entries", JsonValue::integer(static_cast<std::int64_t>(category.unknown_quality_entries)));
  json.set("unknown-time-entries", JsonValue::integer(static_cast<std::int64_t>(category.unknown_time_entries)));
  JsonValue contributors = JsonValue::array();
  for (SequenceNumber sequence : category.contributors) {
    contributors.push_back(JsonValue::integer(static_cast<std::int64_t>(sequence.value())));
  }
  json.set("contributors", std::move(contributors));
  json.set("contributors-truncated", JsonValue::boolean(category.contributors_truncated));
  return json;
}

int command_rollup(const Args& args) {
  if (args.positional.empty() || !args.has("object")) {
    std::cerr << "error: rollup requires a store path and --object REF\n";
    return kExitUsage;
  }
  auto query = query_from_json_args(args, args.get("object"));
  if (!query) {
    return fail(query.error());
  }
  OpenOptions options;
  options.mode = AccessMode::ReadOnly;
  auto ledger = Ledger::open(args.positional[0], options);
  if (!ledger) {
    return fail(ledger.error());
  }
  auto result = ledger.value().rollup(query.value());
  if (!result) {
    return fail(result.error());
  }
  JsonValue json = JsonValue::object();
  json.set("object", JsonValue::string(result.value().object.value()));
  json.set("entries-considered", JsonValue::integer(static_cast<std::int64_t>(result.value().entries_considered)));
  json.set("entries-matched", JsonValue::integer(static_cast<std::int64_t>(result.value().entries_matched)));
  JsonValue generations = JsonValue::array();
  for (const GenerationRollup& generation : result.value().generations) {
    JsonValue entry = JsonValue::object();
    entry.set("generation", JsonValue::string(generation.generation.value()));
    JsonValue categories = JsonValue::array();
    for (const CategoryTotal& category : generation.categories) {
      categories.push_back(category_json(category));
    }
    entry.set("categories", std::move(categories));
    generations.push_back(std::move(entry));
  }
  json.set("generations", std::move(generations));
  if (args.get_flag("json")) {
    std::cout << to_pretty_json(json) << "\n";
  } else {
    const JsonValue* list = json.find("generations");
    if (list != nullptr) {
      for (const JsonValue& generation : list->items()) {
        const JsonValue* name = generation.find("generation");
        std::cout << "generation=" << (name != nullptr ? name->as_string() : "") << "\n";
        const JsonValue* categories = generation.find("categories");
        if (categories == nullptr) {
          continue;
        }
        for (const JsonValue& category : categories->items()) {
          const JsonValue* kind = category.find("kind");
          const JsonValue* total = category.find("total-joules");
          const JsonValue* conflicted = category.find("conflicted-keys");
          const JsonValue* contributors = category.find("contributors");
          std::cout << "  " << (kind != nullptr ? kind->as_string() : "?")
                    << " total-joules=" << (total != nullptr ? total->as_integer() : 0)
                    << " conflicted-keys=" << (conflicted != nullptr ? conflicted->as_integer() : 0)
                    << " contributors=";
          if (contributors != nullptr) {
            bool first = true;
            for (const JsonValue& contributor : contributors->items()) {
              if (!first) {
                std::cout << ",";
              }
              first = false;
              std::cout << contributor.as_integer();
            }
          }
          std::cout << "\n";
        }
      }
    }
  }
  auto closed = ledger.value().close();
  if (!closed) {
    return fail(closed.error());
  }
  return kExitOk;
}

int command_reconcile(const Args& args) {
  if (args.positional.empty() || !args.has("object")) {
    std::cerr << "error: reconcile requires a store path and --object REF\n";
    return kExitUsage;
  }
  auto query = query_from_json_args(args, args.get("object"));
  if (!query) {
    return fail(query.error());
  }
  OpenOptions options;
  options.mode = AccessMode::ReadOnly;
  auto ledger = Ledger::open(args.positional[0], options);
  if (!ledger) {
    return fail(ledger.error());
  }
  auto report = ledger.value().reconcile(query.value());
  if (!report) {
    return fail(report.error());
  }
  const Reconciliation& value = report.value();
  JsonValue json = JsonValue::object();
  json.set("object", JsonValue::string(value.object.value()));
  JsonValue measured = JsonValue::object();
  measured.set("delivered-joules", JsonValue::integer(value.delivered.joules()));
  measured.set("consumed-joules", JsonValue::integer(value.consumed.joules()));
  measured.set("wasted-lost-joules", JsonValue::integer(value.wasted_lost.joules()));
  measured.set("unclassified-recorded-joules", JsonValue::integer(value.unclassified_recorded.joules()));
  measured.set("classified-joules", JsonValue::integer(value.measured_classified.joules()));
  measured.set("residual-joules", JsonValue::integer(value.measured_residual.joules()));
  measured.set("residual-is-zero", JsonValue::boolean(value.measured_residual_is_zero));
  json.set("measured-flow", std::move(measured));
  JsonValue commitment = JsonValue::object();
  commitment.set("committed-joules", JsonValue::integer(value.committed.joules()));
  commitment.set("curtailed-joules", JsonValue::integer(value.curtailed.joules()));
  commitment.set("slack-joules", JsonValue::integer(value.commitment_slack.joules()));
  commitment.set("residual-joules", JsonValue::integer(value.commitment_residual.joules()));
  commitment.set("residual-is-zero", JsonValue::boolean(value.commitment_residual_is_zero));
  json.set("commitment", std::move(commitment));
  json.set("conflicted-keys", JsonValue::integer(static_cast<std::int64_t>(value.conflicted_keys)));
  json.set("stale-entries", JsonValue::integer(static_cast<std::int64_t>(value.stale_entries)));
  json.set("unknown-quality-entries", JsonValue::integer(static_cast<std::int64_t>(value.unknown_quality_entries)));
  json.set("complete", JsonValue::boolean(value.complete));
  json.set("completeness-note", JsonValue::string(value.completeness_note));
  JsonValue explanation = JsonValue::array();
  for (const ExplanationLine& line : value.explanation) {
    JsonValue item = JsonValue::object();
    item.set("code", JsonValue::string(line.code));
    item.set("detail", JsonValue::string(line.detail));
    explanation.push_back(std::move(item));
  }
  json.set("explanation", std::move(explanation));
  if (args.get_flag("json")) {
    std::cout << to_pretty_json(json) << "\n";
  } else {
    std::cout << "measured-flow delivered-joules=" << value.delivered.joules()
              << " consumed-joules=" << value.consumed.joules()
              << " wasted-lost-joules=" << value.wasted_lost.joules()
              << " unclassified-joules=" << value.unclassified_recorded.joules() << "\n";
    std::cout << "measured-residual-joules=" << value.measured_residual.joules() << "\n";
    std::cout << "commitment committed-joules=" << value.committed.joules()
              << " curtailed-joules=" << value.curtailed.joules()
              << " slack-joules=" << value.commitment_slack.joules()
              << " residual-joules=" << value.commitment_residual.joules() << "\n";
    std::cout << "conflicted-keys=" << value.conflicted_keys
              << " complete=" << (value.complete ? "true" : "false") << "\n";
    for (const ExplanationLine& line : value.explanation) {
      std::cout << "explain " << line.code << ": " << line.detail << "\n";
    }
  }
  auto closed = ledger.value().close();
  if (!closed) {
    return fail(closed.error());
  }
  return kExitOk;
}

int command_provenance(const Args& args) {
  if (args.positional.empty() || !args.has("sequence")) {
    std::cerr << "error: provenance requires a store path and --sequence N\n";
    return kExitUsage;
  }
  OpenOptions options;
  options.mode = AccessMode::ReadOnly;
  auto ledger = Ledger::open(args.positional[0], options);
  if (!ledger) {
    return fail(ledger.error());
  }
  auto chain = ledger.value().provenance(
      SequenceNumber(static_cast<std::uint64_t>(args.get_int("sequence", 0))));
  if (!chain) {
    return fail(chain.error());
  }
  JsonValue json = JsonValue::object();
  json.set("root", JsonValue::integer(static_cast<std::int64_t>(chain.value().root.value())));
  json.set("truncated", JsonValue::boolean(chain.value().truncated));
  JsonValue nodes = JsonValue::array();
  for (const ProvenanceNode& node : chain.value().nodes) {
    JsonValue item = JsonValue::object();
    item.set("sequence", JsonValue::integer(static_cast<std::int64_t>(node.sequence.value())));
    item.set("kind", JsonValue::string(to_string(node.kind)));
    item.set("state", JsonValue::string(to_string(node.state)));
    item.set("relation", JsonValue::string(node.relation));
    item.set("depth", JsonValue::integer(node.depth));
    item.set("quantity-joules", JsonValue::integer(node.quantity.joules()));
    item.set("source-instance", JsonValue::string(node.source_instance));
    item.set("source-revision", JsonValue::integer(static_cast<std::int64_t>(node.source_revision)));
    item.set("source-epoch", JsonValue::integer(static_cast<std::int64_t>(node.source_epoch)));
    item.set("authority-tier", JsonValue::integer(node.authority_tier));
    item.set("content-digest", digest_json(node.content_digest));
    nodes.push_back(std::move(item));
  }
  json.set("nodes", std::move(nodes));
  if (args.get_flag("json")) {
    std::cout << to_pretty_json(json) << "\n";
  } else {
    for (const ProvenanceNode& node : chain.value().nodes) {
      std::cout << "sequence=" << node.sequence.value() << " relation=" << node.relation
                << " state=" << to_string(node.state) << " kind=" << to_string(node.kind)
                << " quantity-joules=" << node.quantity.joules()
                << " source=" << node.source_instance
                << " revision=" << node.source_revision << "\n";
    }
  }
  auto closed = ledger.value().close();
  if (!closed) {
    return fail(closed.error());
  }
  return kExitOk;
}

int command_history(const Args& args) {
  if (args.positional.empty()) {
    std::cerr << "error: history requires a store path\n";
    return kExitUsage;
  }
  HistoryQuery query;
  query.limit = static_cast<std::size_t>(std::max<std::int64_t>(args.get_int("limit", 128), 1));
  if (args.has("object")) {
    auto object = FacilityObjectRef::parse(args.get("object"));
    if (!object) {
      return fail(object.error());
    }
    query.by_object = true;
    query.object = object.value();
  } else if (args.has("event-id")) {
    auto id = EventId::from_hex(args.get("event-id"));
    if (!id) {
      return fail(id.error());
    }
    query.by_event_id = true;
    query.event = id.value();
  } else if (args.has("from") || args.has("to")) {
    query.by_sequence_range = true;
    query.from = SequenceNumber(static_cast<std::uint64_t>(args.get_int("from", 1)));
    query.to = SequenceNumber(static_cast<std::uint64_t>(args.get_int("to", INT64_MAX)));
  }
  OpenOptions options;
  options.mode = AccessMode::ReadOnly;
  auto ledger = Ledger::open(args.positional[0], options);
  if (!ledger) {
    return fail(ledger.error());
  }
  auto history = ledger.value().history(query);
  if (!history) {
    return fail(history.error());
  }
  JsonValue json = JsonValue::object();
  json.set("total-matching", JsonValue::integer(static_cast<std::int64_t>(history.value().total_matching)));
  json.set("truncated", JsonValue::boolean(history.value().truncated));
  JsonValue entries = JsonValue::array();
  for (const HistoryEntry& entry : history.value().entries) {
    JsonValue item = JsonValue::object();
    item.set("sequence", JsonValue::integer(static_cast<std::int64_t>(entry.sequence.value())));
    item.set("kind", JsonValue::string(to_string(entry.kind)));
    item.set("state", JsonValue::string(to_string(entry.state)));
    item.set("quantity-joules", JsonValue::integer(entry.quantity.joules()));
    item.set("generation", JsonValue::string(entry.generation));
    item.set("interval", JsonValue::string(entry.interval));
    item.set("source-instance", JsonValue::string(entry.source_instance));
    item.set("content-digest", digest_json(entry.content_digest));
    item.set("segment-id", JsonValue::integer(static_cast<std::int64_t>(entry.segment_id)));
    item.set("record-offset", JsonValue::integer(static_cast<std::int64_t>(entry.record_offset)));
    entries.push_back(std::move(item));
  }
  json.set("entries", std::move(entries));
  if (args.get_flag("json")) {
    std::cout << to_pretty_json(json) << "\n";
  } else {
    for (const HistoryEntry& entry : history.value().entries) {
      std::cout << "sequence=" << entry.sequence.value() << " kind=" << to_string(entry.kind)
                << " state=" << to_string(entry.state) << " quantity-joules=" << entry.quantity.joules()
                << " generation=" << entry.generation << " interval=" << entry.interval
                << " source=" << entry.source_instance << "\n";
    }
    std::cout << "total-matching=" << history.value().total_matching
              << " truncated=" << (history.value().truncated ? "true" : "false") << "\n";
  }
  auto closed = ledger.value().close();
  if (!closed) {
    return fail(closed.error());
  }
  return kExitOk;
}

int command_correct(const Args& args) {
  if (args.positional.empty() || !args.has("target") || !args.has("record")) {
    std::cerr << "error: correct requires a store path, --target N and --record <file.json>\n";
    return kExitUsage;
  }
  auto request = request_from_file(args, args.get("record"));
  if (!request) {
    return fail(request.error());
  }
  const std::string mode = args.get("mode", "correction");
  EntryKind kind = EntryKind::Correction;
  if (mode == "correction") {
    kind = EntryKind::Correction;
  } else if (mode == "supersession") {
    kind = EntryKind::Supersession;
  } else if (mode == "compensating") {
    kind = EntryKind::Compensating;
  } else {
    std::cerr << "error: unknown --mode '" << mode << "'\n";
    return kExitUsage;
  }
  request.value().content.kind = kind;
  request.value().content.target =
      SequenceNumber(static_cast<std::uint64_t>(args.get_int("target", 0)));

  OpenOptions options;
  options.mode = AccessMode::ReadWrite;
  auto ledger = Ledger::open(args.positional[0], options);
  if (!ledger) {
    return fail(ledger.error());
  }
  auto target = ledger.value().inspect(request.value().content.target);
  if (!target) {
    return fail(target.error());
  }
  if (request.value().content.object.empty()) {
    request.value().content.object = target.value().content.object;
  }
  if (request.value().content.generation.empty()) {
    request.value().content.generation = target.value().content.generation;
  }
  if (request.value().content.interval.empty()) {
    request.value().content.interval = target.value().content.interval;
  }
  if (request.value().content.clock_domain.empty()) {
    request.value().content.clock_domain = target.value().content.clock_domain;
  }
  if (request.value().content.time_basis == TimeBasis::Unspecified) {
    request.value().content.time_basis = target.value().content.time_basis;
    request.value().content.interval_start = target.value().content.interval_start;
    request.value().content.interval_end = target.value().content.interval_end;
  }
  auto result = ledger.value().append(request.value());
  if (!result) {
    return fail(result.error());
  }
  std::cout << "disposition=" << to_string(result.value().disposition) << "\n";
  std::cout << "sequence=" << result.value().sequence.value() << "\n";
  std::cout << "mode=" << mode << " target=" << request.value().content.target.value() << "\n";
  auto closed = ledger.value().close();
  if (!closed) {
    return fail(closed.error());
  }
  return kExitOk;
}

int command_verify(const Args& args) {
  if (args.positional.empty()) {
    std::cerr << "error: verify requires a store path\n";
    return kExitUsage;
  }
  VerifyOptions options;
  options.deep = !args.get_flag("shallow");
  auto report = verify_store(args.positional[0], options);
  if (!report) {
    return fail(report.error());
  }
  JsonValue json = JsonValue::object();
  json.set("ok", JsonValue::boolean(report.value().ok));
  json.set("generation", JsonValue::integer(static_cast<std::int64_t>(report.value().generation.value())));
  json.set("incarnation", JsonValue::string(report.value().incarnation.to_hex()));
  json.set("entries-checked", JsonValue::integer(static_cast<std::int64_t>(report.value().entries_checked)));
  json.set("segments-checked", JsonValue::integer(static_cast<std::int64_t>(report.value().segments_checked)));
  json.set("bytes-checked", JsonValue::integer(static_cast<std::int64_t>(report.value().bytes_checked)));
  json.set("residue-bytes", JsonValue::integer(static_cast<std::int64_t>(report.value().residue_bytes)));
  json.set("chain-head", digest_json(report.value().chain_head));
  JsonValue findings = JsonValue::array();
  for (const IntegrityFinding& finding : report.value().findings) {
    JsonValue item = JsonValue::object();
    item.set("area", JsonValue::string(finding.area));
    item.set("code", JsonValue::string(to_string(finding.code)));
    item.set("detail", JsonValue::string(finding.detail));
    findings.push_back(std::move(item));
  }
  json.set("findings", std::move(findings));
  if (args.get_flag("json")) {
    std::cout << to_pretty_json(json) << "\n";
  } else {
    std::cout << "ok=" << (report.value().ok ? "true" : "false") << "\n";
    std::cout << "generation=" << report.value().generation.value() << "\n";
    std::cout << "entries-checked=" << report.value().entries_checked << "\n";
    std::cout << "segments-checked=" << report.value().segments_checked << "\n";
    std::cout << "residue-bytes=" << report.value().residue_bytes << "\n";
    std::cout << "chain-head=" << report.value().chain_head.to_hex() << "\n";
    for (const IntegrityFinding& finding : report.value().findings) {
      std::cout << "finding " << finding.area << " " << to_string(finding.code) << ": "
                << finding.detail << "\n";
    }
  }
  return report.value().ok ? kExitOk : kExitVerificationFailed;
}

int command_audit(const Args& args) {
  if (args.positional.empty()) {
    std::cerr << "error: audit requires a store path\n";
    return kExitUsage;
  }
  auto audit = audit_store(args.positional[0]);
  if (!audit) {
    return fail(audit.error());
  }
  const StoreAudit& value = audit.value();
  JsonValue json = JsonValue::object();
  json.set("store", JsonValue::string(value.path));
  json.set("store-uuid", JsonValue::string(value.store.to_hex()));
  json.set("incarnation", JsonValue::string(value.incarnation.to_hex()));
  json.set("generation", JsonValue::integer(static_cast<std::int64_t>(value.generation.value())));
  json.set("head-sequence", JsonValue::integer(static_cast<std::int64_t>(value.head_sequence.value())));
  json.set("entries", JsonValue::integer(static_cast<std::int64_t>(value.entry_count)));
  json.set("retired-floor", JsonValue::integer(static_cast<std::int64_t>(value.retired_floor_sequence)));
  json.set("chain-head", digest_json(value.chain_head));
  json.set("chain-at-floor", digest_json(value.chain_at_floor));
  json.set("residue-bytes", JsonValue::integer(static_cast<std::int64_t>(value.residue_bytes)));
  json.set("writer-lease-present", JsonValue::boolean(value.writer_lease_present));
  json.set("writer-epoch", JsonValue::integer(static_cast<std::int64_t>(value.writer_epoch.value())));
  json.set("writer-lease-held", JsonValue::boolean(value.writer_lease_held));
  json.set("format-version", JsonValue::integer(static_cast<std::int64_t>(value.format_version)));
  JsonValue slots = JsonValue::array();
  for (const ManifestSlotAudit& slot : value.slots) {
    JsonValue item = JsonValue::object();
    item.set("name", JsonValue::string(slot.name));
    item.set("present", JsonValue::boolean(slot.present));
    item.set("valid", JsonValue::boolean(slot.valid));
    item.set("generation", JsonValue::integer(static_cast<std::int64_t>(slot.generation.value())));
    item.set("failure", JsonValue::string(slot.failure));
    slots.push_back(std::move(item));
  }
  json.set("manifest-slots", std::move(slots));
  JsonValue segments = JsonValue::array();
  for (const SegmentAudit& segment : value.segments) {
    JsonValue item = JsonValue::object();
    item.set("id", JsonValue::integer(static_cast<std::int64_t>(segment.id)));
    item.set("first-sequence", JsonValue::integer(static_cast<std::int64_t>(segment.first_sequence)));
    item.set("entries", JsonValue::integer(static_cast<std::int64_t>(segment.entry_count)));
    item.set("committed-bytes", JsonValue::integer(static_cast<std::int64_t>(segment.committed_bytes)));
    item.set("file-bytes", JsonValue::integer(static_cast<std::int64_t>(segment.file_bytes)));
    item.set("digest-verified", JsonValue::boolean(segment.digest_verified));
    segments.push_back(std::move(item));
  }
  json.set("segments", std::move(segments));
  if (args.get_flag("json")) {
    std::cout << to_pretty_json(json) << "\n";
  } else {
    std::cout << "store=" << value.path << "\n";
    std::cout << "store-uuid=" << value.store.to_hex() << " incarnation=" << value.incarnation.to_hex()
              << "\n";
    std::cout << "generation=" << value.generation.value()
              << " head-sequence=" << value.head_sequence.value() << " entries=" << value.entry_count
              << " retired-floor=" << value.retired_floor_sequence << "\n";
    std::cout << "residue-bytes=" << value.residue_bytes << "\n";
    std::cout << "writer-epoch=" << value.writer_epoch.value()
              << " lease-held=" << (value.writer_lease_held ? "true" : "false")
              << " lease-present=" << (value.writer_lease_present ? "true" : "false") << "\n";
    for (const ManifestSlotAudit& slot : value.slots) {
      std::cout << "slot " << slot.name << " present=" << (slot.present ? "true" : "false")
                << " valid=" << (slot.valid ? "true" : "false")
                << " generation=" << slot.generation.value();
      if (!slot.failure.empty()) {
        std::cout << " failure=" << slot.failure;
      }
      std::cout << "\n";
    }
    for (const SegmentAudit& segment : value.segments) {
      std::cout << "segment id=" << segment.id << " first-sequence=" << segment.first_sequence
                << " entries=" << segment.entry_count
                << " committed-bytes=" << segment.committed_bytes
                << " file-bytes=" << segment.file_bytes
                << " digest-verified=" << (segment.digest_verified ? "true" : "false") << "\n";
    }
  }
  return kExitOk;
}

int command_format(const Args& args) {
  const FormatInfo info = format_info();
  JsonValue json = JsonValue::object();
  json.set("descriptor-version", JsonValue::integer(info.descriptor_version));
  json.set("manifest-version", JsonValue::integer(info.manifest_version));
  json.set("segment-version", JsonValue::integer(info.segment_version));
  json.set("lease-version", JsonValue::integer(info.lease_version));
  json.set("descriptor-bytes", JsonValue::integer(static_cast<std::int64_t>(info.descriptor_bytes)));
  json.set("manifest-header-bytes", JsonValue::integer(static_cast<std::int64_t>(info.manifest_header_bytes)));
  json.set("segment-header-bytes", JsonValue::integer(static_cast<std::int64_t>(info.segment_header_bytes)));
  json.set("lease-bytes", JsonValue::integer(static_cast<std::int64_t>(info.lease_bytes)));
  json.set("little-endian-on-disk", JsonValue::boolean(info.little_endian_on_disk));
  json.set("library-version", JsonValue::string(kVersionString));
  if (args.get_flag("json")) {
    std::cout << to_pretty_json(json) << "\n";
  } else {
    std::cout << "library-version=" << kVersionString << "\n";
    std::cout << "store-format=" << kStoreFormatName << " version=" << kStoreFormatVersion << "\n";
    std::cout << "descriptor-bytes=" << info.descriptor_bytes
              << " manifest-header-bytes=" << info.manifest_header_bytes
              << " segment-header-bytes=" << info.segment_header_bytes
              << " lease-bytes=" << info.lease_bytes << "\n";
    std::cout << "byte-order=little-endian (explicit)" << "\n";
  }
  return kExitOk;
}

int command_crash_append(const Args& args) {
  if (args.positional.empty() || !args.has("record")) {
    std::cerr << "error: __crash-append requires a store path and --record <file.json>\n";
    return kExitUsage;
  }
  auto request = request_from_file(args, args.get("record"));
  if (!request) {
    return fail(request.error());
  }
  if (!args.has("crash-stage")) {
    request.value().fault.terminate_at = FaultInjection::Stage::AfterSlotFlush;
  }
  request.value().fault.exit_code = static_cast<int>(args.get_int("crash-code", test_support::kCrashExitCode));
  OpenOptions options;
  options.mode = AccessMode::ReadWrite;
  auto ledger = Ledger::open(args.positional[0], options);
  if (!ledger) {
    return fail(ledger.error());
  }
  auto result = ledger.value().append(request.value());
  if (!result) {
    return fail(result.error());
  }
  std::cout << "sequence=" << result.value().sequence.value() << "\n";
  std::cout.flush();
  auto closed = ledger.value().close();
  if (!closed) {
    return fail(closed.error());
  }
  return kExitOk;
}

int command_hold_writer(const Args& args) {
  if (args.positional.empty()) {
    std::cerr << "error: __hold-writer requires a store path\n";
    return kExitUsage;
  }
  OpenOptions options;
  options.mode = AccessMode::ReadWrite;
  auto ledger = Ledger::open(args.positional[0], options);
  if (!ledger) {
    if (ledger.error().code == StatusCode::StoreBusy) {
      std::cout << "BUSY" << std::endl;
      return kExitBusy;
    }
    return fail(ledger.error());
  }
  std::cout << "ACQUIRED epoch=" << ledger.value().writer_epoch().value() << std::endl;
  std::string line;
  if (std::getline(std::cin, line)) {
    // Parent released us.
  }
  if (args.has("record")) {
    auto request = request_from_file(args, args.get("record"));
    if (!request) {
      return fail(request.error());
    }
    auto result = ledger.value().append(request.value());
    if (!result) {
      std::cout << "APPEND-REFUSED code=" << to_string(result.error().code) << std::endl;
      auto closed_refused = ledger.value().close();
      (void)closed_refused;
      return 5;
    }
    std::cout << "APPEND-OK sequence=" << result.value().sequence.value() << std::endl;
  }
  auto closed = ledger.value().close();
  if (!closed) {
    return fail(closed.error());
  }
  std::cout << "RELEASED" << std::endl;
  return kExitOk;
}

int command_try_open(const Args& args) {
  if (args.positional.empty()) {
    std::cerr << "error: __try-open requires a store path\n";
    return kExitUsage;
  }
  OpenOptions options;
  options.mode = AccessMode::ReadWrite;
  auto ledger = Ledger::open(args.positional[0], options);
  if (!ledger) {
    if (ledger.error().code == StatusCode::StoreBusy) {
      std::cout << "BUSY" << "\n";
      return kExitBusy;
    }
    return fail(ledger.error());
  }
  std::cout << "ACQUIRED" << "\n";
  auto closed = ledger.value().close();
  if (!closed) {
    return fail(closed.error());
  }
  return kExitOk;
}

int command_hold_and_die(const Args& args) {
  if (args.positional.empty()) {
    std::cerr << "error: __hold-and-die requires a store path\n";
    return kExitUsage;
  }
  OpenOptions options;
  options.mode = AccessMode::ReadWrite;
  auto ledger = Ledger::open(args.positional[0], options);
  if (!ledger) {
    return fail(ledger.error());
  }
  std::cout << "ACQUIRED" << std::endl;
  std::string line;
  std::getline(std::cin, line);
  std::cout << "DYING" << std::endl;
  std::cout.flush();
  std::_Exit(kExitOk);
  return kExitOk;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    print_usage();
    return kExitUsage;
  }
  const std::string command = argv[1];
  const Args args = parse_args(argc, argv, 2);

  if (command == "help" || command == "--help" || command == "-h") {
    print_usage();
    return kExitOk;
  }
  if (command == "version" || command == "--version") {
    std::cout << kLibraryName << " " << kVersionString << "\n";
    return kExitOk;
  }
  if (command == "init") return command_init(args);
  if (command == "open") return command_open(args);
  if (command == "append") return command_append(args);
  if (command == "import") return command_import(args);
  if (command == "inspect") return command_inspect(args);
  if (command == "rollup") return command_rollup(args);
  if (command == "reconcile") return command_reconcile(args);
  if (command == "provenance") return command_provenance(args);
  if (command == "history") return command_history(args);
  if (command == "correct") return command_correct(args);
  if (command == "verify") return command_verify(args);
  if (command == "audit") return command_audit(args);
  if (command == "format") return command_format(args);
  if (command == "__crash-append") return command_crash_append(args);
  if (command == "__hold-writer") return command_hold_writer(args);
  if (command == "__try-open") return command_try_open(args);
  if (command == "__hold-and-die") return command_hold_and_die(args);

  std::cerr << "error: unknown command '" << command << "'\n";
  print_usage();
  return kExitUsage;
}
