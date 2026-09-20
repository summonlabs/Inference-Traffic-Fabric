// Inference Traffic Fabric - coordinator client.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef ITF_CLIENT_HPP
#define ITF_CLIENT_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "itf/accounting.hpp"
#include "itf/coordinator.hpp"
#include "itf/decision.hpp"
#include "itf/error.hpp"
#include "itf/ids.hpp"
#include "itf/messages.hpp"
#include "itf/net.hpp"
#include "itf/policy.hpp"
#include "itf/traffic.hpp"
#include "itf/wire.hpp"

namespace itf::net {

struct ClientConfig {
  std::string host = "127.0.0.1";
  std::uint16_t port = 0;
  std::string peer_name = "client";
  std::string auth_token;
  std::string client_version = "1.0.0";
  std::uint32_t max_frame_payload = 256U * 1024U;
  std::int64_t io_timeout_nanos = 10000000000LL;
  std::int64_t connect_timeout_nanos = 5000000000LL;
};

/// Blocking, single-threaded client. Each request/response pair is
/// correlated, so a response that does not match the outstanding correlation
/// is refused rather than silently accepted.
class Client {
 public:
  Client() = default;
  ~Client();

  Client(Client&& other) noexcept;
  Client& operator=(Client&& other) noexcept;
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  [[nodiscard]] static Result<Client> connect(const ClientConfig& config);

  [[nodiscard]] bool connected() const noexcept { return stream_.valid(); }
  [[nodiscard]] const wire::HelloResponse& hello() const noexcept { return hello_; }
  [[nodiscard]] std::uint32_t max_frame_payload() const noexcept { return max_frame_payload_; }
  [[nodiscard]] std::uint64_t frames_sent() const noexcept { return frames_sent_; }
  [[nodiscard]] std::uint64_t frames_received() const noexcept { return frames_received_; }

  Result<wire::SimpleAck> heartbeat();
  Result<wire::RegisterRequestResponse> register_request(RequestId request,
                                                         std::uint64_t deadline_after_nanos);
  Result<wire::RegisterAttemptResponse> register_attempt(RequestId request, AttemptId attempt,
                                                         std::uint64_t model_hash);
  Result<wire::SimpleAck> advance_stage(RequestId request, AttemptId attempt, ServingStage stage);
  Result<wire::SimpleAck> cancel_request(RequestId request, ReasonCode reason);
  Result<wire::SimpleAck> fail_attempt(RequestId request, AttemptId attempt, ReasonCode reason);
  Result<wire::RegisterAttemptResponse> replace_attempt(RequestId request, AttemptId superseded,
                                                        AttemptId replacement,
                                                        std::uint64_t model_hash);
  Result<Decision> evaluate(const TrafficRequest& request);
  Result<wire::OpenFlowResponse> open_flow(const Decision& decision, std::uint64_t declared_bytes);
  Result<wire::RegisterTransferResponse> register_transfer(const TransferRegistration& registration);
  Result<wire::TransferDecisionResponse> authorize_transfer(const TrafficRequest& request);
  Result<PublishOutcome> publish_completion(const CompletionPublication& publication);
  Result<AccountingSnapshot> accounting();
  Result<CoordinatorStatus> status();
  Result<PolicyConfig> policy();
  Result<wire::TopologyResponseMessage> publish_topology(const TopologyEvidence& evidence);
  Result<wire::SloContractResponse> publish_slo_contract(RequestId request,
                                                         std::uint64_t tail_latency_budget_nanos,
                                                         std::uint64_t registration_deadline_nanos,
                                                         std::uint32_t min_stream_share_per_mille);
  Result<wire::RouteDecisionResponse> issue_route_decision(RequestId request, AttemptId attempt,
                                                           std::uint32_t target_node, bool lateral,
                                                           bool disaggregated_handoff);
  Result<wire::ModelGenerationResponse> advance_model_generation(
      std::uint64_t model_hash, std::optional<ModelGeneration> expected, bool revalidate_only);
  Result<wire::ModelGenerationResponse> advance_state_generation(
      std::uint64_t state_hash, std::optional<StateGeneration> expected, bool revalidate_only);

  /// Raw frame exchange, used by the data plane and by adversarial tests.
  Result<std::vector<std::uint8_t>> exchange(wire::MessageType type, ByteSpan payload);

  /// Sends a frame without waiting for a response.
  Status send_frame(wire::MessageType type, ByteSpan payload);
  /// Receives the next frame of the expected type, enforcing strict ordering.
  Result<std::vector<std::uint8_t>> receive_frame(wire::MessageType expected_type);

  /// Sends a frame carrying an explicit sequence number. Used by the replay
  /// and duplicate-detection tests.
  Status send_frame_raw(wire::MessageType type, ByteSpan payload, std::uint64_t sequence,
                        std::uint64_t correlation);

  void close() noexcept;

 private:
  Status handshake(const ClientConfig& config);

  TcpStream stream_;
  wire::HelloResponse hello_;
  std::uint32_t max_frame_payload_ = 0;
  std::uint64_t send_sequence_ = 0;
  std::uint64_t last_received_sequence_ = 0;
  std::uint64_t frames_sent_ = 0;
  std::uint64_t frames_received_ = 0;
  std::int64_t io_timeout_nanos_ = 10000000000LL;
};

}  // namespace itf::net

#endif  // ITF_CLIENT_HPP
