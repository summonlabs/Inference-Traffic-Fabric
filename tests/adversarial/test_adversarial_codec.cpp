// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <array>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "itf/crc32c.hpp"
#include "itf/messages.hpp"
#include "itf/serialize.hpp"
#include "itf/wire.hpp"
#include "support/test.hpp"
#include "support/test_support.hpp"

namespace {

using namespace itf;

std::vector<std::uint8_t> make_valid_header(wire::MessageType type = wire::MessageType::Heartbeat,
                                            std::uint32_t payload_length = 0) {
  std::vector<std::uint8_t> header(wire::kHeaderBytes, 0);
  wire::FrameHeader frame;
  frame.type = type;
  frame.payload_length = payload_length;
  frame.sequence = 1;
  wire::encode_header(frame, MutableByteSpan(header.data(), header.size()));
  return header;
}

ITF_TEST(frame_header_validation_is_exhaustive) {
  const std::uint32_t limit = 4096;
  {
    auto header = make_valid_header();
    const auto decoded = wire::decode_header(ByteSpan(header.data(), header.size()), limit);
    ITF_REQUIRE(decoded.ok());
    ITF_CHECK_EQ(static_cast<int>(decoded.value().type),
                 static_cast<int>(wire::MessageType::Heartbeat));
  }
  {
    auto header = make_valid_header();
    header[0] = 'X';
    ITF_CHECK_EQ(static_cast<int>(wire::decode_header(ByteSpan(header.data(), header.size()), limit)
                                      .code()),
                 static_cast<int>(StatusCode::MalformedInput));
  }
  {
    auto header = make_valid_header();
    header[4] = 0x7F;
    header[5] = 0x00;
    ITF_CHECK_EQ(static_cast<int>(wire::decode_header(ByteSpan(header.data(), header.size()), limit)
                                      .code()),
                 static_cast<int>(StatusCode::UnsupportedVersion));
  }
  {
    auto header = make_valid_header();
    header[6] = 0xFF;
    header[7] = 0x00;
    ITF_CHECK_EQ(static_cast<int>(wire::decode_header(ByteSpan(header.data(), header.size()), limit)
                                      .code()),
                 static_cast<int>(StatusCode::MalformedInput));
  }
  {
    // An undefined flag with an intact header checksum is a protocol error.
    std::vector<std::uint8_t> header(wire::kHeaderBytes, 0);
    wire::FrameHeader frame;
    frame.type = wire::MessageType::Heartbeat;
    frame.flags = 0x01;
    wire::encode_header(frame, MutableByteSpan(header.data(), header.size()));
    ITF_CHECK_EQ(static_cast<int>(wire::decode_header(ByteSpan(header.data(), header.size()), limit)
                                      .code()),
                 static_cast<int>(StatusCode::MalformedInput));
  }
  {
    // A corrupted flags byte without a matching checksum is an integrity error.
    auto header = make_valid_header();
    header[8] = 0x01;
    ITF_CHECK_EQ(static_cast<int>(wire::decode_header(ByteSpan(header.data(), header.size()), limit)
                                      .code()),
                 static_cast<int>(StatusCode::IntegrityMismatch));
  }
  {
    auto header = make_valid_header();
    header[36] ^= 0x01;  // header checksum corruption
    ITF_CHECK_EQ(static_cast<int>(wire::decode_header(ByteSpan(header.data(), header.size()), limit)
                                      .code()),
                 static_cast<int>(StatusCode::IntegrityMismatch));
  }
  {
    auto header = make_valid_header(wire::MessageType::Heartbeat, 0xFFFFFF);
    ITF_CHECK_EQ(static_cast<int>(wire::decode_header(ByteSpan(header.data(), header.size()), limit)
                                      .code()),
                 static_cast<int>(StatusCode::BoundsExceeded));
  }
  {
    auto header = make_valid_header(wire::MessageType::Heartbeat, 4097);
    ITF_CHECK_EQ(static_cast<int>(wire::decode_header(ByteSpan(header.data(), header.size()), limit)
                                      .code()),
                 static_cast<int>(StatusCode::BoundsExceeded));
  }
  {
    const std::array<std::uint8_t, 8> truncated{};
    ITF_CHECK_EQ(
        static_cast<int>(wire::decode_header(ByteSpan(truncated.data(), truncated.size()), limit)
                             .code()),
        static_cast<int>(StatusCode::MalformedInput));
  }
}

ITF_TEST(message_bodies_reject_trailing_and_truncated_bodies) {
  wire::HeartbeatMessage heartbeat;
  heartbeat.client_time_nanos = 42;
  Writer writer;
  heartbeat.encode(writer);
  ITF_REQUIRE(writer.ok());

  {
    std::vector<std::uint8_t> bytes = writer.buffer();
    bytes.push_back(0xAB);
    Reader reader(ByteSpan(bytes.data(), bytes.size()));
    const auto decoded = wire::HeartbeatMessage::decode(reader);
    ITF_REQUIRE(decoded.ok());
    ITF_CHECK_EQ(static_cast<int>(reader.finish().code()),
                 static_cast<int>(StatusCode::TrailingGarbage));
  }
  {
    std::vector<std::uint8_t> bytes = writer.buffer();
    bytes.resize(bytes.size() - 3);
    Reader reader(ByteSpan(bytes.data(), bytes.size()));
    const auto decoded = wire::HeartbeatMessage::decode(reader);
    ITF_CHECK(!decoded.ok());
  }
}

ITF_TEST(hello_requests_are_strictly_validated) {
  wire::HelloRequest hello;
  hello.peer_name = "peer";
  hello.auth_token = "secret";
  hello.client_version = "1.0.0";
  hello.client_boot = BootId(1, 2);
  Writer writer;
  hello.encode(writer);
  ITF_REQUIRE(writer.ok());
  {
    Reader reader(ByteSpan(writer.buffer().data(), writer.buffer().size()));
    ITF_CHECK(wire::HelloRequest::decode(reader).ok());
  }
  {
    wire::HelloRequest hostile = hello;
    hostile.peer_name = "bad name";
    Writer hostile_writer;
    hostile.encode(hostile_writer);
    Reader reader(ByteSpan(hostile_writer.buffer().data(), hostile_writer.buffer().size()));
    ITF_CHECK_EQ(static_cast<int>(wire::HelloRequest::decode(reader).code()),
                 static_cast<int>(StatusCode::MalformedInput));
  }
  {
    wire::HelloRequest hostile = hello;
    hostile.protocol_version = 99;
    Writer hostile_writer;
    hostile.encode(hostile_writer);
    Reader reader(ByteSpan(hostile_writer.buffer().data(), hostile_writer.buffer().size()));
    ITF_CHECK_EQ(static_cast<int>(wire::HelloRequest::decode(reader).code()),
                 static_cast<int>(StatusCode::UnsupportedVersion));
  }
  {
    wire::HelloRequest hostile = hello;
    hostile.max_frame_payload = 0xFFFFFFFFU;
    Writer hostile_writer;
    hostile.encode(hostile_writer);
    Reader reader(ByteSpan(hostile_writer.buffer().data(), hostile_writer.buffer().size()));
    ITF_CHECK_EQ(static_cast<int>(wire::HelloRequest::decode(reader).code()),
                 static_cast<int>(StatusCode::BoundsExceeded));
  }
  {
    // An invalid UTF-8 peer name must be refused even when it is a valid label
    // byte pattern.
    wire::HelloRequest hostile = hello;
    hostile.client_version = std::string("v\xC3");
    Writer hostile_writer;
    hostile.encode(hostile_writer);
    ITF_CHECK(!hostile_writer.ok());
  }
}

ITF_TEST(data_chunk_bounds_are_enforced) {
  wire::DataChunkMessage chunk;
  chunk.transfer = StateTransferId(1, 2);
  chunk.chunk_index = 0;
  chunk.total_chunks = 1;
  chunk.total_bytes = 16;
  const std::vector<std::uint8_t> payload(16, 0x5A);
  chunk.data = ByteSpan(payload.data(), payload.size());
  Writer writer;
  chunk.encode(writer);
  ITF_REQUIRE(writer.ok());
  {
    Reader reader(ByteSpan(writer.buffer().data(), writer.buffer().size()));
    const auto decoded = wire::DataChunkMessage::decode(reader);
    ITF_REQUIRE(decoded.ok());
    ITF_CHECK_EQ(decoded.value().data.size(), static_cast<std::size_t>(16));
  }
  {
    wire::DataChunkMessage hostile = chunk;
    hostile.chunk_index = 1;
    hostile.total_chunks = 1;
    Writer hostile_writer;
    hostile.encode(hostile_writer);
    Reader reader(ByteSpan(hostile_writer.buffer().data(), hostile_writer.buffer().size()));
    ITF_CHECK_EQ(static_cast<int>(wire::DataChunkMessage::decode(reader).code()),
                 static_cast<int>(StatusCode::MalformedInput));
  }
  {
    // An empty chunk is not a legal transfer unit.
    wire::DataChunkMessage hostile = chunk;
    hostile.data = ByteSpan();
    Writer hostile_writer;
    hostile.encode(hostile_writer);
    Reader reader(ByteSpan(hostile_writer.buffer().data(), hostile_writer.buffer().size()));
    ITF_CHECK_EQ(static_cast<int>(wire::DataChunkMessage::decode(reader).code()),
                 static_cast<int>(StatusCode::MalformedInput));
  }
}

ITF_TEST(random_byte_soup_never_decodes_successfully) {
  std::mt19937_64 random(0xF0F0F0F0ULL);
  int decoded_count = 0;
  for (int round = 0; round < 4000; ++round) {
    const std::size_t length = static_cast<std::size_t>(random() % 512);
    std::vector<std::uint8_t> bytes(length);
    for (std::uint8_t& byte : bytes) byte = static_cast<std::uint8_t>(random() & 0xFFU);
    const auto header = wire::decode_header(ByteSpan(bytes.data(), bytes.size()), 4096);
    if (header.ok()) ++decoded_count;
    Reader reader(ByteSpan(bytes.data(), bytes.size()), 4096);
    (void)decode_decision(reader);
    (void)decode_traffic_request(reader);
    (void)decode_accounting_snapshot(reader);
    (void)decode_topology_evidence(reader);
  }
  // Random bytes cannot satisfy magic, version, type, length and both CRCs.
  ITF_CHECK_EQ(decoded_count, 0);
}

ITF_TEST(every_single_bit_flip_in_a_frame_is_detected) {
  wire::EvaluateTrafficMessage message;
  TrafficRequest request;
  request.declared_class = TrafficClass::ModelTransfer;
  request.declared_stage = ServingStage::Admitted;
  request.direction = FlowDirection::Lateral;
  request.payload_bytes = 123456;
  message.request = request;
  Writer writer;
  message.encode(writer);
  ITF_REQUIRE(writer.ok());
  const std::vector<std::uint8_t> payload = writer.buffer();

  // The frame checksum covers the whole payload, so no single-bit corruption
  // can survive transmission undetected.
  int undetected = 0;
  for (std::size_t index = 0; index < payload.size(); ++index) {
    for (int bit = 0; bit < 8; ++bit) {
      std::vector<std::uint8_t> mutated = payload;
      mutated[index] = static_cast<std::uint8_t>(mutated[index] ^ (1U << bit));
      if (Crc32c::compute(ByteSpan(mutated.data(), mutated.size())) ==
          Crc32c::compute(ByteSpan(payload.data(), payload.size()))) {
        ++undetected;
      }
    }
  }
  ITF_CHECK_EQ(undetected, 0);
}

ITF_TEST(structural_bit_flips_are_refused_by_the_decoder) {
  // The bytes that carry semantic meaning are rejected when they become
  // impossible values rather than being silently reinterpreted.
  const std::vector<std::pair<TrafficClass, ServingStage>> illegal = {
      {TrafficClass::DecodeStream, ServingStage::Admitted},
      {TrafficClass::ModelTransfer, ServingStage::PrefillRunning},
      {TrafficClass::Unknown, ServingStage::Admitted},
  };
  for (const auto& [traffic, stage] : illegal) {
    TrafficRequest request;
    request.declared_class = traffic;
    request.declared_stage = stage;
    request.direction = FlowDirection::Lateral;
    TrafficSubject subject;
    subject.kind = SubjectKind::Serving;
    subject.request = itf::test::make_request_id(201, 1);
    subject.attempt = itf::test::make_attempt_id(201, 1);
    subject.request_generation = RequestGeneration::from(1);
    request.subject = subject;
    wire::EvaluateTrafficMessage message;
    message.request = request;
    Writer writer;
    message.encode(writer);
    ITF_REQUIRE(writer.ok());
    Reader reader(ByteSpan(writer.buffer().data(), writer.buffer().size()));
    const auto decoded = wire::EvaluateTrafficMessage::decode(reader);
    // The envelope is representable; the coordinator is what refuses an
    // impossible serving stage and class combination.
    ITF_CHECK(decoded.ok());
    if (decoded.ok()) {
      ITF_CHECK(!is_class_legal_for_stage(traffic, stage));
    }
  }
  std::vector<std::uint8_t> hostile = {0xFF, 0xFF};
  Reader reader(ByteSpan(hostile.data(), hostile.size()));
  (void)reader.enumeration(TrafficClass::Count);
  ITF_CHECK_EQ(static_cast<int>(reader.fail_code()), static_cast<int>(StatusCode::MalformedInput));
}

}  // namespace

int main(int argc, char** argv) { return itf::test::run_all("test_adversarial_codec", argc, argv); }
