// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "itf/wire.hpp"

#include <cstring>

#include "itf/crc32c.hpp"

namespace itf::wire {
namespace {

void store_u16(std::uint8_t* out, std::uint16_t value) noexcept {
  out[0] = static_cast<std::uint8_t>(value & 0xFFU);
  out[1] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
}

void store_u32(std::uint8_t* out, std::uint32_t value) noexcept {
  for (int index = 0; index < 4; ++index) {
    out[index] = static_cast<std::uint8_t>((value >> static_cast<unsigned>(index * 8)) & 0xFFU);
  }
}

void store_u64(std::uint8_t* out, std::uint64_t value) noexcept {
  for (int index = 0; index < 8; ++index) {
    out[index] = static_cast<std::uint8_t>((value >> static_cast<unsigned>(index * 8)) & 0xFFU);
  }
}

std::uint16_t load_u16(const std::uint8_t* in) noexcept {
  return static_cast<std::uint16_t>(
      static_cast<std::uint16_t>(in[0]) |
      static_cast<std::uint16_t>(static_cast<std::uint16_t>(in[1]) << 8U));
}

std::uint32_t load_u32(const std::uint8_t* in) noexcept {
  std::uint32_t value = 0;
  for (int index = 0; index < 4; ++index) {
    value |= static_cast<std::uint32_t>(in[index]) << static_cast<unsigned>(index * 8);
  }
  return value;
}

std::uint64_t load_u64(const std::uint8_t* in) noexcept {
  std::uint64_t value = 0;
  for (int index = 0; index < 8; ++index) {
    value |= static_cast<std::uint64_t>(in[index]) << static_cast<unsigned>(index * 8);
  }
  return value;
}

}  // namespace

std::string_view to_string(MessageType value) noexcept {
  switch (value) {
    case MessageType::Invalid: return "INVALID";
    case MessageType::HelloRequest: return "HELLO_REQUEST";
    case MessageType::HelloResponse: return "HELLO_RESPONSE";
    case MessageType::ErrorResponse: return "ERROR_RESPONSE";
    case MessageType::AckResponse: return "ACK_RESPONSE";
    case MessageType::RegisterRequestResponse: return "REGISTER_REQUEST_RESPONSE";
    case MessageType::RegisterAttemptResponse: return "REGISTER_ATTEMPT_RESPONSE";
    case MessageType::RegisterTransferResponse: return "REGISTER_TRANSFER_RESPONSE";
    case MessageType::RegisterRequest: return "REGISTER_REQUEST";
    case MessageType::RegisterAttempt: return "REGISTER_ATTEMPT";
    case MessageType::StageUpdate: return "STAGE_UPDATE";
    case MessageType::CancelRequest: return "CANCEL_REQUEST";
    case MessageType::FailAttempt: return "FAIL_ATTEMPT";
    case MessageType::ReplaceAttempt: return "REPLACE_ATTEMPT";
    case MessageType::EvaluateTraffic: return "EVALUATE_TRAFFIC";
    case MessageType::DecisionResponse: return "DECISION_RESPONSE";
    case MessageType::OpenFlow: return "OPEN_FLOW";
    case MessageType::OpenFlowResponse: return "OPEN_FLOW_RESPONSE";
    case MessageType::RegisterTransfer: return "REGISTER_TRANSFER";
    case MessageType::AuthorizeTransfer: return "AUTHORIZE_TRANSFER";
    case MessageType::TransferDecisionResponse: return "TRANSFER_DECISION_RESPONSE";
    case MessageType::TransferComplete: return "TRANSFER_COMPLETE";
    case MessageType::CompletionPublish: return "COMPLETION_PUBLISH";
    case MessageType::CompletionResponse: return "COMPLETION_RESPONSE";
    case MessageType::AccountingQuery: return "ACCOUNTING_QUERY";
    case MessageType::AccountingResponse: return "ACCOUNTING_RESPONSE";
    case MessageType::StatusQuery: return "STATUS_QUERY";
    case MessageType::StatusResponse: return "STATUS_RESPONSE";
    case MessageType::PolicyQuery: return "POLICY_QUERY";
    case MessageType::PolicyResponse: return "POLICY_RESPONSE";
    case MessageType::TopologyPublish: return "TOPOLOGY_PUBLISH";
    case MessageType::TopologyResponse: return "TOPOLOGY_RESPONSE";
    case MessageType::PublishSloContract: return "PUBLISH_SLO_CONTRACT";
    case MessageType::SloContractResponse: return "SLO_CONTRACT_RESPONSE";
    case MessageType::IssueRouteDecision: return "ISSUE_ROUTE_DECISION";
    case MessageType::RouteDecisionResponse: return "ROUTE_DECISION_RESPONSE";
    case MessageType::AdvanceModelGeneration: return "ADVANCE_MODEL_GENERATION";
    case MessageType::ModelGenerationResponse: return "MODEL_GENERATION_RESPONSE";
    case MessageType::Shutdown: return "SHUTDOWN";
    case MessageType::ShutdownAck: return "SHUTDOWN_ACK";
    case MessageType::Heartbeat: return "HEARTBEAT";
    case MessageType::HeartbeatAck: return "HEARTBEAT_ACK";
    case MessageType::DataChunk: return "DATA_CHUNK";
    case MessageType::DataChunkAck: return "DATA_CHUNK_ACK";
    case MessageType::DataComplete: return "DATA_COMPLETE";
    case MessageType::DataCompleteAck: return "DATA_COMPLETE_ACK";
    case MessageType::Count: return "COUNT";
  }
  return "INVALID";
}

bool is_known_message_type(MessageType value) noexcept {
  return value != MessageType::Invalid && value != MessageType::Count &&
         static_cast<std::uint16_t>(value) < static_cast<std::uint16_t>(MessageType::Count);
}

MessageType response_type_for(MessageType request) noexcept {
  switch (request) {
    case MessageType::HelloRequest: return MessageType::HelloResponse;
    case MessageType::RegisterRequest: return MessageType::RegisterRequestResponse;
    case MessageType::RegisterAttempt: return MessageType::RegisterAttemptResponse;
    case MessageType::StageUpdate:
    case MessageType::CancelRequest:
    case MessageType::FailAttempt:
    case MessageType::TransferComplete:
      return MessageType::AckResponse;
    case MessageType::ReplaceAttempt: return MessageType::RegisterAttemptResponse;
    case MessageType::EvaluateTraffic: return MessageType::DecisionResponse;
    case MessageType::OpenFlow: return MessageType::OpenFlowResponse;
    case MessageType::RegisterTransfer: return MessageType::RegisterTransferResponse;
    case MessageType::AuthorizeTransfer: return MessageType::TransferDecisionResponse;
    case MessageType::CompletionPublish: return MessageType::CompletionResponse;
    case MessageType::AccountingQuery: return MessageType::AccountingResponse;
    case MessageType::StatusQuery: return MessageType::StatusResponse;
    case MessageType::PolicyQuery: return MessageType::PolicyResponse;
    case MessageType::TopologyPublish: return MessageType::TopologyResponse;
    case MessageType::PublishSloContract: return MessageType::SloContractResponse;
    case MessageType::IssueRouteDecision: return MessageType::RouteDecisionResponse;
    case MessageType::AdvanceModelGeneration: return MessageType::ModelGenerationResponse;
    case MessageType::Shutdown: return MessageType::ShutdownAck;
    case MessageType::Heartbeat: return MessageType::HeartbeatAck;
    case MessageType::DataChunk: return MessageType::DataChunkAck;
    case MessageType::DataComplete: return MessageType::DataCompleteAck;
    case MessageType::Invalid:
    case MessageType::HelloResponse:
    case MessageType::ErrorResponse:
    case MessageType::AckResponse:
    case MessageType::RegisterRequestResponse:
    case MessageType::RegisterAttemptResponse:
    case MessageType::DecisionResponse:
    case MessageType::OpenFlowResponse:
    case MessageType::RegisterTransferResponse:
    case MessageType::TransferDecisionResponse:
    case MessageType::CompletionResponse:
    case MessageType::AccountingResponse:
    case MessageType::StatusResponse:
    case MessageType::PolicyResponse:
    case MessageType::TopologyResponse:
    case MessageType::SloContractResponse:
    case MessageType::RouteDecisionResponse:
    case MessageType::ModelGenerationResponse:
    case MessageType::ShutdownAck:
    case MessageType::HeartbeatAck:
    case MessageType::DataChunkAck:
    case MessageType::DataCompleteAck:
    case MessageType::Count:
      return MessageType::Invalid;
  }
  return MessageType::Invalid;
}

void encode_header(const FrameHeader& header, MutableByteSpan out) noexcept {
  if (out.size() < kHeaderBytes) return;
  std::uint8_t* cursor = out.data();
  std::memcpy(cursor, kMagic, sizeof(kMagic));
  store_u16(cursor + 4, header.protocol_version);
  store_u16(cursor + 6, static_cast<std::uint16_t>(header.type));
  store_u32(cursor + 8, header.flags);
  store_u32(cursor + 12, header.payload_length);
  store_u64(cursor + 16, header.sequence);
  store_u64(cursor + 24, header.correlation);
  store_u32(cursor + 32, header.payload_crc32c);
  const std::uint32_t header_crc =
      Crc32c::compute(ByteSpan(cursor, kHeaderBytes - sizeof(std::uint32_t)));
  store_u32(cursor + 36, header_crc);
}

Result<FrameHeader> decode_header(ByteSpan header, std::uint32_t max_payload_bytes) noexcept {
  if (header.size() < kHeaderBytes) {
    return Result<FrameHeader>::failure(StatusCode::MalformedInput, "frame header is truncated");
  }
  const std::uint8_t* cursor = header.data();
  if (std::memcmp(cursor, kMagic, sizeof(kMagic)) != 0) {
    return Result<FrameHeader>::failure(StatusCode::MalformedInput, "frame magic does not match");
  }
  FrameHeader decoded;
  decoded.protocol_version = load_u16(cursor + 4);
  if (decoded.protocol_version != kWireProtocolVersion) {
    return Result<FrameHeader>::failure(StatusCode::UnsupportedVersion,
                                        "frame protocol version is not supported");
  }
  decoded.type = static_cast<MessageType>(load_u16(cursor + 6));
  if (!is_known_message_type(decoded.type)) {
    return Result<FrameHeader>::failure(StatusCode::MalformedInput, "unknown message type");
  }
  decoded.flags = load_u32(cursor + 8);
  decoded.payload_length = load_u32(cursor + 12);
  decoded.sequence = load_u64(cursor + 16);
  decoded.correlation = load_u64(cursor + 24);
  decoded.payload_crc32c = load_u32(cursor + 32);
  const std::uint32_t expected_header_crc = load_u32(cursor + 36);
  const std::uint32_t actual_header_crc =
      Crc32c::compute(ByteSpan(cursor, kHeaderBytes - sizeof(std::uint32_t)));
  if (expected_header_crc != actual_header_crc) {
    return Result<FrameHeader>::failure(StatusCode::IntegrityMismatch,
                                        "frame header checksum mismatch");
  }
  if (decoded.payload_length > kAbsoluteMaxPayloadBytes) {
    return Result<FrameHeader>::failure(StatusCode::BoundsExceeded,
                                        "frame payload exceeds the protocol ceiling");
  }
  if (decoded.payload_length > max_payload_bytes) {
    return Result<FrameHeader>::failure(StatusCode::BoundsExceeded,
                                        "frame payload exceeds the negotiated limit");
  }
  if (decoded.flags != 0) {
    return Result<FrameHeader>::failure(StatusCode::MalformedInput,
                                        "frame flags are not defined in this protocol version");
  }
  return Result<FrameHeader>::success(decoded);
}

}  // namespace itf::wire
