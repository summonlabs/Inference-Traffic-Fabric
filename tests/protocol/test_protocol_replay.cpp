// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <string>
#include <vector>

#include "itf/client.hpp"
#include "itf/crc32c.hpp"
#include "service.hpp"
#include "support/test.hpp"
#include "support/test_support.hpp"

namespace {

using namespace itf;

const std::string kToken = "replay-secret";

class ReplayFixture {
 public:
  ReplayFixture() {
    directory_ = itf::test::make_temp_dir("protocol-replay");
    service::ServiceConfig config;
    config.server.bind_host = "127.0.0.1";
    config.server.port = 0;
    config.server.auth_token = kToken;
    config.server.require_auth = true;
    config.persist = false;
    config.coordinator.boot_seed = 41;
    service_ = std::make_unique<service::CoordinatorService>(config, &steady_clock_singleton());
  }

  ~ReplayFixture() {
    if (service_) {
      service_->stop();
      service_.reset();
    }
    itf::test::remove_dir(directory_);
  }

  Status start() { return service_->start(); }
  [[nodiscard]] std::uint16_t port() const { return service_->server().bound_port(); }

  Result<net::Client> connect(const std::string& name) const {
    net::ClientConfig config;
    config.host = "127.0.0.1";
    config.port = port();
    config.peer_name = name;
    config.auth_token = kToken;
    return net::Client::connect(config);
  }

  [[nodiscard]] net::Server& server() const { return service_->server(); }

