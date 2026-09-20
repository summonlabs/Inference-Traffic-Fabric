// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <array>
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "itf/client.hpp"
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

}  // namespace

int main(int argc, char** argv) { return itf::test::run_all("test_concurrency_server", argc, argv); }
