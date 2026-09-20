// Inference Traffic Fabric - inspection, explanation and probe CLI.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "itf/accel.hpp"
#include "itf/client.hpp"
#include "itf/coordinator.hpp"
#include "itf/persistence.hpp"
#include "itf/serialize.hpp"
#include "itf/version.hpp"

namespace {

using namespace itf;

constexpr std::uint64_t kModelHash = 0x1111222233334444ULL;
constexpr std::uint64_t kStateHash = 0x5555666677778888ULL;
constexpr std::uint64_t kTenantHash = 0x9999AAAABBBBCCCCULL;

[[nodiscard]] std::string vocabulary() {
  std::string text;
  text.append("stages:  ");
  for (std::size_t index = 1; index < kServingStageCount; ++index) {
    if (index > 1) text.push_back(' ');
    text.append(label(static_cast<ServingStage>(index)));
  }
  text.append("\nclasses: ");
  for (std::size_t index = 1; index < kTrafficClassCount; ++index) {
    if (index > 1) text.push_back(' ');
    text.append(label(static_cast<TrafficClass>(index)));
  }
  text.push_back('\n');
  return text;
}

void print_usage() {
  std::printf(
      "itfctl - Inference Traffic Fabric inspection tool\n"
      "\n"
      "usage: itfctl COMMAND [options]\n"
      "\n"
      "commands:\n"
      "  version                     print version and compatibility constants\n"
      "  accel-probe [--no-kernel]   run the real accelerator probe on this host\n"
      "  simulate SCENARIO           run a deterministic in-process scenario and explain it\n"
      "  status     --port N --token S   query a running coordinator\n"
      "  accounting --port N --token S   query per-class accounting\n"
      "  policy     --port N --token S   fetch the active policy document\n"
      "  evaluate   --port N --token S [--stage S] [--class C] [--bytes N]\n"
      "  snapshot-info PATH          summarise a durable snapshot file\n"
      "\n"
      "scenarios: basic, cancel, stale, starvation, unknown-stage, bulk-yield, generations\n"
      "\n");
  std::printf("%s", vocabulary().c_str());
}

[[nodiscard]] bool parse_u64(std::string_view text, std::uint64_t& out) {
  if (text.empty()) return false;
  std::uint64_t value = 0;
  for (const char raw : text) {
    if (raw < '0' || raw > '9') return false;
    const std::uint64_t digit = static_cast<std::uint64_t>(raw - '0');
    if (value > (UINT64_MAX - digit) / 10ULL) return false;
    value = value * 10ULL + digit;
  }
  out = value;
  return true;
}

struct Options {
  std::string host = "127.0.0.1";
  std::uint16_t port = 7777;
  std::string token;
  std::string stage = "ADMITTED";
  std::string traffic_class = "REQUEST_INGRESS";
  std::uint64_t bytes = 4096;
  bool no_kernel = false;
};

[[nodiscard]] bool parse_options(int argc, char** argv, int start, Options& options) {
  for (int index = start; index < argc; ++index) {
    const std::string_view flag(argv[index]);
    const auto next = [&](const char* name) -> const char* {
      if (index + 1 >= argc) {
        std::fprintf(stderr, "missing value for %s\n", name);
        std::exit(2);
      }
      return argv[++index];
    };
    std::uint64_t number = 0;
    if (flag == "--host") {
      options.host = next("--host");
    } else if (flag == "--port") {
      if (!parse_u64(next("--port"), number) || number > 65535) return false;
      options.port = static_cast<std::uint16_t>(number);
    } else if (flag == "--token") {
      options.token = next("--token");
    } else if (flag == "--stage") {
      options.stage = next("--stage");
    } else if (flag == "--class") {
      options.traffic_class = next("--class");
    } else if (flag == "--bytes") {
      if (!parse_u64(next("--bytes"), number)) return false;
      options.bytes = number;
    } else if (flag == "--no-kernel") {
      options.no_kernel = true;
    } else {
      std::fprintf(stderr, "unknown option: %s\n", argv[index]);
      return false;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// Deterministic in-process scenarios
// ---------------------------------------------------------------------------

struct Harness {
  VirtualClock clock;
  Coordinator coordinator;

  explicit Harness(CoordinatorConfig config)
      : coordinator(std::move(config), &clock) {
    (void)coordinator.boot_fresh();
  }

  RequestId request{0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL};
  AttemptId attempt{0x1111111111111111ULL, 0x2222222222222222ULL};

  void publish_topology(EvidenceProvenance provenance = EvidenceProvenance::Synthetic) {
    TopologyEvidence evidence;
    evidence.provenance = provenance;
    evidence.node_count = 2;
    evidence.link_count = 1;
    evidence.accelerator_count = 1;
    evidence.fabric_bytes_per_sec = 25ULL * 1000ULL * 1000ULL * 1000ULL;
    evidence.max_age_nanos = 1000000000ULL;
    evidence.disaggregated = true;
    const auto published = coordinator.publish_topology(evidence);
    if (!published.ok()) {
      std::fprintf(stderr, "topology publish failed: %s\n", published.status().to_string().c_str());
    }
  }

  void prepare_request(ServingStage stage = ServingStage::Admitted) {
    (void)coordinator.set_model_generation(kModelHash, std::nullopt);
    (void)coordinator.advance_state_generation(kStateHash, std::nullopt);
    (void)coordinator.register_request(request);
    (void)coordinator.register_attempt(request, attempt, kModelHash);
    (void)coordinator.issue_route_decision(request, attempt, 1, false, true);
    (void)coordinator.register_slo_contract(request, 20000000ULL, 20000000ULL, 200);
    if (stage != ServingStage::Unknown) (void)coordinator.advance_stage(request, attempt, stage);
  }

  [[nodiscard]] TrafficRequest make_request(TrafficClass traffic, ServingStage stage,
                                            std::uint64_t bytes) {
    TrafficRequest request_message;
    TrafficSubject subject;
    subject.kind = SubjectKind::Serving;
    subject.request = request;
    AttemptId current{};
    {
      auto record = coordinator.request_record(request);
      if (record.has_value()) current = record->current_attempt;
    }
    subject.attempt = current.is_nil() ? attempt : current;
    subject.request_generation = RequestGeneration::from(1);
    request_message.subject = subject;
    request_message.declared_class = traffic;
    request_message.declared_stage = stage;
    request_message.direction = FlowDirection::Lateral;
    request_message.payload_bytes = bytes;
    request_message.model_hash = kModelHash;
    request_message.tenant_hash = kTenantHash;
    request_message.deadline = Instant::from_now(clock.now_nanos(), 20000000);
    request_message.binding.policy = coordinator.policy().generation();
    request_message.binding.coordinator_epoch = coordinator.epoch();
    request_message.binding.model_generation = coordinator.model_generation(kModelHash).value();
    auto route = coordinator.issue_route_decision(request, subject.attempt, 1, false, true);
    if (route.ok()) request_message.binding.route_decision = route.value().generation;
    auto slo = coordinator.register_slo_contract(request, 20000000ULL, 20000000ULL, 200);
    if (slo.ok()) request_message.binding.slo_contract = slo.value().generation;
    auto state = coordinator.state_generation(kStateHash);
    if (state.ok()) request_message.binding.state_generation = state.value();
    return request_message;
  }
};

int report(const Decision& decision, bool verbose) {
  if (verbose) {
    std::printf("%s", decision.explain().c_str());
  } else {
    std::printf("%s\n", decision.to_string().c_str());
  }
  return 0;
}

int scenario_basic(bool verbose) {
  CoordinatorConfig config;
  config.policy = PolicyConfig::canonical_defaults(PolicyGeneration::from(1));
  Harness harness(config);
  harness.publish_topology();
  harness.prepare_request(ServingStage::Admitted);
  (void)harness.coordinator.advance_stage(harness.request, harness.attempt,
                                          ServingStage::PrefillQueued);
  (void)harness.coordinator.advance_stage(harness.request, harness.attempt,
                                          ServingStage::PrefillRunning);
  (void)harness.coordinator.advance_stage(harness.request, harness.attempt,
                                          ServingStage::HandoffPending);

  const auto request_message = harness.make_request(TrafficClass::PrefillDecodeHandoff,
                                                    ServingStage::HandoffPending, 1U << 20);
  const Decision handoff = harness.coordinator.evaluate(request_message);
  report(handoff, verbose);

  auto flow = harness.coordinator.open_flow(handoff, request_message.payload_bytes);
  if (!flow.ok()) {
    std::fprintf(stderr, "open_flow failed: %s\n", flow.status().to_string().c_str());
    return 1;
  }
  (void)harness.coordinator.record_flow_bytes(flow.value(), request_message.payload_bytes, false);
  (void)harness.coordinator.release_flow(flow.value());

  (void)harness.coordinator.advance_stage(harness.request, harness.attempt,
                                          ServingStage::DecodeRunning);
  (void)harness.coordinator.advance_stage(harness.request, harness.attempt,
                                          ServingStage::Streaming);
  std::printf("%s\n",
              harness.coordinator
                  .evaluate(harness.make_request(TrafficClass::DecodeStream,
                                                 ServingStage::Streaming, 4096))
                  .to_string()
                  .c_str());
  (void)harness.coordinator.advance_stage(harness.request, harness.attempt,
                                          ServingStage::Completing);
  std::printf("%s\n",
              harness.coordinator
                  .evaluate(harness.make_request(TrafficClass::ResponseEgress,
                                                 ServingStage::Completing, 2048))
                  .to_string()
                  .c_str());

  CompletionPublication publication;
  publication.request = harness.request;
  publication.attempt = harness.attempt;
  {
    auto record = harness.coordinator.request_record(harness.request);
    if (record.has_value()) publication.attempt_generation = record->current_attempt_generation;
  }
  publication.bytes_in = 1U << 20;
  publication.bytes_out = 2048;
  const auto outcome = harness.coordinator.publish_completion(publication);
  if (!outcome.ok()) {
    std::fprintf(stderr, "completion failed: %s\n", outcome.status().to_string().c_str());
    return 1;
  }
  std::printf("completion disposition=%u reason=%s\n",
              static_cast<unsigned>(outcome.value().disposition),
              std::string(to_string(outcome.value().reason)).c_str());
  const Status invariants = harness.coordinator.verify_invariants();
  std::printf("invariants: %s\n", invariants.to_string().c_str());
  std::printf("accounting: %s\n", harness.coordinator.accounting().to_string().c_str());
  return invariants.ok() ? 0 : 1;
}

int scenario_cancel(bool verbose) {
  CoordinatorConfig config;
  config.policy = PolicyConfig::canonical_defaults(PolicyGeneration::from(1));
  Harness harness(config);
  harness.publish_topology();
  harness.prepare_request(ServingStage::Admitted);

  const auto request_message = harness.make_request(TrafficClass::RequestIngress,
                                                    ServingStage::Admitted, 1024);
  const Decision before = harness.coordinator.evaluate(request_message);
  report(before, verbose);
  const auto flow = harness.coordinator.open_flow(before, 1024);
  std::printf("pre-cancel flow opened: %s\n", flow.ok() ? "yes" : "no");

  (void)harness.coordinator.cancel_request(harness.request, ReasonCode::Cancelled);
  const Decision after = harness.coordinator.evaluate(request_message);
  report(after, verbose);
  const auto reopened = harness.coordinator.open_flow(before, 1024);
  std::printf("pre-cancel decision re-used after cancellation: %s\n",
              reopened.ok() ? "ACCEPTED (defect)" : "refused");

  CompletionPublication publication;
  publication.request = harness.request;
  publication.attempt = harness.attempt;
  publication.attempt_generation = AttemptGeneration::from(1);
  publication.bytes_in = 1024;
  const auto outcome = harness.coordinator.publish_completion(publication);
  std::printf("post-cancel completion: disposition=%u reason=%s\n",
              static_cast<unsigned>(outcome.value().disposition),
              std::string(to_string(outcome.value().reason)).c_str());
  const Status invariants = harness.coordinator.verify_invariants();
  std::printf("invariants: %s\n", invariants.to_string().c_str());
  std::printf("accounting: %s\n", harness.coordinator.accounting().to_string().c_str());
  const bool honest = !reopened.ok() && outcome.value().refused() && invariants.ok();
  return honest ? 0 : 1;
}

int scenario_stale(bool verbose) {
  CoordinatorConfig config;
  config.policy = PolicyConfig::canonical_defaults(PolicyGeneration::from(1));
  Harness harness(config);
  harness.publish_topology();
  harness.prepare_request(ServingStage::PrefillRunning);

  TransferRegistration registration;
  registration.id = StateTransferId(0xAAAABBBBCCCCDDDDULL, 0x0102030405060708ULL);
  registration.state_hash = kStateHash;
  registration.model_hash = kModelHash;
  registration.payload_bytes = 8U << 20;
  registration.declared_class = TrafficClass::KvTransfer;
  registration.direction = FlowDirection::Lateral;
  registration.required_integrity = IntegrityClass::ChecksumAndVerify;
  registration.request = harness.request;
  registration.attempt = harness.attempt;
  (void)harness.coordinator.register_slo_contract(harness.request, 20000000ULL, 20000000ULL, 200);
  const auto registered = harness.coordinator.register_transfer(registration);
  if (!registered.ok()) {
    std::fprintf(stderr, "register_transfer failed: %s\n", registered.status().to_string().c_str());
    return 1;
  }

  TrafficRequest request_message;
  TrafficSubject subject;
  subject.kind = SubjectKind::Transfer;
  subject.transfer = registration.id;
  request_message.subject = subject;
  request_message.declared_class = TrafficClass::KvTransfer;
  request_message.direction = FlowDirection::Lateral;
  request_message.payload_bytes = registration.payload_bytes;
  request_message.binding.policy = harness.coordinator.policy().generation();
  request_message.binding.coordinator_epoch = harness.coordinator.epoch();
  request_message.binding.model_generation = harness.coordinator.model_generation(kModelHash).value();
  request_message.binding.state_generation = harness.coordinator.state_generation(kStateHash).value();
  request_message.binding.slo_contract =
      harness.coordinator.register_slo_contract(harness.request, 20000000ULL, 20000000ULL, 200).value()
          .generation;
  request_message.deadline = Instant::from_now(harness.clock.now_nanos(), 20000000);

  const auto authorized = harness.coordinator.authorize_transfer(request_message);
  std::printf("initial transfer authorization: authorized=%s reason=%s\n",
              authorized.value().authorized ? "true" : "false",
              std::string(to_string(authorized.value().reason)).c_str());

  (void)harness.coordinator.advance_state_generation(kStateHash, std::nullopt);
  const auto stale = harness.coordinator.authorize_transfer(request_message);
  std::printf("after state generation advance: authorized=%s reason=%s\n",
              stale.value().authorized ? "true" : "false",
              std::string(to_string(stale.value().reason)).c_str());
  report(harness.coordinator.evaluate(request_message), verbose);
  const bool honest = authorized.value().authorized && !stale.value().authorized &&
                      stale.value().reason == ReasonCode::RejectedStaleStateGeneration;
  return honest ? 0 : 1;
}

int scenario_starvation(bool verbose) {
  CoordinatorConfig config;
  PolicyConfig policy = PolicyConfig::canonical_defaults(PolicyGeneration::from(1));
  ClassPolicyRule streaming = policy.rule(TrafficClass::DecodeStream);
  streaming.reserved_weight = 0;
  streaming.max_defer_count = 4;
  streaming.max_defer_horizon_nanos = 1000000000ULL;
  policy.set_rule(TrafficClass::DecodeStream, streaming);
  policy.set_congestion_queue_depth(4);
  config.policy = policy;
  Harness harness(config);

  TopologyEvidence evidence;
  evidence.provenance = EvidenceProvenance::Synthetic;
  evidence.node_count = 2;
  evidence.link_count = 1;
  evidence.queue_depth = 64;
  evidence.congestion_signalled = true;
  evidence.fabric_bytes_per_sec = 1000000000ULL;
  evidence.max_age_nanos = 1000000000ULL;
  (void)harness.coordinator.publish_topology(evidence);

  harness.prepare_request(ServingStage::Admitted);
  (void)harness.coordinator.advance_stage(harness.request, harness.attempt,
                                          ServingStage::PrefillQueued);
  (void)harness.coordinator.advance_stage(harness.request, harness.attempt,
                                          ServingStage::PrefillRunning);
  (void)harness.coordinator.advance_stage(harness.request, harness.attempt,
                                          ServingStage::HandoffPending);
  (void)harness.coordinator.advance_stage(harness.request, harness.attempt,
                                          ServingStage::DecodeQueued);
  (void)harness.coordinator.advance_stage(harness.request, harness.attempt,
                                          ServingStage::DecodeRunning);
  (void)harness.coordinator.advance_stage(harness.request, harness.attempt,
                                          ServingStage::Streaming);

  const auto request_message =
      harness.make_request(TrafficClass::DecodeStream, ServingStage::Streaming, 8192);
  bool admitted = false;
  for (int attempt_index = 0; attempt_index < 8 && !admitted; ++attempt_index) {
    const Decision decision = harness.coordinator.evaluate(request_message);
    std::printf("evaluation %d: %s\n", attempt_index + 1, decision.to_string().c_str());
    if (decision.permits_traffic()) {
      admitted = decision.reason == ReasonCode::AdmittedStarvationGuard;
      std::printf("  admitted by starvation guard: %s\n", admitted ? "true" : "false");
      break;
    }
    harness.clock.advance(2000000);
  }
  const AccountingSnapshot snapshot = harness.coordinator.accounting();
  std::printf("starvation admissions: %u\n",
              snapshot.per_class[static_cast<std::size_t>(TrafficClass::DecodeStream)]
                  .starvation_admissions);
  (void)verbose;
  return admitted ? 0 : 1;
}

int scenario_unknown_stage(bool verbose) {
  CoordinatorConfig config;
  config.policy = PolicyConfig::canonical_defaults(PolicyGeneration::from(1));
  Harness harness(config);
  harness.publish_topology();
  harness.prepare_request(ServingStage::Unknown);

  const auto strict_message = harness.make_request(TrafficClass::RequestIngress,
                                                   ServingStage::Unknown, 1024);
  const Decision strict = harness.coordinator.evaluate(strict_message);
  std::printf("strict policy: %s\n", strict.to_string().c_str());

  CoordinatorConfig relaxed = config;
  PolicyConfig policy = PolicyConfig::canonical_defaults(PolicyGeneration::from(1));
  policy.set_allow_unknown_stage(true);
  policy.set_allow_unknown_traffic_class(true);
  ClassPolicyRule conservative;
  conservative.enabled = true;
  conservative.priority = PriorityClass::Background;
  conservative.pacing = PacingClass::BestEffort;
  conservative.integrity = IntegrityClass::ChecksumAndVerify;
  conservative.isolation = IsolationDomain::Background;
  conservative.max_burst_bytes = 65536;
  (void)policy.set_rule(TrafficClass::Unknown, conservative);
  relaxed.policy = policy;
  Harness relaxed_harness(relaxed);
  relaxed_harness.publish_topology();
  relaxed_harness.prepare_request(ServingStage::Unknown);
  const auto relaxed_message = relaxed_harness.make_request(TrafficClass::RequestIngress,
                                                            ServingStage::Unknown, 1024);
  const Decision lenient = relaxed_harness.coordinator.evaluate(relaxed_message);
  std::printf("relaxed policy: %s\n", lenient.to_string().c_str());
  if (verbose) std::printf("%s", lenient.explain().c_str());
  const bool honest = strict.outcome == DecisionOutcome::Rejected &&
                      strict.reason == ReasonCode::RejectedUnknownStage &&
                      lenient.outcome == DecisionOutcome::Degraded &&
                      lenient.reason == ReasonCode::AdmittedConservative &&
                      lenient.treatment.reserved_weight == 0;
  return honest ? 0 : 1;
}

int scenario_bulk_yield(bool verbose) {
  CoordinatorConfig config;
  PolicyConfig policy = PolicyConfig::canonical_defaults(PolicyGeneration::from(1));
  ClassPolicyRule streaming = policy.rule(TrafficClass::DecodeStream);
  streaming.reserved_weight = 0;
  streaming.max_defer_count = 8;
  streaming.max_defer_horizon_nanos = 1000000000ULL;
  policy.set_rule(TrafficClass::DecodeStream, streaming);
  policy.set_congestion_queue_depth(4);
  config.policy = policy;
  Harness harness(config);

  TopologyEvidence evidence;
  evidence.provenance = EvidenceProvenance::Synthetic;
  evidence.node_count = 2;
  evidence.link_count = 1;
  evidence.queue_depth = 64;
  evidence.congestion_signalled = true;
  evidence.fabric_bytes_per_sec = 1000000000ULL;
  evidence.max_age_nanos = 1000000000ULL;
  (void)harness.coordinator.publish_topology(evidence);

  harness.prepare_request(ServingStage::Admitted);
  (void)harness.coordinator.advance_stage(harness.request, harness.attempt,
                                          ServingStage::PrefillQueued);
  (void)harness.coordinator.advance_stage(harness.request, harness.attempt,
                                          ServingStage::PrefillRunning);
  (void)harness.coordinator.advance_stage(harness.request, harness.attempt,
                                          ServingStage::HandoffPending);
  (void)harness.coordinator.advance_stage(harness.request, harness.attempt,
                                          ServingStage::DecodeQueued);
  (void)harness.coordinator.advance_stage(harness.request, harness.attempt,
                                          ServingStage::DecodeRunning);
  (void)harness.coordinator.advance_stage(harness.request, harness.attempt,
                                          ServingStage::Streaming);

  const auto streaming_message =
      harness.make_request(TrafficClass::DecodeStream, ServingStage::Streaming, 8192);
  const Decision deferred = harness.coordinator.evaluate(streaming_message);
  std::printf("streaming under congestion: %s\n", deferred.to_string().c_str());

  TransferRegistration registration;
  registration.id = StateTransferId(0x1234123412341234ULL, 0x4321432143214321ULL);
  registration.state_hash = kStateHash;
  registration.model_hash = kModelHash;
  registration.payload_bytes = 64U << 20;
  registration.declared_class = TrafficClass::ModelTransfer;
  registration.direction = FlowDirection::Lateral;
  registration.required_integrity = IntegrityClass::Checksum;
  const auto registered = harness.coordinator.register_transfer(registration);
  if (!registered.ok()) {
    std::fprintf(stderr, "register_transfer failed: %s\n", registered.status().to_string().c_str());
    return 1;
  }
  TrafficRequest bulk;
  TrafficSubject subject;
  subject.kind = SubjectKind::Transfer;
  subject.transfer = registration.id;
  bulk.subject = subject;
  bulk.declared_class = TrafficClass::ModelTransfer;
  bulk.direction = FlowDirection::Lateral;
  bulk.payload_bytes = registration.payload_bytes;
  bulk.binding.policy = harness.coordinator.policy().generation();
  bulk.binding.coordinator_epoch = harness.coordinator.epoch();
  bulk.binding.model_generation = harness.coordinator.model_generation(kModelHash).value();
  const Decision bulk_decision = harness.coordinator.evaluate(bulk);
  std::printf("model movement while streaming is deferred: %s\n", bulk_decision.to_string().c_str());
  if (verbose) std::printf("%s", bulk_decision.explain().c_str());
  const bool honest = deferred.outcome == DecisionOutcome::Deferred &&
                      bulk_decision.outcome == DecisionOutcome::Deferred;
  return honest ? 0 : 1;
}

int scenario_generations(bool verbose) {
  CoordinatorConfig config;
  config.policy = PolicyConfig::canonical_defaults(PolicyGeneration::from(1));
  Harness harness(config);
  harness.publish_topology();
  harness.prepare_request(ServingStage::Admitted);

  auto stale_policy = harness.make_request(TrafficClass::RequestIngress, ServingStage::Admitted, 512);
  stale_policy.binding.policy = PolicyGeneration::from(99);
  const ReasonCode policy_reason = harness.coordinator.evaluate(stale_policy).reason;
  std::printf("stale policy: %s\n", std::string(to_string(policy_reason)).c_str());

  auto stale_epoch = harness.make_request(TrafficClass::RequestIngress, ServingStage::Admitted, 512);
  stale_epoch.binding.coordinator_epoch = CoordinatorEpoch::from(99);
  const ReasonCode epoch_reason = harness.coordinator.evaluate(stale_epoch).reason;
  std::printf("stale epoch: %s\n", std::string(to_string(epoch_reason)).c_str());

  auto stale_route = harness.make_request(TrafficClass::RequestIngress, ServingStage::Admitted, 512);
  stale_route.binding.route_decision = RouteDecisionGeneration::from(99);
  const ReasonCode route_reason = harness.coordinator.evaluate(stale_route).reason;
  std::printf("stale route decision: %s\n", std::string(to_string(route_reason)).c_str());

  auto stale_model = harness.make_request(TrafficClass::RequestIngress, ServingStage::Admitted, 512);
  stale_model.binding.route_decision.reset();
  stale_model.declared_class = TrafficClass::PrefillInput;
  stale_model.declared_stage = ServingStage::Admitted;
  const Decision model_checked = harness.coordinator.evaluate(stale_model);
  (void)verbose;
  std::printf("model fence (stage mismatch, class re-derived): %s\n",
              model_checked.to_string().c_str());

  auto unknown_request = harness.make_request(TrafficClass::RequestIngress, ServingStage::Admitted, 512);
  TrafficSubject missing;
  missing.kind = SubjectKind::Serving;
  missing.request = RequestId(0xDEADBEEFDEADBEEFULL, 0xDEADBEEFDEADBEEFULL);
  missing.attempt = AttemptId(0xFEEDFACEFEEDFACEULL, 0xFEEDFACEFEEDFACEULL);
  missing.request_generation = RequestGeneration::from(1);
  unknown_request.subject = missing;
  unknown_request.binding.route_decision.reset();
  std::printf("unknown request: %s\n",
              harness.coordinator.evaluate(unknown_request).to_string().c_str());

  const bool honest = policy_reason == ReasonCode::RejectedStalePolicyGeneration &&
                      epoch_reason == ReasonCode::RejectedStaleEpoch &&
                      route_reason == ReasonCode::RejectedStaleRouteDecision;
  return honest ? 0 : 1;
}

int run_scenario(std::string_view name, bool verbose) {
  if (name == "basic") return scenario_basic(verbose);
  if (name == "cancel") return scenario_cancel(verbose);
  if (name == "stale") return scenario_stale(verbose);
  if (name == "starvation") return scenario_starvation(verbose);
  if (name == "unknown-stage") return scenario_unknown_stage(verbose);
  if (name == "bulk-yield") return scenario_bulk_yield(verbose);
  if (name == "generations") return scenario_generations(verbose);
  std::fprintf(stderr, "unknown scenario: %.*s\n", static_cast<int>(name.size()), name.data());
  return 2;
}

// ---------------------------------------------------------------------------
// Remote commands
// ---------------------------------------------------------------------------

int connect_remote(const Options& options, net::Client& client) {
  net::ClientConfig config;
  config.host = options.host;
  config.port = options.port;
  config.peer_name = "itfctl";
  config.auth_token = options.token;
  config.client_version = std::string(kVersionString);
  auto connected = net::Client::connect(config);
  if (!connected.ok()) {
    std::fprintf(stderr, "cannot reach the coordinator: %s\n",
                 connected.status().to_string().c_str());
    return 1;
  }
  client = std::move(connected).value();
  return 0;
}

int command_status(const Options& options) {
  net::Client client;
  if (const int code = connect_remote(options, client); code != 0) return code;
  const auto status = client.status();
  if (!status.ok()) {
    std::fprintf(stderr, "%s\n", status.status().to_string().c_str());
    return 1;
  }
  std::printf("%s\n", status.value().to_string().c_str());
  return 0;
}

int command_accounting(const Options& options) {
  net::Client client;
  if (const int code = connect_remote(options, client); code != 0) return code;
  const auto snapshot = client.accounting();
  if (!snapshot.ok()) {
    std::fprintf(stderr, "%s\n", snapshot.status().to_string().c_str());
    return 1;
  }
  std::printf("%s\n", snapshot.value().to_string().c_str());
  return 0;
}

int command_policy(const Options& options) {
  net::Client client;
  if (const int code = connect_remote(options, client); code != 0) return code;
  const auto policy = client.policy();
  if (!policy.ok()) {
    std::fprintf(stderr, "%s\n", policy.status().to_string().c_str());
    return 1;
  }
  std::printf("%s\n", policy.value().to_string().c_str());
  return 0;
}

int command_evaluate(const Options& options) {
  const auto stage = serving_stage_from_label(options.stage);
  const auto traffic = traffic_class_from_label(options.traffic_class);
  if (!stage.has_value()) {
    std::fprintf(stderr, "unknown stage: %s\n", options.stage.c_str());
    return 2;
  }
  if (!traffic.has_value()) {
    std::fprintf(stderr, "unknown class: %s\n", options.traffic_class.c_str());
    return 2;
  }
  net::Client client;
  if (const int code = connect_remote(options, client); code != 0) return code;
  TrafficRequest request_message;
  request_message.declared_class = *traffic;
  request_message.declared_stage = *stage;
  request_message.payload_bytes = options.bytes;
  request_message.direction = FlowDirection::Ingress;
  request_message.binding.policy = client.hello().policy_generation;
  request_message.binding.coordinator_epoch = client.hello().epoch;
  const auto decision = client.evaluate(request_message);
  if (!decision.ok()) {
    std::fprintf(stderr, "%s\n", decision.status().to_string().c_str());
    return 1;
  }
  std::printf("%s\n", decision.value().explain().c_str());
  return 0;
}

int command_snapshot_info(const std::string& path, bool verbose) {
  SnapshotStore store(path);
  auto raw = store.load_raw();
  if (!raw.ok()) {
    std::fprintf(stderr, "%s\n", raw.status().to_string().c_str());
    return 1;
  }
  std::printf("snapshot %s bytes=%zu crc=ok\n", path.c_str(), raw.value().size());
  auto state = store.load();
  if (!state.ok()) {
    std::fprintf(stderr, "%s\n", state.status().to_string().c_str());
    return 1;
  }
  std::printf("%s\n", state.value().to_string().c_str());
  if (verbose) {
    for (const RequestRecord& record : state.value().terminal_requests) {
      std::printf("  terminal %s lifecycle=%s stage=%s bytes_in=%llu bytes_out=%llu\n",
                  record.id.to_string().c_str(), std::string(to_string(record.lifecycle)).c_str(),
                  std::string(to_string(record.stage)).c_str(),
                  static_cast<unsigned long long>(record.bytes_in),
                  static_cast<unsigned long long>(record.bytes_out));
    }
    for (const RequestRecord& record : state.value().interrupted_requests) {
      std::printf("  interrupted %s lifecycle=%s\n", record.id.to_string().c_str(),
                  std::string(to_string(record.lifecycle)).c_str());
    }
  }
  return 0;
}

int command_accel_probe(bool no_kernel) {
  const auto probe = accel::probe(!no_kernel);
  if (!probe.ok()) {
    std::printf("accelerator: %s\n", probe.status().message().c_str());
    return 3;
  }
  std::printf("%s\n", probe.value().to_string().c_str());
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    print_usage();
    return 2;
  }
  const std::string_view command(argv[1]);
  Options options;
  bool verbose = false;

  if (command == "version") {
    std::printf("Inference Traffic Fabric %.*s\n", static_cast<int>(kVersionString.size()),
                kVersionString.data());
    std::printf("api=%u wire_protocol=%u snapshot_format=%u\n", kApiVersion,
                static_cast<unsigned>(kWireProtocolVersion),
                static_cast<unsigned>(kSnapshotFormatVersion));
    std::printf("%s\n", std::string(kCopyrightNotice).c_str());
    return 0;
  }
  if (command == "help" || command == "--help" || command == "-h") {
    print_usage();
    return 0;
  }
  if (command == "accel-probe") {
    if (!parse_options(argc, argv, 2, options)) return 2;
    return command_accel_probe(options.no_kernel);
  }
  if (command == "simulate") {
    if (argc < 3) {
      std::fprintf(stderr, "simulate requires a scenario name\n");
      return 2;
    }
    return run_scenario(argv[2], verbose);
  }
  if (command == "snapshot-info") {
    if (argc < 3) {
      std::fprintf(stderr, "snapshot-info requires a path\n");
      return 2;
    }
    return command_snapshot_info(argv[2], verbose);
  }
  if (command == "status" || command == "accounting" || command == "policy" ||
      command == "evaluate") {
    if (!parse_options(argc, argv, 2, options)) return 2;
    if (command == "status") return command_status(options);
    if (command == "accounting") return command_accounting(options);
    if (command == "policy") return command_policy(options);
    return command_evaluate(options);
  }
  std::fprintf(stderr, "unknown command: %.*s\n", static_cast<int>(command.size()), command.data());
  print_usage();
  return 2;
}
