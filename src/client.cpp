// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "itf/client.hpp"

#include <cstring>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "itf/crc32c.hpp"
#include "itf/version.hpp"

namespace itf::net {
namespace {

template <class Tag>
Id128<Tag> random_id() {
  std::random_device device;
  std::uint64_t high = (static_cast<std::uint64_t>(device()) << 32U) | device();
  std::uint64_t low = (static_cast<std::uint64_t>(device()) << 32U) | device();
  if (high == 0 && low == 0) low = 1;
  return Id128<Tag>(high, low);
}

}  // namespace

Client::~Client() { close(); }

Client::Client(Client&& other) noexcept
    : stream_(std::move(other.stream_)),
      hello_(std::move(other.hello_)),
      max_frame_payload_(other.max_frame_payload_),
      send_sequence_(other.send_sequence_),
      last_received_sequence_(other.last_received_sequence_),
      frames_sent_(other.frames_sent_),
      frames_received_(other.frames_received_),
      io_timeout_nanos_(other.io_timeout_nanos_) {
  other.max_frame_payload_ = 0;
  other.send_sequence_ = 0;
  other.last_received_sequence_ = 0;
}

Client& Client::operator=(Client&& other) noexcept {
  if (this != &other) {
    close();
    stream_ = std::move(other.stream_);
    hello_ = std::move(other.hello_);
    max_frame_payload_ = other.max_frame_payload_;
    send_sequence_ = other.send_sequence_;
    last_received_sequence_ = other.last_received_sequence_;
    frames_sent_ = other.frames_sent_;
    frames_received_ = other.frames_received_;
    io_timeout_nanos_ = other.io_timeout_nanos_;
    other.max_frame_payload_ = 0;
    other.send_sequence_ = 0;
    other.last_received_sequence_ = 0;
  }
  return *this;
}

void Client::close() noexcept { stream_.close(); }

Status Client::handshake(const ClientConfig& config) {
  // The provisional limit covers the handshake itself; the negotiated limit is
  // installed once the server has answered.
  max_frame_payload_ = config.max_frame_payload;
  wire::HelloRequest hello;
  hello.peer_name = config.peer_name;
  hello.auth_token = config.auth_token;
  hello.client_version = config.client_version;
  hello.client_boot = random_id<BootIdTag>();
  hello.max_frame_payload = config.max_frame_payload;
  Writer writer;
  hello.encode(writer);
  if (!writer.ok()) {
    return Status(writer.fail_code(), "handshake request exceeds the frame ceiling");
  }
  const Status sent = send_frame(wire::MessageType::HelloRequest,
                                 ByteSpan(writer.buffer().data(), writer.buffer().size()));
  if (!sent.ok()) return sent;
  auto response = receive_frame(wire::MessageType::HelloResponse);
  if (!response.ok()) return response.status();
  const std::vector<std::uint8_t>& payload = response.value();
  Reader reader(ByteSpan(payload.data(), payload.size()));
  auto decoded = wire::HelloResponse::decode(reader);
  if (!decoded.ok()) return decoded.status();
  const Status finished = reader.finish();
  if (!finished.ok()) return finished;
  hello_ = std::move(decoded).value();
  max_frame_payload_ =
      config.max_frame_payload < hello_.max_frame_payload ? config.max_frame_payload
                                                          : hello_.max_frame_payload;
  return Status::success();
}

Result<Client> Client::connect(const ClientConfig& config) {
  if (!is_safe_label(config.peer_name)) {
    return Result<Client>::failure(StatusCode::InvalidArgument,
                                   "peer name must be a bounded label");
  }
  auto stream = TcpStream::connect(config.host, config.port, config.connect_timeout_nanos);
  if (!stream.ok()) return Result<Client>::failure(stream.status());
  Client client;
  client.stream_ = std::move(stream).value();
  client.io_timeout_nanos_ = config.io_timeout_nanos;
  const Status handshaken = client.handshake(config);
  if (!handshaken.ok()) {
    client.close();
    return Result<Client>::failure(handshaken);
  }
  return Result<Client>::success(std::move(client));
}

Status Client::send_frame_raw(wire::MessageType type, ByteSpan payload, std::uint64_t sequence,
                              std::uint64_t correlation) {
  if (!stream_.valid()) return Status(StatusCode::Closed, "client is not connected");
  if (payload.size() > max_frame_payload_) {
    return Status(StatusCode::BoundsExceeded, "payload exceeds the negotiated frame limit");
  }
  std::vector<std::uint8_t> header(wire::kHeaderBytes, 0);
  wire::FrameHeader frame;
  frame.type = type;
  frame.payload_length = static_cast<std::uint32_t>(payload.size());
  frame.sequence = sequence;
  frame.correlation = correlation;
  frame.payload_crc32c = Crc32c::compute(payload);
  wire::encode_header(frame, MutableByteSpan(header.data(), header.size()));
  Status status = stream_.write_all(ByteSpan(header.data(), header.size()));
  if (status.ok() && !payload.empty()) status = stream_.write_all(payload);
  if (status.ok()) frames_sent_ += 1;
  return status;
}

Status Client::send_frame(wire::MessageType type, ByteSpan payload) {
  send_sequence_ += 1;
  return send_frame_raw(type, payload, send_sequence_, 0);
}

Result<std::vector<std::uint8_t>> Client::receive_frame(wire::MessageType expected_type) {
  if (!stream_.valid()) return Result<std::vector<std::uint8_t>>::failure(StatusCode::Closed, "client is not connected");
  std::vector<std::uint8_t> header(wire::kHeaderBytes, 0);
  const Status read_header =
      stream_.read_exact(MutableByteSpan(header.data(), header.size()), io_timeout_nanos_);
  if (!read_header.ok()) return Result<std::vector<std::uint8_t>>::failure(read_header);
  const auto decoded = wire::decode_header(ByteSpan(header.data(), header.size()),
                                           max_frame_payload_);
  if (!decoded.ok()) return Result<std::vector<std::uint8_t>>::failure(decoded.status());
  const wire::FrameHeader frame = decoded.value();
  std::vector<std::uint8_t> payload(frame.payload_length, 0);
  if (!payload.empty()) {
    const Status read_payload = stream_.read_exact(
        MutableByteSpan(payload.data(), payload.size()), io_timeout_nanos_);
    if (!read_payload.ok()) return Result<std::vector<std::uint8_t>>::failure(read_payload);
  }
  if (Crc32c::compute(ByteSpan(payload.data(), payload.size())) != frame.payload_crc32c) {
    return Result<std::vector<std::uint8_t>>::failure(StatusCode::IntegrityMismatch,
                                                      "frame payload checksum mismatch");
  }
  if (frame.sequence <= last_received_sequence_) {
    return Result<std::vector<std::uint8_t>>::failure(
        StatusCode::ReplayRejected, "server frame sequence is not strictly increasing");
  }
  last_received_sequence_ = frame.sequence;
  frames_received_ += 1;
  if (frame.type == wire::MessageType::ErrorResponse) {
    Reader reader(ByteSpan(payload.data(), payload.size()));
    auto error = wire::ErrorResponse::decode(reader);
    if (!error.ok()) return Result<std::vector<std::uint8_t>>::failure(error.status());
    return Result<std::vector<std::uint8_t>>::failure(
        Status(error.value().code, error.value().detail));
  }
  if (frame.type != expected_type) {
    return Result<std::vector<std::uint8_t>>::failure(
        StatusCode::MalformedInput, "server responded with an unexpected message type");
  }
  return Result<std::vector<std::uint8_t>>::success(std::move(payload));
}

Result<std::vector<std::uint8_t>> Client::exchange(wire::MessageType type, ByteSpan payload) {
  const wire::MessageType expected = wire::response_type_for(type);
  if (expected == wire::MessageType::Invalid) {
    return Result<std::vector<std::uint8_t>>::failure(StatusCode::InvalidArgument,
                                                      "message type has no response mapping");
  }
  const Status sent = send_frame(type, payload);
  if (!sent.ok()) return Result<std::vector<std::uint8_t>>::failure(sent);
  return receive_frame(expected);
}

Result<wire::SimpleAck> Client::heartbeat() {
  wire::HeartbeatMessage body;
  Writer writer;
  body.encode(writer);
  auto response =
      exchange(wire::MessageType::Heartbeat, ByteSpan(writer.buffer().data(), writer.size()));
  if (!response.ok()) return Result<wire::SimpleAck>::failure(response.status());
  const std::vector<std::uint8_t>& payload = response.value();
  Reader reader(ByteSpan(payload.data(), payload.size()));
  auto ack = wire::SimpleAck::decode(reader);
  if (!ack.ok()) return ack;
  const Status finished = reader.finish();
  if (!finished.ok()) return Result<wire::SimpleAck>::failure(finished);
  return ack;
}

Result<wire::RegisterRequestResponse> Client::register_request(RequestId request,
                                                               std::uint64_t deadline_after_nanos) {
  wire::RegisterRequestMessage body;
  body.request = request;
  body.deadline_after_nanos = deadline_after_nanos;
  Writer writer;
  body.encode(writer);
  auto response = exchange(wire::MessageType::RegisterRequest,
                           ByteSpan(writer.buffer().data(), writer.size()));
  if (!response.ok()) return Result<wire::RegisterRequestResponse>::failure(response.status());
  const std::vector<std::uint8_t>& payload = response.value();
  Reader reader(ByteSpan(payload.data(), payload.size()));
  auto decoded = wire::RegisterRequestResponse::decode(reader);
  if (!decoded.ok()) return decoded;
  const Status finished = reader.finish();
  if (!finished.ok()) return Result<wire::RegisterRequestResponse>::failure(finished);
  return decoded;
}

Result<wire::RegisterAttemptResponse> Client::register_attempt(RequestId request, AttemptId attempt,
                                                               std::uint64_t model_hash) {
  wire::RegisterAttemptMessage body;
  body.request = request;
  body.attempt = attempt;
  body.model_hash = model_hash;
  Writer writer;
  body.encode(writer);
  auto response = exchange(wire::MessageType::RegisterAttempt,
                           ByteSpan(writer.buffer().data(), writer.size()));
  if (!response.ok()) return Result<wire::RegisterAttemptResponse>::failure(response.status());
  const std::vector<std::uint8_t>& payload = response.value();
  Reader reader(ByteSpan(payload.data(), payload.size()));
  auto decoded = wire::RegisterAttemptResponse::decode(reader);
  if (!decoded.ok()) return decoded;
  const Status finished = reader.finish();
  if (!finished.ok()) return Result<wire::RegisterAttemptResponse>::failure(finished);
  return decoded;
}

Result<wire::SimpleAck> Client::advance_stage(RequestId request, AttemptId attempt,
                                              ServingStage stage) {
  wire::StageUpdateMessage body;
  body.request = request;
  body.attempt = attempt;
  body.stage = stage;
  Writer writer;
  body.encode(writer);
  auto response = exchange(wire::MessageType::StageUpdate,
                           ByteSpan(writer.buffer().data(), writer.size()));
  if (!response.ok()) return Result<wire::SimpleAck>::failure(response.status());
  const std::vector<std::uint8_t>& payload = response.value();
  Reader reader(ByteSpan(payload.data(), payload.size()));
  auto ack = wire::SimpleAck::decode(reader);
  if (!ack.ok()) return ack;
  const Status finished = reader.finish();
  if (!finished.ok()) return Result<wire::SimpleAck>::failure(finished);
  return ack;
}

Result<wire::SimpleAck> Client::cancel_request(RequestId request, ReasonCode reason) {
  wire::CancelRequestMessage body;
  body.request = request;
  body.reason = reason;
  Writer writer;
  body.encode(writer);
  auto response =
      exchange(wire::MessageType::CancelRequest, ByteSpan(writer.buffer().data(), writer.size()));
  if (!response.ok()) return Result<wire::SimpleAck>::failure(response.status());
  const std::vector<std::uint8_t>& payload = response.value();
  Reader reader(ByteSpan(payload.data(), payload.size()));
  auto ack = wire::SimpleAck::decode(reader);
  if (!ack.ok()) return ack;
  const Status finished = reader.finish();
  if (!finished.ok()) return Result<wire::SimpleAck>::failure(finished);
  return ack;
}

Result<wire::SimpleAck> Client::fail_attempt(RequestId request, AttemptId attempt,
                                                 ReasonCode reason) {
  wire::FailAttemptMessage body;
  body.request = request;
  body.attempt = attempt;
  body.reason = reason;
  Writer writer;
  body.encode(writer);
  auto response =
      exchange(wire::MessageType::FailAttempt, ByteSpan(writer.buffer().data(), writer.size()));
  if (!response.ok()) return Result<wire::SimpleAck>::failure(response.status());
  const std::vector<std::uint8_t>& payload = response.value();
  Reader reader(ByteSpan(payload.data(), payload.size()));
  auto ack = wire::SimpleAck::decode(reader);
  if (!ack.ok()) return ack;
  const Status finished = reader.finish();
  if (!finished.ok()) return Result<wire::SimpleAck>::failure(finished);
  return ack;
}

Result<wire::RegisterAttemptResponse> Client::replace_attempt(RequestId request,
                                                              AttemptId superseded,
                                                              AttemptId replacement,
                                                              std::uint64_t model_hash) {
  wire::ReplaceAttemptMessage body;
  body.request = request;
  body.superseded = superseded;
  body.replacement = replacement;
  body.model_hash = model_hash;
  Writer writer;
  body.encode(writer);
  auto response =
      exchange(wire::MessageType::ReplaceAttempt, ByteSpan(writer.buffer().data(), writer.size()));
  if (!response.ok()) return Result<wire::RegisterAttemptResponse>::failure(response.status());
  const std::vector<std::uint8_t>& payload = response.value();
  Reader reader(ByteSpan(payload.data(), payload.size()));
  auto decoded = wire::RegisterAttemptResponse::decode(reader);
  if (!decoded.ok()) return decoded;
  const Status finished = reader.finish();
  if (!finished.ok()) return Result<wire::RegisterAttemptResponse>::failure(finished);
  return decoded;
}

Result<Decision> Client::evaluate(const TrafficRequest& request) {
  wire::EvaluateTrafficMessage body;
  body.request = request;
  Writer writer;
  body.encode(writer);
  if (!writer.ok()) {
    return Result<Decision>::failure(writer.fail_code(), "traffic request exceeds the frame ceiling");
  }
  auto response = exchange(wire::MessageType::EvaluateTraffic,
                           ByteSpan(writer.buffer().data(), writer.size()));
  if (!response.ok()) return Result<Decision>::failure(response.status());
  const std::vector<std::uint8_t>& payload = response.value();
  Reader reader(ByteSpan(payload.data(), payload.size()));
  auto decoded = decode_decision(reader);
  if (!decoded.ok()) return decoded;
  const Status finished = reader.finish();
  if (!finished.ok()) return Result<Decision>::failure(finished);
  return decoded;
}

Result<wire::OpenFlowResponse> Client::open_flow(const Decision& decision,
                                                      std::uint64_t declared_bytes) {
  wire::OpenFlowMessage body;
  body.decision = decision;
  body.declared_bytes = declared_bytes;
  Writer writer;
  body.encode(writer);
  if (!writer.ok()) {
    return Result<wire::OpenFlowResponse>::failure(writer.fail_code(),
                                                   "open flow request exceeds the frame ceiling");
  }
  auto response =
      exchange(wire::MessageType::OpenFlow, ByteSpan(writer.buffer().data(), writer.size()));
  if (!response.ok()) return Result<wire::OpenFlowResponse>::failure(response.status());
  const std::vector<std::uint8_t>& payload = response.value();
  Reader reader(ByteSpan(payload.data(), payload.size()));
  auto decoded = wire::OpenFlowResponse::decode(reader);
  if (!decoded.ok()) return decoded;
  const Status finished = reader.finish();
  if (!finished.ok()) return Result<wire::OpenFlowResponse>::failure(finished);
  return decoded;
}

Result<wire::RegisterTransferResponse> Client::register_transfer(
    const TransferRegistration& registration) {
  wire::RegisterTransferMessage body;
  body.registration = registration;
  Writer writer;
  body.encode(writer);
  if (!writer.ok()) {
    return Result<wire::RegisterTransferResponse>::failure(
        writer.fail_code(), "transfer registration exceeds the frame ceiling");
  }
  auto response = exchange(wire::MessageType::RegisterTransfer,
                           ByteSpan(writer.buffer().data(), writer.size()));
  if (!response.ok()) return Result<wire::RegisterTransferResponse>::failure(response.status());
  const std::vector<std::uint8_t>& payload = response.value();
  Reader reader(ByteSpan(payload.data(), payload.size()));
  auto decoded = wire::RegisterTransferResponse::decode(reader);
  if (!decoded.ok()) return decoded;
  const Status finished = reader.finish();
  if (!finished.ok()) return Result<wire::RegisterTransferResponse>::failure(finished);
  return decoded;
}

Result<wire::TransferDecisionResponse> Client::authorize_transfer(const TrafficRequest& request) {
  wire::AuthorizeTransferMessage body;
  body.request = request;
  Writer writer;
  body.encode(writer);
  auto response = exchange(wire::MessageType::AuthorizeTransfer,
                           ByteSpan(writer.buffer().data(), writer.size()));
  if (!response.ok()) return Result<wire::TransferDecisionResponse>::failure(response.status());
  const std::vector<std::uint8_t>& payload = response.value();
  Reader reader(ByteSpan(payload.data(), payload.size()));
  auto decoded = wire::TransferDecisionResponse::decode(reader);
  if (!decoded.ok()) return decoded;
  const Status finished = reader.finish();
  if (!finished.ok()) return Result<wire::TransferDecisionResponse>::failure(finished);
  return decoded;
}

Result<PublishOutcome> Client::publish_completion(const CompletionPublication& publication) {
  wire::CompletionPublishMessage body;
  body.publication = publication;
  Writer writer;
  body.encode(writer);
  auto response = exchange(wire::MessageType::CompletionPublish,
                           ByteSpan(writer.buffer().data(), writer.size()));
  if (!response.ok()) return Result<PublishOutcome>::failure(response.status());
  const std::vector<std::uint8_t>& payload = response.value();
  Reader reader(ByteSpan(payload.data(), payload.size()));
  auto decoded = decode_publish_outcome(reader);
  if (!decoded.ok()) return decoded;
  const Status finished = reader.finish();
  if (!finished.ok()) return Result<PublishOutcome>::failure(finished);
  return decoded;
}

Result<AccountingSnapshot> Client::accounting() {
  wire::AccountingQueryMessage body;
  Writer writer;
  body.encode(writer);
  auto response = exchange(wire::MessageType::AccountingQuery,
                           ByteSpan(writer.buffer().data(), writer.size()));
  if (!response.ok()) return Result<AccountingSnapshot>::failure(response.status());
  const std::vector<std::uint8_t>& payload = response.value();
  Reader reader(ByteSpan(payload.data(), payload.size()));
  auto decoded = decode_accounting_snapshot(reader);
  if (!decoded.ok()) return decoded;
  const Status finished = reader.finish();
  if (!finished.ok()) return Result<AccountingSnapshot>::failure(finished);
  return decoded;
}

Result<CoordinatorStatus> Client::status() {
  wire::StatusQueryMessage body;
  Writer writer;
  body.encode(writer);
  auto response =
      exchange(wire::MessageType::StatusQuery, ByteSpan(writer.buffer().data(), writer.size()));
  if (!response.ok()) return Result<CoordinatorStatus>::failure(response.status());
  const std::vector<std::uint8_t>& payload = response.value();
  Reader reader(ByteSpan(payload.data(), payload.size()));
  auto decoded = decode_coordinator_status(reader);
  if (!decoded.ok()) return decoded;
  const Status finished = reader.finish();
  if (!finished.ok()) return Result<CoordinatorStatus>::failure(finished);
  return decoded;
}

Result<PolicyConfig> Client::policy() {
  wire::PolicyQueryMessage body;
  Writer writer;
  body.encode(writer);
  auto response =
      exchange(wire::MessageType::PolicyQuery, ByteSpan(writer.buffer().data(), writer.size()));
  if (!response.ok()) return Result<PolicyConfig>::failure(response.status());
  const std::vector<std::uint8_t>& payload = response.value();
  Reader reader(ByteSpan(payload.data(), payload.size()));
  auto decoded = PolicyConfig::decode(reader);
  if (!decoded.ok()) return decoded;
  const Status finished = reader.finish();
  if (!finished.ok()) return Result<PolicyConfig>::failure(finished);
  return decoded;
}

Result<wire::TopologyResponseMessage> Client::publish_topology(const TopologyEvidence& evidence) {
  wire::TopologyPublishMessage body;
  body.evidence = evidence;
  Writer writer;
  body.encode(writer);
  auto response = exchange(wire::MessageType::TopologyPublish,
                           ByteSpan(writer.buffer().data(), writer.size()));
  if (!response.ok()) return Result<wire::TopologyResponseMessage>::failure(response.status());
  const std::vector<std::uint8_t>& payload = response.value();
  Reader reader(ByteSpan(payload.data(), payload.size()));
  auto decoded = wire::TopologyResponseMessage::decode(reader);
  if (!decoded.ok()) return decoded;
  const Status finished = reader.finish();
  if (!finished.ok()) return Result<wire::TopologyResponseMessage>::failure(finished);
  return decoded;
}

Result<wire::SloContractResponse> Client::publish_slo_contract(
    RequestId request, std::uint64_t tail_latency_budget_nanos,
    std::uint64_t registration_deadline_nanos, std::uint32_t min_stream_share_per_mille) {
  wire::PublishSloContractMessage body;
  body.request = request;
  body.tail_latency_budget_nanos = tail_latency_budget_nanos;
  body.registration_deadline_nanos = registration_deadline_nanos;
  body.min_stream_share_per_mille = min_stream_share_per_mille;
  Writer writer;
  body.encode(writer);
  auto response = exchange(wire::MessageType::PublishSloContract,
                           ByteSpan(writer.buffer().data(), writer.size()));
  if (!response.ok()) return Result<wire::SloContractResponse>::failure(response.status());
  const std::vector<std::uint8_t>& payload = response.value();
  Reader reader(ByteSpan(payload.data(), payload.size()));
  auto decoded = wire::SloContractResponse::decode(reader);
  if (!decoded.ok()) return decoded;
  const Status finished = reader.finish();
  if (!finished.ok()) return Result<wire::SloContractResponse>::failure(finished);
  return decoded;
}

Result<wire::RouteDecisionResponse> Client::issue_route_decision(RequestId request,
                                                                AttemptId attempt,
                                                                std::uint32_t target_node,
                                                                bool lateral,
                                                                bool disaggregated_handoff) {
  wire::IssueRouteDecisionMessage body;
  body.request = request;
  body.attempt = attempt;
  body.target_node = target_node;
  body.lateral = lateral;
  body.disaggregated_handoff = disaggregated_handoff;
  Writer writer;
  body.encode(writer);
  auto response = exchange(wire::MessageType::IssueRouteDecision,
                           ByteSpan(writer.buffer().data(), writer.size()));
  if (!response.ok()) return Result<wire::RouteDecisionResponse>::failure(response.status());
  const std::vector<std::uint8_t>& payload = response.value();
  Reader reader(ByteSpan(payload.data(), payload.size()));
  auto decoded = wire::RouteDecisionResponse::decode(reader);
  if (!decoded.ok()) return decoded;
  const Status finished = reader.finish();
  if (!finished.ok()) return Result<wire::RouteDecisionResponse>::failure(finished);
  return decoded;
}

Result<wire::ModelGenerationResponse> Client::advance_model_generation(
    std::uint64_t model_hash, std::optional<ModelGeneration> expected, bool revalidate_only) {
  wire::AdvanceGenerationMessage body;
  body.target_hash = model_hash;
  body.has_expected = expected.has_value();
  body.expected_generation = expected.has_value() ? expected->value() : 0;
  body.revalidate_only = revalidate_only;
  body.state_target = false;
  Writer writer;
  body.encode(writer);
  auto response = exchange(wire::MessageType::AdvanceModelGeneration,
                           ByteSpan(writer.buffer().data(), writer.size()));
  if (!response.ok()) return Result<wire::ModelGenerationResponse>::failure(response.status());
  const std::vector<std::uint8_t>& payload = response.value();
  Reader reader(ByteSpan(payload.data(), payload.size()));
  auto decoded = wire::ModelGenerationResponse::decode(reader);
  if (!decoded.ok()) return decoded;
  const Status finished = reader.finish();
  if (!finished.ok()) return Result<wire::ModelGenerationResponse>::failure(finished);
  return decoded;
}

Result<wire::ModelGenerationResponse> Client::advance_state_generation(
    std::uint64_t state_hash, std::optional<StateGeneration> expected, bool revalidate_only) {
  wire::AdvanceGenerationMessage body;
  body.target_hash = state_hash;
  body.has_expected = expected.has_value();
  body.expected_generation = expected.has_value() ? expected->value() : 0;
  body.revalidate_only = revalidate_only;
  body.state_target = true;
  Writer writer;
  body.encode(writer);
  auto response = exchange(wire::MessageType::AdvanceModelGeneration,
                           ByteSpan(writer.buffer().data(), writer.buffer().size()));
  if (!response.ok()) return Result<wire::ModelGenerationResponse>::failure(response.status());
  const std::vector<std::uint8_t>& payload = response.value();
  Reader reader(ByteSpan(payload.data(), payload.size()));
  auto decoded = wire::ModelGenerationResponse::decode(reader);
  if (!decoded.ok()) return decoded;
  const Status finished = reader.finish();
  if (!finished.ok()) return Result<wire::ModelGenerationResponse>::failure(finished);
  return decoded;
}

}  // namespace itf::net