 private:
  std::string directory_;
  std::unique_ptr<service::CoordinatorService> service_;
};

std::vector<std::uint8_t> heartbeat_body() {
  wire::HeartbeatMessage heartbeat;
  Writer writer;
  heartbeat.encode(writer);
  return writer.take();
}

ITF_TEST(replayed_sequences_are_refused) {
  ReplayFixture fixture;
  ITF_REQUIRE_STATUS_OK(fixture.start());
  auto connected = fixture.connect("replayer");
  ITF_REQUIRE(connected.ok());
  net::Client& client = connected.value();
  const std::vector<std::uint8_t> body = heartbeat_body();

  ITF_REQUIRE_STATUS_OK(
      client.send_frame_raw(wire::MessageType::Heartbeat, ByteSpan(body.data(), body.size()), 10, 0));
  const auto first = client.receive_frame(wire::MessageType::HeartbeatAck);
  ITF_REQUIRE(first.ok());

  // The same sequence number again is a replay.
  ITF_REQUIRE_STATUS_OK(
      client.send_frame_raw(wire::MessageType::Heartbeat, ByteSpan(body.data(), body.size()), 10, 0));
  const auto replayed = client.receive_frame(wire::MessageType::HeartbeatAck);
  ITF_CHECK(!replayed.ok());
  ITF_CHECK_EQ(static_cast<int>(replayed.code()), static_cast<int>(StatusCode::ReplayRejected));
  ITF_CHECK(fixture.server().stats().replays_rejected >= 1ULL);
}

ITF_TEST(out_of_order_sequences_are_refused) {
  ReplayFixture fixture;
  ITF_REQUIRE_STATUS_OK(fixture.start());
  auto connected = fixture.connect("out-of-order");
  ITF_REQUIRE(connected.ok());
  net::Client& client = connected.value();
  const std::vector<std::uint8_t> body = heartbeat_body();

  ITF_REQUIRE_STATUS_OK(
      client.send_frame_raw(wire::MessageType::Heartbeat, ByteSpan(body.data(), body.size()), 40, 0));
  ITF_REQUIRE(client.receive_frame(wire::MessageType::HeartbeatAck).ok());

  ITF_REQUIRE_STATUS_OK(
      client.send_frame_raw(wire::MessageType::Heartbeat, ByteSpan(body.data(), body.size()), 12, 0));
  const auto stale = client.receive_frame(wire::MessageType::HeartbeatAck);
  ITF_CHECK(!stale.ok());
  ITF_CHECK_EQ(static_cast<int>(stale.code()), static_cast<int>(StatusCode::ReplayRejected));
}

ITF_TEST(persistent_replay_eventually_closes_the_session) {
  ReplayFixture fixture;
  ITF_REQUIRE_STATUS_OK(fixture.start());
  auto connected = fixture.connect("persistent-replayer");
  ITF_REQUIRE(connected.ok());
  net::Client& client = connected.value();
  const std::vector<std::uint8_t> body = heartbeat_body();

  ITF_REQUIRE_STATUS_OK(
      client.send_frame_raw(wire::MessageType::Heartbeat, ByteSpan(body.data(), body.size()), 2, 0));
  ITF_REQUIRE(client.receive_frame(wire::MessageType::HeartbeatAck).ok());

  bool closed = false;
  for (int attempt = 0; attempt < 20 && !closed; ++attempt) {
    const Status sent = client.send_frame_raw(wire::MessageType::Heartbeat,
                                              ByteSpan(body.data(), body.size()), 2, 0);
    if (!sent.ok()) {
      closed = true;
      break;
    }
    const auto response = client.receive_frame(wire::MessageType::HeartbeatAck);
    if (!response.ok() && response.code() == StatusCode::Closed) closed = true;
  }
  ITF_CHECK(closed);
  ITF_CHECK(fixture.server().stats().replays_rejected >= 8ULL);
}

ITF_TEST(a_corrupted_payload_is_refused_by_checksum) {
  ReplayFixture fixture;
  ITF_REQUIRE_STATUS_OK(fixture.start());

  // The client recomputes frame checksums, so corruption is injected on a raw
  // stream exactly as a hostile peer would.
  auto stream = net::TcpStream::connect("127.0.0.1", fixture.port(), 2000000000LL);
  ITF_REQUIRE(stream.ok());
  wire::HelloRequest hello;
  hello.peer_name = "corrupter";
  hello.auth_token = kToken;
  hello.client_boot = BootId(7, 8);
  Writer hello_writer;
  hello.encode(hello_writer);
  std::vector<std::uint8_t> header(wire::kHeaderBytes, 0);
  wire::FrameHeader frame;
  frame.type = wire::MessageType::HelloRequest;
  frame.payload_length = static_cast<std::uint32_t>(hello_writer.size());
  frame.sequence = 1;
  frame.payload_crc32c =
      Crc32c::compute(ByteSpan(hello_writer.buffer().data(), hello_writer.size()));
  wire::encode_header(frame, MutableByteSpan(header.data(), header.size()));
  ITF_REQUIRE_STATUS_OK(stream.value().write_all(ByteSpan(header.data(), header.size())));
  ITF_REQUIRE_STATUS_OK(
      stream.value().write_all(ByteSpan(hello_writer.buffer().data(), hello_writer.size())));
  std::vector<std::uint8_t> response_header(wire::kHeaderBytes, 0);
  ITF_REQUIRE_STATUS_OK(stream.value().read_exact(
      MutableByteSpan(response_header.data(), response_header.size()), 3000000000LL));
  const auto decoded =
      wire::decode_header(ByteSpan(response_header.data(), response_header.size()), 65536);
  ITF_REQUIRE(decoded.ok());
  ITF_CHECK_EQ(static_cast<int>(decoded.value().type),
               static_cast<int>(wire::MessageType::HelloResponse));
  std::vector<std::uint8_t> response_body(decoded.value().payload_length, 0);
  if (!response_body.empty()) {
    ITF_REQUIRE_STATUS_OK(stream.value().read_exact(
        MutableByteSpan(response_body.data(), response_body.size()), 3000000000LL));
  }

  const std::vector<std::uint8_t> body = heartbeat_body();
  frame.type = wire::MessageType::Heartbeat;
  frame.payload_length = static_cast<std::uint32_t>(body.size());
  frame.sequence = 2;
  frame.payload_crc32c = Crc32c::compute(ByteSpan(body.data(), body.size())) ^ 0xFFFFFFFFU;
  wire::encode_header(frame, MutableByteSpan(header.data(), header.size()));
  ITF_REQUIRE_STATUS_OK(stream.value().write_all(ByteSpan(header.data(), header.size())));
  ITF_REQUIRE_STATUS_OK(stream.value().write_all(ByteSpan(body.data(), body.size())));

  std::vector<std::uint8_t> error_header(wire::kHeaderBytes, 0);
  const Status read = stream.value().read_exact(
      MutableByteSpan(error_header.data(), error_header.size()), 3000000000LL);
  ITF_REQUIRE_STATUS_OK(read);
  const auto error_frame =
      wire::decode_header(ByteSpan(error_header.data(), error_header.size()), 65536);
  ITF_REQUIRE(error_frame.ok());
  ITF_CHECK_EQ(static_cast<int>(error_frame.value().type),
               static_cast<int>(wire::MessageType::ErrorResponse));
  std::vector<std::uint8_t> error_body(error_frame.value().payload_length, 0);
  if (!error_body.empty()) {
    ITF_REQUIRE_STATUS_OK(stream.value().read_exact(
        MutableByteSpan(error_body.data(), error_body.size()), 3000000000LL));
  }
  Reader reader(ByteSpan(error_body.data(), error_body.size()));
  const auto error = wire::ErrorResponse::decode(reader);
  ITF_REQUIRE(error.ok());
  ITF_CHECK_EQ(static_cast<int>(error.value().code),
               static_cast<int>(StatusCode::IntegrityMismatch));
  ITF_CHECK(fixture.server().stats().frames_invalid >= 1ULL);
  stream.value().close();
}

ITF_TEST(sessions_are_isolated_from_each_other) {
  ReplayFixture fixture;
  ITF_REQUIRE_STATUS_OK(fixture.start());
  auto first = fixture.connect("session-a");
  ITF_REQUIRE(first.ok());
  auto second = fixture.connect("session-b");
  ITF_REQUIRE(second.ok());
  ITF_CHECK(first.value().hello().session != second.value().hello().session);
  ITF_CHECK(first.value().hello().peer != second.value().hello().peer);

  // Sequence numbers are per session, so an identical sequence on another
  // session is not a replay.
  const std::vector<std::uint8_t> body = heartbeat_body();
  ITF_REQUIRE_STATUS_OK(first.value().send_frame_raw(wire::MessageType::Heartbeat,
                                                     ByteSpan(body.data(), body.size()), 2, 0));
  ITF_REQUIRE(first.value().receive_frame(wire::MessageType::HeartbeatAck).ok());
  ITF_REQUIRE_STATUS_OK(second.value().send_frame_raw(wire::MessageType::Heartbeat,
                                                      ByteSpan(body.data(), body.size()), 2, 0));
  ITF_REQUIRE(second.value().receive_frame(wire::MessageType::HeartbeatAck).ok());
  ITF_CHECK_EQ(fixture.server().stats().replays_rejected, 0ULL);
}

}  // namespace

int main(int argc, char** argv) { return itf::test::run_all("test_protocol_replay", argc, argv); }
