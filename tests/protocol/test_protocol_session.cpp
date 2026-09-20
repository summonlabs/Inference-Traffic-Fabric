// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "itf/client.hpp"
#include "itf/crc32c.hpp"
#include "itf/session.hpp"
#include "support/test.hpp"
#include "support/test_support.hpp"

namespace {

using namespace itf;

const std::string kToken = "protocol-secret";

class AckHandler final : public net::MessageHandler {
 public:
  Result<std::vector<std::uint8_t>> handle(Context& context, wire::MessageType type,
                                           ByteSpan payload) override {
    (void)context;
    if (type != wire::MessageType::Heartbeat) {
      return Result<std::vector<std::uint8_t>>::failure(StatusCode::Unsupported,
                                                        "only heartbeats are handled here");
    }
    Reader reader(payload);
    auto body = wire::HeartbeatMessage::decode(reader);
    if (!body.ok()) return Result<std::vector<std::uint8_t>>::failure(body.status());
    const Status finished = reader.finish();
    if (!finished.ok()) return Result<std::vector<std::uint8_t>>::failure(finished);
    wire::SimpleAck ack;
    Writer writer;
    ack.encode(writer);
    return Result<std::vector<std::uint8_t>>::success(writer.take());
  }

  void describe_authority(wire::HelloResponse& response) override {
    response.epoch = CoordinatorEpoch::from(5);
    response.policy_generation = PolicyGeneration::from(4);
  }
};

struct RawSession {
  net::TcpStream stream;
  std::uint64_t sequence = 0;
  std::int64_t timeout_nanos = 3000000000LL;

  Status send(wire::MessageType type, ByteSpan payload) {
    sequence += 1;
    std::vector<std::uint8_t> header(wire::kHeaderBytes, 0);
    wire::FrameHeader frame;
    frame.type = type;
    frame.payload_length = static_cast<std::uint32_t>(payload.size());
    frame.sequence = sequence;
    frame.payload_crc32c = Crc32c::compute(payload);
    wire::encode_header(frame, MutableByteSpan(header.data(), header.size()));
    Status status = stream.write_all(ByteSpan(header.data(), header.size()));
    if (status.ok() && !payload.empty()) status = stream.write_all(payload);
    return status;
  }

