// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "itf/client.hpp"
#include "itf/crc32c.hpp"
#include "service.hpp"
#include "support/test.hpp"
#include "support/test_support.hpp"

namespace {

using namespace itf;

const std::string kToken = "server-secret";

/// Minimal handler used to isolate transport behaviour from serving policy.
class EchoHandler final : public net::MessageHandler {
 public:
  Result<std::vector<std::uint8_t>> handle(Context& context, wire::MessageType type,
                                           ByteSpan payload) override {
    (void)context;
    if (type == wire::MessageType::Heartbeat) {
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
    return Result<std::vector<std::uint8_t>>::failure(StatusCode::Unsupported,
                                                      "the echo handler only answers heartbeats");
  }

  void describe_authority(wire::HelloResponse& response) override {
    response.epoch = CoordinatorEpoch::from(3);
    response.policy_generation = PolicyGeneration::from(2);
  }
};

std::unique_ptr<net::Server> make_server(EchoHandler& handler, std::uint32_t max_sessions,
                                         std::uint16_t& port) {
  net::ServerConfig config;
  config.bind_host = "127.0.0.1";
  config.port = 0;
  config.max_sessions = max_sessions;
  config.auth_token = kToken;
  config.io_timeout_nanos = 200000000;
  auto server = std::make_unique<net::Server>(config, &handler);
  if (!server->start().ok()) return nullptr;
  port = server->bound_port();
  return server;
}

ITF_TEST(repeated_start_and_stop_cycles_are_clean) {
  EchoHandler handler;
  for (int cycle = 0; cycle < 12; ++cycle) {
    std::uint16_t port = 0;
    auto server = make_server(handler, 4, port);
    ITF_REQUIRE(server != nullptr);
    auto connected = net::Client::connect(net::ClientConfig{
        "127.0.0.1", port, "cycle-client", kToken, "1.0.0", 65536, 5000000000LL, 5000000000LL});
    ITF_REQUIRE(connected.ok());
    ITF_CHECK(connected.value().heartbeat().ok());
    server->stop();
    ITF_CHECK(!server->running());
  }
}

ITF_TEST(concurrent_clients_are_served_and_counted) {
  EchoHandler handler;
  std::uint16_t port = 0;
  auto server = make_server(handler, 8, port);
  ITF_REQUIRE(server != nullptr);

  std::atomic<int> successes{0};
  std::vector<std::thread> threads;
  for (int index = 0; index < 6; ++index) {
    threads.emplace_back([&, index] {
      net::ClientConfig config;
      config.port = port;
      config.peer_name = "worker-" + std::to_string(index);
      config.auth_token = kToken;
      auto client = net::Client::connect(config);
      if (!client.ok()) return;
      for (int round = 0; round < 20; ++round) {
        if (client.value().heartbeat().ok()) successes.fetch_add(1);
      }
    });
  }
  for (std::thread& thread : threads) thread.join();
  ITF_CHECK_EQ(successes.load(), 120);
  const net::SessionStats stats = server->stats();
  ITF_CHECK_EQ(stats.accepted, 6ULL);
  ITF_CHECK_EQ(stats.rejected_auth, 0ULL);
  ITF_CHECK_EQ(stats.frames_invalid, 0ULL);
  // Session teardown is asynchronous: the counter settles once every worker
  // has observed the disconnect and finished.
  ITF_CHECK(itf::test::wait_until([&] { return server->stats().active_sessions == 0U; }, 8000));
  server->stop();
}

ITF_TEST(stop_unblocks_a_session_that_never_sends) {
  EchoHandler handler;
  std::uint16_t port = 0;
  auto server = make_server(handler, 4, port);
  ITF_REQUIRE(server != nullptr);

  // A raw connection that completes no handshake and then stays silent: the
  // session is blocked inside a read when stop() is called.
  auto stream = net::TcpStream::connect("127.0.0.1", port, 2000000000LL);
  ITF_REQUIRE(stream.ok());
  ITF_CHECK(itf::test::wait_until([&] { return server->stats().active_sessions == 1U; }, 5000));

  const auto started = std::chrono::steady_clock::now();
  server->stop();
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - started)
                           .count();
  ITF_CHECK(!server->running());
  ITF_CHECK(elapsed < 5000);
  stream.value().close();
}

ITF_TEST(session_capacity_is_bounded) {
  EchoHandler handler;
  std::uint16_t port = 0;
  auto server = make_server(handler, 1, port);
  ITF_REQUIRE(server != nullptr);

  net::ClientConfig config;
  config.port = port;
  config.peer_name = "capacity-client";
  config.auth_token = kToken;
  auto first = net::Client::connect(config);
  ITF_REQUIRE(first.ok());
  ITF_CHECK(itf::test::wait_until([&] { return server->stats().active_sessions == 1U; }, 5000));

  auto second = net::Client::connect(config);
  ITF_CHECK(!second.ok());
  ITF_CHECK(server->stats().rejected_capacity >= 1ULL);

  first.value().close();
  ITF_CHECK(itf::test::wait_until([&] { return server->stats().active_sessions == 0U; }, 5000));
  auto third = net::Client::connect(config);
  ITF_CHECK(third.ok());
  server->stop();
}

ITF_TEST(abrupt_client_disconnects_leave_the_server_healthy) {
  EchoHandler handler;
  std::uint16_t port = 0;
  auto server = make_server(handler, 4, port);
  ITF_REQUIRE(server != nullptr);

  for (int index = 0; index < 5; ++index) {
    auto stream = net::TcpStream::connect("127.0.0.1", port, 2000000000LL);
    ITF_REQUIRE(stream.ok());
    // A full header's worth of garbage decodes as a malformed frame.
    const std::string garbage = "not-a-frame-not-a-frame-not-a-frame-not-a-frame!";
    ITF_REQUIRE_STATUS_OK(stream.value().write_all(as_bytes(garbage)));
    std::array<std::uint8_t, 64> response{};
    // The server answers with one error frame and then closes the session.
    (void)stream.value().read_exact(MutableByteSpan(response.data(), response.size()),
                                    3000000000LL);
    stream.value().close();
  }
  ITF_CHECK(itf::test::wait_until([&] { return server->stats().active_sessions == 0U; }, 8000));
  ITF_CHECK(server->stats().frames_invalid >= 5ULL);

  net::ClientConfig config;
  config.port = port;
  config.peer_name = "after-garbage";
  config.auth_token = kToken;
  auto client = net::Client::connect(config);
  ITF_REQUIRE(client.ok());
  ITF_CHECK(client.value().heartbeat().ok());
  server->stop();
}

ITF_TEST(service_survives_repeated_lifecycle_churn) {
  for (int cycle = 0; cycle < 6; ++cycle) {
    const std::string directory = itf::test::make_temp_dir("server-churn");
    service::ServiceConfig config;
    config.server.bind_host = "127.0.0.1";
    config.server.port = 0;
    config.server.auth_token = kToken;
    config.persist = true;
    config.snapshot_path = directory + "/coordinator.bin";
    config.coordinator.boot_seed = static_cast<std::uint64_t>(cycle) + 1;
    auto service = std::make_unique<service::CoordinatorService>(config, &steady_clock_singleton());
    ITF_REQUIRE_STATUS_OK(service->start());
    net::ClientConfig client_config;
    client_config.port = service->server().bound_port();
    client_config.peer_name = "churn-client";
    client_config.auth_token = kToken;
    auto client = net::Client::connect(client_config);
    ITF_REQUIRE(client.ok());
    ITF_CHECK(client.value().status().ok());
    client.value().close();
    service->stop();
    service.reset();
    itf::test::remove_dir(directory);
  }
}


/// The handshake frame every client sends as soon as it has connected.
std::vector<std::uint8_t> handshake_frame() {
  wire::HelloRequest hello;
  hello.peer_name = "raw-peer";
  hello.auth_token = kToken;
  hello.client_boot = BootId(7, 9);
  Writer writer;
  hello.encode(writer);
  std::vector<std::uint8_t> frame(wire::kHeaderBytes, 0);
  wire::FrameHeader header;
  header.type = wire::MessageType::HelloRequest;
  header.payload_length = static_cast<std::uint32_t>(writer.buffer().size());
  header.sequence = 1;
  header.payload_crc32c =
      Crc32c::compute(ByteSpan(writer.buffer().data(), writer.buffer().size()));
  wire::encode_header(header, MutableByteSpan(frame.data(), frame.size()));
  frame.insert(frame.end(), writer.buffer().data(), writer.buffer().data() + writer.buffer().size());
  return frame;
}

ITF_TEST(a_capacity_refusal_reaches_a_client_that_already_sent_its_handshake) {
  EchoHandler handler;
  std::uint16_t port = 0;
  auto server = make_server(handler, 1, port);
  ITF_REQUIRE(server != nullptr);

  net::ClientConfig config;
  config.port = port;
  config.peer_name = "capacity-holder";
  config.auth_token = kToken;
  auto holder = net::Client::connect(config);
  ITF_REQUIRE(holder.ok());
  ITF_CHECK(itf::test::wait_until([&] { return server->stats().active_sessions == 1U; }, 5000));

  // A second client connects and, like every client, sends its handshake at
  // once. Only then does it read what the server had to say.
  auto stream = net::TcpStream::connect("127.0.0.1", port, 2000000000LL);
  ITF_REQUIRE(stream.ok());
  const std::vector<std::uint8_t> hello = handshake_frame();
  (void)stream.value().write_all(ByteSpan(hello.data(), hello.size()));
  ITF_CHECK(itf::test::wait_until([&] { return server->stats().rejected_capacity >= 1ULL; }, 5000));
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  std::vector<std::uint8_t> header(wire::kHeaderBytes, 0);
  const Status read = stream.value().read_exact(MutableByteSpan(header.data(), header.size()),
                                                3000000000LL);
  ITF_CHECK_STATUS_OK(read);
  if (read.ok()) {
    const auto decoded = wire::decode_header(ByteSpan(header.data(), header.size()), 65536);
    ITF_REQUIRE(decoded.ok());
    ITF_CHECK_EQ(static_cast<int>(decoded.value().type),
                 static_cast<int>(wire::MessageType::ErrorResponse));
    std::vector<std::uint8_t> payload(decoded.value().payload_length, 0);
    ITF_REQUIRE_STATUS_OK(stream.value().read_exact(
        MutableByteSpan(payload.data(), payload.size()), 3000000000LL));
    Reader reader(ByteSpan(payload.data(), payload.size()));
    const auto error = wire::ErrorResponse::decode(reader);
    ITF_REQUIRE(error.ok());
    ITF_CHECK_EQ(static_cast<int>(error.value().code),
                 static_cast<int>(StatusCode::CapacityExhausted));
  }
  stream.value().close();
  holder.value().close();
  server->stop();
}

ITF_TEST(a_silent_session_is_retired_after_the_idle_timeout) {
  EchoHandler handler;
  net::ServerConfig server_config;
  server_config.bind_host = "127.0.0.1";
  server_config.port = 0;
  server_config.max_sessions = 1;
  server_config.auth_token = kToken;
  server_config.io_timeout_nanos = 50000000;
  server_config.idle_timeout_nanos = 400000000;
  net::Server server(server_config, &handler);
  ITF_REQUIRE_STATUS_OK(server.start());
  const std::uint16_t port = server.bound_port();

  // A session that keeps talking is never idle, however long it lasts.
  net::ClientConfig config;
  config.port = port;
  config.peer_name = "talkative";
  config.auth_token = kToken;
  {
    auto client = net::Client::connect(config);
    ITF_REQUIRE(client.ok());
    int answered = 0;
    for (int round = 0; round < 8; ++round) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      if (client.value().heartbeat().ok()) ++answered;
    }
    ITF_CHECK_EQ(answered, 8);
    client.value().close();
  }
  ITF_CHECK(itf::test::wait_until([&] { return server.stats().active_sessions == 0U; }, 5000));

  // A peer that connects and then says nothing holds the only session slot.
  // It has not even proved it knows the secret.
  auto silent = net::TcpStream::connect("127.0.0.1", port, 2000000000LL);
  ITF_REQUIRE(silent.ok());
  ITF_CHECK(itf::test::wait_until([&] { return server.stats().active_sessions == 1U; }, 5000));
  ITF_CHECK(itf::test::wait_until([&] { return server.stats().active_sessions == 0U; }, 5000));

  // The slot is free again for a client that has something to say.
  config.peer_name = "after-silence";
  auto client = net::Client::connect(config);
  ITF_CHECK(client.ok());
  if (client.ok()) ITF_CHECK(client.value().heartbeat().ok());
  silent.value().close();
  server.stop();
}

}  // namespace

int main(int argc, char** argv) { return itf::test::run_all("test_concurrency_server", argc, argv); }
