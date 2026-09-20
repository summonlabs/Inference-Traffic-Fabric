// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "service.hpp"

#include <csignal>
#include <cstdio>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "itf/log.hpp"
#include "itf/serialize.hpp"

namespace itf::service {
namespace {

template <class Fn>
Result<std::vector<std::uint8_t>> build_body(Fn&& fn) {
  Writer writer;
  fn(writer);
  if (!writer.ok()) {
    return Result<std::vector<std::uint8_t>>::failure(writer.fail_code(),
                                                      "response exceeds the frame ceiling");
  }
  return Result<std::vector<std::uint8_t>>::success(writer.take());
}

template <class T, class Decoder>
Result<T> decode_body(ByteSpan payload, Decoder decoder) {
  Reader reader(payload);
  auto value = decoder(reader);
  if (!value.ok()) return value;
  const Status finished = reader.finish();
  if (!finished.ok()) return Result<T>::failure(finished);
  return value;
}

std::atomic<bool> g_interrupted{false};

void handle_interrupt(int) { g_interrupted.store(true); }

}  // namespace

CoordinatorService::CoordinatorService(ServiceConfig config, Clock* clock)
    : config_(std::move(config)), clock_(clock != nullptr ? clock : &steady_clock_singleton()) {
  coordinator_ = std::make_unique<Coordinator>(config_.coordinator, clock_);
  server_ = std::make_unique<net::Server>(config_.server, this);
}

CoordinatorService::~CoordinatorService() { stop(); }

Status CoordinatorService::start() {
  if (config_.persist && !config_.snapshot_path.empty()) {
    SnapshotStore store(config_.snapshot_path);
    auto loaded = store.load();
    if (loaded.ok()) {
      const Status booted = coordinator_->boot_from(loaded.value()).status();
      if (!booted.ok()) return booted;
      log_info("service", std::string("restored coordinator state: ") +
                              loaded.value().to_string());
    } else if (loaded.code() == StatusCode::NotFound) {
      const Status booted = coordinator_->boot_fresh().status();
      if (!booted.ok()) return booted;
      log_info("service", "no durable state was present; a fresh epoch was established");
    } else {
      // A durable document that cannot be trusted is refused, never partially
      // applied and never silently replaced by a fresh epoch.
      return Status(loaded.code(),
                    std::string("durable state was refused: ") + loaded.status().to_string());
    }
  } else {
    const Status booted = coordinator_->boot_fresh().status();
    if (!booted.ok()) return booted;
  }
  return server_->start();
}

void CoordinatorService::stop() {
  if (server_) server_->stop();
  if (config_.persist && !config_.snapshot_path.empty()) {
    const Status saved = save_snapshot();
    if (!saved.ok()) {
      log_warn("service", std::string("final snapshot failed: ") + saved.to_string());
    }
  }
}

void CoordinatorService::describe_authority(wire::HelloResponse& response) {
  const AuthorityStamp stamp = coordinator_->authority();
  const CoordinatorStatus status = coordinator_->status();
  response.coordinator_boot = stamp.incarnation;
  response.epoch = stamp.epoch;
  response.policy_generation = stamp.policy;
  response.topology_generation = status.topology_generation;
  response.topology_state = status.topology_state;
  response.authority_sequence = stamp.sequence.value();
}

Status CoordinatorService::save_snapshot() {
  std::lock_guard<std::mutex> guard(snapshot_mutex_);
  if (!config_.persist || config_.snapshot_path.empty()) {
    return Status(StatusCode::Unsupported, "persistence is disabled for this service");
  }
  auto state = coordinator_->export_state();
  if (!state.ok()) return state.status();
  SnapshotStore store(config_.snapshot_path);
  const Status saved = store.save(state.value());
  if (!saved.ok()) {
    last_snapshot_error_ = saved.to_string();
    return saved;
  }
  last_snapshot_nanos_ = clock_->now_nanos();
  last_snapshot_error_.clear();
  return Status::success();
}

void CoordinatorService::maybe_snapshot(bool mutated) {
  if (!mutated || !config_.persist || config_.snapshot_path.empty()) return;
  const std::int64_t now = clock_->now_nanos();
  {
    std::lock_guard<std::mutex> guard(snapshot_mutex_);
    if (last_snapshot_nanos_ != 0 && now - last_snapshot_nanos_ < config_.snapshot_interval_nanos) {
      return;
    }
  }
  const Status saved = save_snapshot();
  if (!saved.ok()) {
    log_warn("service", std::string("snapshot failed: ") + saved.to_string());
  }
}

Result<std::vector<std::uint8_t>> CoordinatorService::handle(Context& context,
                                                             wire::MessageType type,
                                                             ByteSpan payload) {
  bool mutated = false;
  Result<std::vector<std::uint8_t>> result =
      Result<std::vector<std::uint8_t>>::failure(StatusCode::Unsupported, "unhandled message");
  switch (type) {
    case wire::MessageType::Heartbeat: {
      auto body = decode_body<wire::HeartbeatMessage>(
          payload, [](Reader& reader) { return wire::HeartbeatMessage::decode(reader); });
      if (!body.ok()) return Result<std::vector<std::uint8_t>>::failure(body.status());
      wire::SimpleAck ack;
      result = build_body([&](Writer& writer) { ack.encode(writer); });
      break;
    }
    case wire::MessageType::RegisterRequest: {
      auto body = decode_body<wire::RegisterRequestMessage>(
          payload, [](Reader& reader) { return wire::RegisterRequestMessage::decode(reader); });
      if (!body.ok()) return Result<std::vector<std::uint8_t>>::failure(body.status());
      auto generation = coordinator_->register_request(body.value().request);
      if (!generation.ok()) {
        return Result<std::vector<std::uint8_t>>::failure(generation.status());
      }
      mutated = true;
      wire::RegisterRequestResponse response;
      response.request = body.value().request;
      response.generation = generation.value();
      response.authority = coordinator_->authority();
      result = build_body([&](Writer& writer) { response.encode(writer); });
      break;
    }
    case wire::MessageType::RegisterAttempt: {
      auto body = decode_body<wire::RegisterAttemptMessage>(
          payload, [](Reader& reader) { return wire::RegisterAttemptMessage::decode(reader); });
      if (!body.ok()) return Result<std::vector<std::uint8_t>>::failure(body.status());
      auto generation = coordinator_->register_attempt(body.value().request, body.value().attempt,
                                                        body.value().model_hash);
      if (!generation.ok()) {
        return Result<std::vector<std::uint8_t>>::failure(generation.status());
      }
      mutated = true;
      wire::RegisterAttemptResponse response;
      response.request = body.value().request;
      response.attempt = body.value().attempt;
      response.generation = generation.value();
      if (body.value().model_hash != 0) {
        auto model = coordinator_->model_generation(body.value().model_hash);
        if (model.ok()) response.model_generation = model.value();
      }
      response.authority = coordinator_->authority();
      result = build_body([&](Writer& writer) { response.encode(writer); });
      break;
    }
    case wire::MessageType::StageUpdate: {
      auto body = decode_body<wire::StageUpdateMessage>(
          payload, [](Reader& reader) { return wire::StageUpdateMessage::decode(reader); });
      if (!body.ok()) return Result<std::vector<std::uint8_t>>::failure(body.status());
      const Status advanced = coordinator_->advance_stage(body.value().request, body.value().attempt,
                                                          body.value().stage);
      if (!advanced.ok()) return Result<std::vector<std::uint8_t>>::failure(advanced);
      mutated = true;
      wire::SimpleAck ack;
      result = build_body([&](Writer& writer) { ack.encode(writer); });
      break;
    }
    case wire::MessageType::CancelRequest: {
      auto body = decode_body<wire::CancelRequestMessage>(
          payload, [](Reader& reader) { return wire::CancelRequestMessage::decode(reader); });
      if (!body.ok()) return Result<std::vector<std::uint8_t>>::failure(body.status());
      const Status cancelled = coordinator_->cancel_request(body.value().request, body.value().reason);
      if (!cancelled.ok()) return Result<std::vector<std::uint8_t>>::failure(cancelled);
      mutated = true;
      wire::SimpleAck ack;
      result = build_body([&](Writer& writer) { ack.encode(writer); });
      break;
    }
    case wire::MessageType::FailAttempt: {
      auto body = decode_body<wire::FailAttemptMessage>(
          payload, [](Reader& reader) { return wire::FailAttemptMessage::decode(reader); });
      if (!body.ok()) return Result<std::vector<std::uint8_t>>::failure(body.status());
      auto outcome = coordinator_->fail_attempt(body.value().request, body.value().attempt,
                                                body.value().reason);
      if (!outcome.ok()) return Result<std::vector<std::uint8_t>>::failure(outcome.status());
      mutated = true;
      wire::SimpleAck ack;
      result = build_body([&](Writer& writer) { ack.encode(writer); });
      break;
    }
    case wire::MessageType::ReplaceAttempt: {
      auto body = decode_body<wire::ReplaceAttemptMessage>(
          payload, [](Reader& reader) { return wire::ReplaceAttemptMessage::decode(reader); });
      if (!body.ok()) return Result<std::vector<std::uint8_t>>::failure(body.status());
      auto outcome = coordinator_->replace_attempt(body.value().request, body.value().superseded,
                                                   body.value().replacement,
                                                   body.value().model_hash);
      if (!outcome.ok()) return Result<std::vector<std::uint8_t>>::failure(outcome.status());
      mutated = true;
      wire::RegisterAttemptResponse response;
      response.request = body.value().request;
      response.attempt = body.value().replacement;
      response.generation = outcome.value();
      response.authority = coordinator_->authority();
      result = build_body([&](Writer& writer) { response.encode(writer); });
      break;
    }
    case wire::MessageType::EvaluateTraffic: {
      auto body = decode_body<wire::EvaluateTrafficMessage>(
          payload, [](Reader& reader) { return wire::EvaluateTrafficMessage::decode(reader); });
      if (!body.ok()) return Result<std::vector<std::uint8_t>>::failure(body.status());
      wire::DecisionResponseMessage response;
      response.decision = coordinator_->evaluate(body.value().request);
      (void)context;
      result = build_body([&](Writer& writer) { response.encode(writer); });
      break;
    }
    case wire::MessageType::OpenFlow: {
      auto body = decode_body<wire::OpenFlowMessage>(
          payload, [](Reader& reader) { return wire::OpenFlowMessage::decode(reader); });
      if (!body.ok()) return Result<std::vector<std::uint8_t>>::failure(body.status());
      auto flow = coordinator_->open_flow(body.value().decision, body.value().declared_bytes);
      if (!flow.ok()) return Result<std::vector<std::uint8_t>>::failure(flow.status());
      mutated = true;
      wire::OpenFlowResponse response;
      response.flow = flow.value();
      response.authority = coordinator_->authority();
      result = build_body([&](Writer& writer) { response.encode(writer); });
      break;
    }
    case wire::MessageType::RegisterTransfer: {
      auto body = decode_body<wire::RegisterTransferMessage>(
          payload, [](Reader& reader) { return wire::RegisterTransferMessage::decode(reader); });
      if (!body.ok()) return Result<std::vector<std::uint8_t>>::failure(body.status());
      auto registered = coordinator_->register_transfer(body.value().registration);
      if (!registered.ok()) {
        return Result<std::vector<std::uint8_t>>::failure(registered.status());
      }
      mutated = true;
      wire::RegisterTransferResponse response;
      response.transfer = registered.value();
      response.state = TransferState::Registered;
      // The registration bound the current model and state generations; the
      // caller needs them to build a generation-bound authorization request.
      auto model = coordinator_->model_generation(body.value().registration.model_hash);
      if (model.ok()) response.model_generation = model.value();
      auto state = coordinator_->state_generation(body.value().registration.state_hash);
      if (state.ok()) response.state_generation = state.value();
      response.authority = coordinator_->authority();
      result = build_body([&](Writer& writer) { response.encode(writer); });
      break;
    }
    case wire::MessageType::AuthorizeTransfer: {
      auto body = decode_body<wire::AuthorizeTransferMessage>(
          payload, [](Reader& reader) { return wire::AuthorizeTransferMessage::decode(reader); });
      if (!body.ok()) return Result<std::vector<std::uint8_t>>::failure(body.status());
      auto authorization = coordinator_->authorize_transfer(body.value().request);
      if (!authorization.ok()) {
        return Result<std::vector<std::uint8_t>>::failure(authorization.status());
      }
      mutated = true;
      wire::TransferDecisionResponse response;
      response.authorization = authorization.value();
      response.subject = body.value().request.subject.value_or(TrafficSubject{});
      response.binding = body.value().request.binding;
      response.authority = authorization.value().authority;
      result = build_body([&](Writer& writer) { response.encode(writer); });
      break;
    }
    case wire::MessageType::TransferComplete: {
      auto body = decode_body<wire::TransferCompleteMessage>(
          payload, [](Reader& reader) { return wire::TransferCompleteMessage::decode(reader); });
      if (!body.ok()) return Result<std::vector<std::uint8_t>>::failure(body.status());
      const Status completed =
          coordinator_->complete_transfer(body.value().transfer, body.value().verified_bytes);
      if (!completed.ok()) return Result<std::vector<std::uint8_t>>::failure(completed);
      mutated = true;
      wire::SimpleAck ack;
      result = build_body([&](Writer& writer) { ack.encode(writer); });
      break;
    }
    case wire::MessageType::CompletionPublish: {
      auto body = decode_body<wire::CompletionPublishMessage>(
          payload, [](Reader& reader) { return wire::CompletionPublishMessage::decode(reader); });
      if (!body.ok()) return Result<std::vector<std::uint8_t>>::failure(body.status());
      auto outcome = coordinator_->publish_completion(body.value().publication);
      if (!outcome.ok()) return Result<std::vector<std::uint8_t>>::failure(outcome.status());
      mutated = true;
      wire::CompletionResponse response;
      response.outcome = outcome.value();
      result = build_body([&](Writer& writer) { response.encode(writer); });
      break;
    }
    case wire::MessageType::AccountingQuery: {
      auto body = decode_body<wire::AccountingQueryMessage>(
          payload, [](Reader& reader) { return wire::AccountingQueryMessage::decode(reader); });
      if (!body.ok()) return Result<std::vector<std::uint8_t>>::failure(body.status());
      wire::AccountingResponseMessage response;
      response.snapshot = coordinator_->accounting();
      result = build_body([&](Writer& writer) { response.encode(writer); });
      break;
    }
    case wire::MessageType::StatusQuery: {
      auto body = decode_body<wire::StatusQueryMessage>(
          payload, [](Reader& reader) { return wire::StatusQueryMessage::decode(reader); });
      if (!body.ok()) return Result<std::vector<std::uint8_t>>::failure(body.status());
      wire::StatusResponseMessage response;
      response.status = coordinator_->status();
      result = build_body([&](Writer& writer) { response.encode(writer); });
      break;
    }
    case wire::MessageType::PolicyQuery: {
      auto body = decode_body<wire::PolicyQueryMessage>(
          payload, [](Reader& reader) { return wire::PolicyQueryMessage::decode(reader); });
      if (!body.ok()) return Result<std::vector<std::uint8_t>>::failure(body.status());
      wire::PolicyResponseMessage response;
      response.policy = coordinator_->policy();
      result = build_body([&](Writer& writer) { response.encode(writer); });
      break;
    }
    case wire::MessageType::TopologyPublish: {
      auto body = decode_body<wire::TopologyPublishMessage>(
          payload, [](Reader& reader) { return wire::TopologyPublishMessage::decode(reader); });
      if (!body.ok()) return Result<std::vector<std::uint8_t>>::failure(body.status());
      auto generation = coordinator_->publish_topology(body.value().evidence);
      if (!generation.ok()) {
        return Result<std::vector<std::uint8_t>>::failure(generation.status());
      }
      mutated = true;
      wire::TopologyResponseMessage response;
      response.generation = generation.value();
      response.state = EvidenceState::Current;
      response.provenance = body.value().evidence.provenance;
      result = build_body([&](Writer& writer) { response.encode(writer); });
      break;
    }
    case wire::MessageType::PublishSloContract: {
      auto body = decode_body<wire::PublishSloContractMessage>(
          payload, [](Reader& reader) { return wire::PublishSloContractMessage::decode(reader); });
      if (!body.ok()) return Result<std::vector<std::uint8_t>>::failure(body.status());
      auto contract = coordinator_->register_slo_contract(
          body.value().request, body.value().tail_latency_budget_nanos,
          body.value().registration_deadline_nanos, body.value().min_stream_share_per_mille);
      if (!contract.ok()) return Result<std::vector<std::uint8_t>>::failure(contract.status());
      mutated = true;
      wire::SloContractResponse response;
      response.contract = contract.value();
      result = build_body([&](Writer& writer) { response.encode(writer); });
      break;
    }
    case wire::MessageType::IssueRouteDecision: {
      auto body = decode_body<wire::IssueRouteDecisionMessage>(
          payload, [](Reader& reader) { return wire::IssueRouteDecisionMessage::decode(reader); });
      if (!body.ok()) return Result<std::vector<std::uint8_t>>::failure(body.status());
      auto decision = coordinator_->issue_route_decision(
          body.value().request, body.value().attempt, body.value().target_node,
          body.value().lateral, body.value().disaggregated_handoff);
      if (!decision.ok()) return Result<std::vector<std::uint8_t>>::failure(decision.status());
      mutated = true;
      wire::RouteDecisionResponse response;
      response.decision = decision.value();
      result = build_body([&](Writer& writer) { response.encode(writer); });
      break;
    }
    case wire::MessageType::AdvanceModelGeneration: {
      auto body = decode_body<wire::AdvanceGenerationMessage>(
          payload, [](Reader& reader) { return wire::AdvanceGenerationMessage::decode(reader); });
      if (!body.ok()) return Result<std::vector<std::uint8_t>>::failure(body.status());
      std::optional<ModelGeneration> expected;
      if (body.value().has_expected) {
        expected = ModelGeneration::from(body.value().expected_generation);
      }
      wire::ModelGenerationResponse response;
      response.model_hash = body.value().target_hash;
      if (body.value().state_target) {
        std::optional<StateGeneration> state_expected;
        if (body.value().has_expected) {
          state_expected = StateGeneration::from(body.value().expected_generation);
        }
        if (body.value().revalidate_only) {
          auto state = coordinator_->revalidate_state_generation(body.value().target_hash);
          if (!state.ok()) return Result<std::vector<std::uint8_t>>::failure(state.status());
          response.state_generation = state.value();
        } else {
          auto state =
              coordinator_->advance_state_generation(body.value().target_hash, state_expected);
          if (!state.ok()) return Result<std::vector<std::uint8_t>>::failure(state.status());
          response.state_generation = state.value();
        }
        mutated = true;
        result = build_body([&](Writer& writer) { response.encode(writer); });
        break;
      }
      if (body.value().revalidate_only) {
        auto model = coordinator_->revalidate_model_generation(body.value().target_hash);
        auto state = coordinator_->revalidate_state_generation(body.value().target_hash);
        if (!model.ok() && !state.ok()) {
          return Result<std::vector<std::uint8_t>>::failure(model.status());
        }
        if (model.ok()) response.generation = model.value();
        if (state.ok()) response.state_generation = state.value();
      } else {
        auto model = coordinator_->set_model_generation(body.value().target_hash, expected);
        if (!model.ok()) return Result<std::vector<std::uint8_t>>::failure(model.status());
        response.generation = model.value();
      }
      mutated = true;
      result = build_body([&](Writer& writer) { response.encode(writer); });
      break;
    }
    case wire::MessageType::Shutdown: {
      wire::SimpleAck ack;
      ack.detail = "shutdown accepted";
      request_shutdown();
      result = build_body([&](Writer& writer) { ack.encode(writer); });
      break;
    }
    default:
      return Result<std::vector<std::uint8_t>>::failure(
          StatusCode::Unsupported, "this endpoint does not handle the requested message type");
  }
  maybe_snapshot(mutated);
  return result;
}

int run_coordinator_service(const ServiceConfig& config, bool verbose) {
  Logger::instance().set_level(verbose ? LogLevel::kInfo : LogLevel::kWarn);
  (void)std::signal(SIGINT, handle_interrupt);
  (void)std::signal(SIGTERM, handle_interrupt);
  CoordinatorService service(config, &steady_clock_singleton());
  const Status started = service.start();
  if (!started.ok()) {
    (void)std::fprintf(stderr, "coordinator failed to start: %s\n", started.to_string().c_str());
    return 2;
  }
  std::printf("coordinator listening on %s:%u epoch=%llu\n", config.server.bind_host.c_str(),
              static_cast<unsigned>(service.server().bound_port()),
              static_cast<unsigned long long>(service.coordinator().epoch().value()));
  (void)std::fflush(stdout);
  while (!service.shutdown_requested() && !g_interrupted.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  service.stop();
  std::printf("coordinator stopped: %s\n", service.coordinator().accounting().to_string().c_str());
  return 0;
}

}  // namespace itf::service