  Result<std::vector<std::uint8_t>> receive(wire::MessageType& type_out,
                                            std::uint32_t max_payload = 65536) {
    std::vector<std::uint8_t> header(wire::kHeaderBytes, 0);
    const Status read = stream.read_exact(MutableByteSpan(header.data(), header.size()),
                                          timeout_nanos);
    if (!read.ok()) return Result<std::vector<std::uint8_t>>::failure(read);
    const auto decoded = wire::decode_header(ByteSpan(header.data(), header.size()), max_payload);
    if (!decoded.ok()) return Result<std::vector<std::uint8_t>>::failure(decoded.status());
    std::vector<std::uint8_t> payload(decoded.value().payload_length, 0);
    if (!payload.empty()) {
      const Status body = stream.read_exact(MutableByteSpan(payload.data(), payload.size()),
                                            timeout_nanos);
      if (!body.ok()) return Result<std::vector<std::uint8_t>>::failure(body);
    }
    type_out = decoded.value().type;
    return Result<std::vector<std::uint8_t>>::success(std::move(payload));
  }
};

std::unique_ptr<net::Server> start_server(AckHandler& handler, net::ServerConfig& config,
                                          std::uint16_t& port) {
  config.bind_host = "127.0.0.1";
  config.port = 0;
  config.auth_token = kToken;
  config.max_frame_payload = 65536;
  auto server = std::make_unique<net::Server>(config, &handler);
  if (!server->start().ok()) return nullptr;
  port = server->bound_port();
  return server;
}

ITF_TEST(handshake_rejects_protocol_and_secret_mismatches) {
  AckHandler handler;
  net::ServerConfig config;
  std::uint16_t port = 0;
  auto server = start_server(handler, config, port);
  ITF_REQUIRE(server != nullptr);

  {
    auto stream = net::TcpStream::connect("127.0.0.1", port, 2000000000LL);
    ITF_REQUIRE(stream.ok());
    RawSession session;
    session.stream = std::move(stream).value();
    wire::HelloRequest hello;
    hello.peer_name = "raw-peer";
    hello.auth_token = kToken;
    hello.client_boot = BootId(1, 2);
    hello.protocol_version = 99;
    Writer writer;
    hello.encode(writer);
    ITF_REQUIRE(session.send(wire::MessageType::HelloRequest,
                             ByteSpan(writer.buffer().data(), writer.buffer().size()))
                    .ok());
    wire::MessageType type = wire::MessageType::Invalid;
    const auto response = session.receive(type);
    ITF_REQUIRE(response.ok());
    ITF_CHECK_EQ(static_cast<int>(type), static_cast<int>(wire::MessageType::ErrorResponse));
    Reader reader(ByteSpan(response.value().data(), response.value().size()));
    const auto error = wire::ErrorResponse::decode(reader);
    ITF_REQUIRE(error.ok());
    ITF_CHECK_EQ(static_cast<int>(error.value().code),
                 static_cast<int>(StatusCode::UnsupportedVersion));
    session.stream.close();
  }
  {
    auto stream = net::TcpStream::connect("127.0.0.1", port, 2000000000LL);
    ITF_REQUIRE(stream.ok());
    RawSession session;
    session.stream = std::move(stream).value();
    wire::HelloRequest hello;
    hello.peer_name = "raw-peer";
    hello.auth_token = "not-the-secret";
    hello.client_boot = BootId(1, 2);
    Writer writer;
    hello.encode(writer);
    ITF_REQUIRE(session.send(wire::MessageType::HelloRequest,
                             ByteSpan(writer.buffer().data(), writer.buffer().size()))
                    .ok());
    wire::MessageType type = wire::MessageType::Invalid;
    const auto response = session.receive(type);
    ITF_REQUIRE(response.ok());
    Reader reader(ByteSpan(response.value().data(), response.value().size()));
    const auto error = wire::ErrorResponse::decode(reader);
    ITF_REQUIRE(error.ok());
    ITF_CHECK_EQ(static_cast<int>(error.value().code),
                 static_cast<int>(StatusCode::NotAuthorized));
    session.stream.close();
  }
  server->stop();
  ITF_CHECK(server->stats().rejected_auth >= 1ULL);
  ITF_CHECK(server->stats().rejected_protocol >= 1ULL);
}

ITF_TEST(a_message_before_the_handshake_is_refused) {
  AckHandler handler;
  net::ServerConfig config;
  std::uint16_t port = 0;
  auto server = start_server(handler, config, port);
  ITF_REQUIRE(server != nullptr);

  auto stream = net::TcpStream::connect("127.0.0.1", port, 2000000000LL);
  ITF_REQUIRE(stream.ok());
  RawSession session;
  session.stream = std::move(stream).value();
  wire::HeartbeatMessage heartbeat;
  Writer writer;
  heartbeat.encode(writer);
  ITF_REQUIRE(session
                  .send(wire::MessageType::Heartbeat,
                        ByteSpan(writer.buffer().data(), writer.buffer().size()))
                  .ok());
  wire::MessageType type = wire::MessageType::Invalid;
  const auto response = session.receive(type);
  ITF_REQUIRE(response.ok());
  Reader reader(ByteSpan(response.value().data(), response.value().size()));
  const auto error = wire::ErrorResponse::decode(reader);
  ITF_REQUIRE(error.ok());
  ITF_CHECK_EQ(static_cast<int>(error.value().code), static_cast<int>(StatusCode::NotAuthorized));
  session.stream.close();
  server->stop();
}

ITF_TEST(garbage_bytes_produce_a_deterministic_error_then_close) {
  AckHandler handler;
  net::ServerConfig config;
  std::uint16_t port = 0;
  auto server = start_server(handler, config, port);
  ITF_REQUIRE(server != nullptr);

  auto stream = net::TcpStream::connect("127.0.0.1", port, 2000000000LL);
  ITF_REQUIRE(stream.ok());
  const std::string garbage(64, '\x7F');
  ITF_REQUIRE_STATUS_OK(stream.value().write_all(as_bytes(garbage)));
  std::array<std::uint8_t, 64> buffer{};
  const Status read = stream.value().read_exact(MutableByteSpan(buffer.data(), buffer.size()),
                                                3000000000LL);
  if (read.ok()) {
    const auto decoded = wire::decode_header(ByteSpan(buffer.data(), buffer.size()), 65536);
    // The server may answer with one error frame before closing; either way the
    // session must not accept the bytes.
    ITF_CHECK(decoded.ok());
    if (decoded.ok()) {
      ITF_CHECK_EQ(static_cast<int>(decoded.value().type),
                   static_cast<int>(wire::MessageType::ErrorResponse));
    }
  }
  stream.value().close();
  ITF_CHECK(itf::test::wait_until([&] { return server->stats().active_sessions == 0U; }, 8000));
  ITF_CHECK(server->stats().frames_invalid >= 1ULL);
  ITF_CHECK(server->stats().bytes_in >= wire::kHeaderBytes);
  server->stop();
}

ITF_TEST(the_negotiated_frame_limit_is_enforced) {
  AckHandler handler;
  net::ServerConfig config;
  std::uint16_t port = 0;
  auto server = start_server(handler, config, port);
  ITF_REQUIRE(server != nullptr);

  net::ClientConfig client_config;
  client_config.port = port;
  client_config.peer_name = "small-frame-client";
  client_config.auth_token = kToken;
  client_config.max_frame_payload = 4096;
  auto client = net::Client::connect(client_config);
  ITF_REQUIRE(client.ok());
  ITF_CHECK_EQ(client.value().max_frame_payload(), 4096U);

  // The client refuses to send anything larger than the negotiated limit.
  std::vector<std::uint8_t> oversized(8192, 0x41);
  const Status sent = client.value().send_frame(wire::MessageType::Heartbeat,
                                                ByteSpan(oversized.data(), oversized.size()));
  ITF_CHECK(!sent.ok());
  ITF_CHECK_EQ(static_cast<int>(sent.code()), static_cast<int>(StatusCode::BoundsExceeded));

  // A raw peer that ignores the limit is refused by the server.
  auto stream = net::TcpStream::connect("127.0.0.1", port, 2000000000LL);
  ITF_REQUIRE(stream.ok());
  RawSession session;
  session.stream = std::move(stream).value();
  wire::HelloRequest hello;
  hello.peer_name = "greedy-peer";
  hello.auth_token = kToken;
  hello.client_boot = BootId(3, 4);
  hello.max_frame_payload = 4096;
  Writer writer;
  hello.encode(writer);
  ITF_REQUIRE(session.send(wire::MessageType::HelloRequest,
                           ByteSpan(writer.buffer().data(), writer.buffer().size()))
                  .ok());
  wire::MessageType type = wire::MessageType::Invalid;
  ITF_REQUIRE(session.receive(type).ok());
  ITF_CHECK_EQ(static_cast<int>(type), static_cast<int>(wire::MessageType::HelloResponse));

  std::vector<std::uint8_t> header(wire::kHeaderBytes, 0);
  wire::FrameHeader frame;
  frame.type = wire::MessageType::Heartbeat;
  frame.payload_length = 8192;
  frame.sequence = 2;
  frame.payload_crc32c = Crc32c::compute(ByteSpan());
  wire::encode_header(frame, MutableByteSpan(header.data(), header.size()));
  ITF_REQUIRE_STATUS_OK(session.stream.write_all(ByteSpan(header.data(), header.size())));
  const auto response = session.receive(type);
  ITF_REQUIRE(response.ok());
  Reader reader(ByteSpan(response.value().data(), response.value().size()));
  const auto error = wire::ErrorResponse::decode(reader);
  ITF_REQUIRE(error.ok());
  ITF_CHECK_EQ(static_cast<int>(error.value().code), static_cast<int>(StatusCode::BoundsExceeded));
  session.stream.close();
  server->stop();
}

ITF_TEST(mutated_frames_never_confuse_the_server) {
  AckHandler handler;
  net::ServerConfig config;
  config.io_timeout_nanos = 200000000;
  std::uint16_t port = 0;
  auto server = start_server(handler, config, port);
  ITF_REQUIRE(server != nullptr);

  // Build one valid frame, then send systematic mutations of it over fresh
  // connections. The server must answer with an error frame or close, never
  // with success and never by hanging.
  std::vector<std::uint8_t> payload = {0, 0, 0, 0, 0, 0, 0, 0};
  std::vector<std::uint8_t> header(wire::kHeaderBytes, 0);
  wire::FrameHeader frame;
  frame.type = wire::MessageType::Heartbeat;
  frame.payload_length = static_cast<std::uint32_t>(payload.size());
  frame.payload_crc32c = Crc32c::compute(ByteSpan(payload.data(), payload.size()));
  frame.sequence = 1;
  wire::encode_header(frame, MutableByteSpan(header.data(), header.size()));

  int completed_handshakes = 0;
  for (std::size_t index = 0; index < header.size(); ++index) {
    for (int bit = 0; bit < 8; bit += 3) {
      std::vector<std::uint8_t> mutated = header;
      mutated[index] = static_cast<std::uint8_t>(mutated[index] ^ (1U << bit));
      auto stream = net::TcpStream::connect("127.0.0.1", port, 2000000000LL);
      ITF_REQUIRE(stream.ok());
      ITF_REQUIRE_STATUS_OK(stream.value().write_all(ByteSpan(mutated.data(), mutated.size())));
      ITF_REQUIRE_STATUS_OK(
          stream.value().write_all(ByteSpan(payload.data(), payload.size())));
      std::vector<std::uint8_t> reply(wire::kHeaderBytes, 0);
      const Status read = stream.value().read_exact(
          MutableByteSpan(reply.data(), reply.size()), 3000000000LL);
      if (read.ok()) {
        // Any answer to a mutated handshake must be an error frame.
        const auto decoded = wire::decode_header(ByteSpan(reply.data(), reply.size()), 65536);
        if (decoded.ok()) {
          ITF_CHECK_EQ(static_cast<int>(decoded.value().type),
                       static_cast<int>(wire::MessageType::ErrorResponse));
          ++completed_handshakes;
        }
      }
      stream.value().close();
    }
  }
  std::printf("  frame mutations answered with a deterministic error frame: %d\n",
              completed_handshakes);
  ITF_CHECK(completed_handshakes > 0);
  ITF_CHECK(itf::test::wait_until([&] { return server->stats().active_sessions == 0U; }, 10000));

  // The server is still healthy afterwards.
  net::ClientConfig client_config;
  client_config.port = port;
  client_config.peer_name = "after-mutations";
  client_config.auth_token = kToken;
  auto client = net::Client::connect(client_config);
  ITF_REQUIRE(client.ok());
  ITF_CHECK(client.value().heartbeat().ok());
  ITF_CHECK_EQ(server->stats().rejected_auth, 0ULL);
  server->stop();
}

ITF_TEST(truncated_frames_do_not_wedge_the_server) {
  AckHandler handler;
  net::ServerConfig config;
  config.io_timeout_nanos = 200000000;
  std::uint16_t port = 0;
  auto server = start_server(handler, config, port);
  ITF_REQUIRE(server != nullptr);

  for (int index = 0; index < 4; ++index) {
    auto stream = net::TcpStream::connect("127.0.0.1", port, 2000000000LL);
    ITF_REQUIRE(stream.ok());
    const std::string partial = "ITF1";
    (void)stream.value().write_all(as_bytes(partial));
    stream.value().close();
  }
  ITF_CHECK(itf::test::wait_until([&] { return server->stats().active_sessions == 0U; }, 8000));

  net::ClientConfig client_config;
  client_config.port = port;
  client_config.peer_name = "after-truncation";
  client_config.auth_token = kToken;
  auto client = net::Client::connect(client_config);
  ITF_REQUIRE(client.ok());
  ITF_CHECK(client.value().heartbeat().ok());
  server->stop();
}

}  // namespace

int main(int argc, char** argv) { return itf::test::run_all("test_protocol_session", argc, argv); }
