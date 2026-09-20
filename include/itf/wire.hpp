// Inference Traffic Fabric - framed, versioned, integrity-checked transport.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef ITF_WIRE_HPP
#define ITF_WIRE_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "itf/bytes.hpp"
#include "itf/codec.hpp"
#include "itf/error.hpp"
#include "itf/version.hpp"

namespace itf::wire {

/// Frame magic: ASCII "ITF1" in transmission order.
inline constexpr std::uint8_t kMagic[4] = {'I', 'T', 'F', '1'};

/// Fixed frame header layout, little-endian:
///   0  magic[4]
///   4  protocol_version u16
///   6  message_type u16
///   8  flags u32
///  12  payload_length u32
///  16  sequence u64
///  24  correlation u64
///  32  payload_crc32c u32
///  36  header_crc32c u32   (covers bytes 0..35)
///  40  payload
inline constexpr std::size_t kHeaderBytes = 40;

/// Absolute ceiling on a single frame payload, independent of negotiation.
inline constexpr std::uint32_t kAbsoluteMaxPayloadBytes = 4U * 1024U * 1024U;

/// Chunk payload ceiling for the data plane.
inline constexpr std::uint32_t kMaxChunkBytes = 256U * 1024U;

enum class MessageType : std::uint16_t {
  Invalid = 0,
  HelloRequest = 1,
  HelloResponse = 2,
  ErrorResponse = 3,
  /// Generic acknowledgement carrying a SimpleAck body.
  AckResponse = 4,
  RegisterRequestResponse = 5,
  RegisterAttemptResponse = 6,
  RegisterTransferResponse = 7,
  RegisterRequest = 10,
  RegisterAttempt = 11,
  StageUpdate = 12,
  CancelRequest = 13,
  FailAttempt = 14,
  ReplaceAttempt = 15,
  EvaluateTraffic = 20,
  DecisionResponse = 21,
  OpenFlow = 22,
  OpenFlowResponse = 23,
  RegisterTransfer = 30,
  AuthorizeTransfer = 31,
  TransferDecisionResponse = 32,
  TransferComplete = 33,
  CompletionPublish = 40,
  CompletionResponse = 41,
  AccountingQuery = 50,
  AccountingResponse = 51,
  StatusQuery = 52,
  StatusResponse = 53,
  PolicyQuery = 54,
  PolicyResponse = 55,
  TopologyPublish = 60,
  TopologyResponse = 61,
  PublishSloContract = 62,
  SloContractResponse = 63,
  IssueRouteDecision = 64,
  RouteDecisionResponse = 65,
  AdvanceModelGeneration = 66,
  ModelGenerationResponse = 67,
  Shutdown = 70,
  ShutdownAck = 71,
  Heartbeat = 80,
  HeartbeatAck = 81,
  DataChunk = 90,
  DataChunkAck = 91,
  DataComplete = 92,
  DataCompleteAck = 93,
  Count = 94,
};

[[nodiscard]] std::string_view to_string(MessageType value) noexcept;
[[nodiscard]] bool is_known_message_type(MessageType value) noexcept;

/// The single response type a request type is allowed to produce. Responses
/// are never inferred from enum adjacency.
[[nodiscard]] MessageType response_type_for(MessageType request) noexcept;

struct FrameHeader {
  std::uint16_t protocol_version = kWireProtocolVersion;
  MessageType type = MessageType::Invalid;
  std::uint32_t flags = 0;
  std::uint32_t payload_length = 0;
  std::uint64_t sequence = 0;
  std::uint64_t correlation = 0;
  std::uint32_t payload_crc32c = 0;
};

/// Encodes the fixed header. Callers must have already bounded
/// payload_length by the negotiated frame limit.
void encode_header(const FrameHeader& header, MutableByteSpan out) noexcept;

/// Decodes and fully validates a fixed header: magic, protocol version,
/// known message type, negotiated payload bound and the header CRC. Every
/// failure is a distinct, deterministic code.
[[nodiscard]] Result<FrameHeader> decode_header(ByteSpan header,
                                                std::uint32_t max_payload_bytes) noexcept;

}  // namespace itf::wire

#endif  // ITF_WIRE_HPP
