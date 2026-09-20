// Inference Traffic Fabric - framed session server.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef ITF_SESSION_HPP
#define ITF_SESSION_HPP

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "itf/bytes.hpp"
#include "itf/error.hpp"
#include "itf/ids.hpp"
#include "itf/messages.hpp"
#include "itf/net.hpp"
#include "itf/wire.hpp"

namespace itf::net {

struct SessionStats {
  std::uint64_t accepted = 0;
  std::uint64_t rejected_capacity = 0;
  std::uint64_t rejected_auth = 0;
  std::uint64_t rejected_protocol = 0;
  std::uint64_t frames_in = 0;
  std::uint64_t frames_out = 0;
  std::uint64_t frames_invalid = 0;
  std::uint64_t replays_rejected = 0;
  std::uint64_t duplicates_served = 0;
  std::uint64_t bytes_in = 0;
  std::uint64_t bytes_out = 0;
  std::uint32_t active_sessions = 0;
  std::uint32_t peak_sessions = 0;

  [[nodiscard]] std::string to_string() const;
};

/// Implemented by the coordinator service. The handler is invoked from session
/// threads with no server lock held, and it must not re-enter the server.
class MessageHandler {
 public:
  struct Context {
    SessionId session;
    PeerId peer;
    std::string peer_name;
    BootId peer_boot;
    std::uint32_t max_frame_payload = 0;
    std::atomic<bool>* shutdown_requested = nullptr;
  };

  MessageHandler() = default;
  virtual ~MessageHandler() = default;
  MessageHandler(const MessageHandler&) = delete;
  MessageHandler& operator=(const MessageHandler&) = delete;

  [[nodiscard]] virtual Result<std::vector<std::uint8_t>> handle(Context& context,
                                                                 wire::MessageType type,
                                                                 ByteSpan payload) = 0;

  /// Supplies the coordinator-owned fields of the handshake response. The
  /// server mints session identity; authority state is the handler's.
  virtual void describe_authority(wire::HelloResponse& response) { (void)response; }

  virtual void on_session_closed(const Context& context) { (void)context; }
};

struct ServerConfig {
  std::string bind_host = "127.0.0.1";
  std::uint16_t port = 0;
  std::uint32_t max_sessions = 32;
  std::size_t max_frame_payload = 256U * 1024U;
  std::int64_t idle_timeout_nanos = 30000000000LL;
  std::int64_t io_timeout_nanos = 1000000000LL;
  std::int64_t accept_poll_nanos = 100000000LL;
  std::string auth_token;
  bool require_auth = true;
  std::uint32_t accept_backlog = 32;
};

class Server {
 public:
  Server(ServerConfig config, MessageHandler* handler);
  ~Server();

  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  /// Binds the listener and starts the acceptor thread. Fails atomically: on
  /// failure no thread has been started and no socket remains bound.
  Status start();

  /// Stops accepting, signals every session, closes every socket to unblock
  /// blocked readers, then joins all workers outside any shared lock.
  void stop();

  [[nodiscard]] bool running() const noexcept { return running_.load(); }
  [[nodiscard]] std::uint16_t bound_port() const noexcept { return bound_port_; }
  [[nodiscard]] SessionStats stats() const;
  [[nodiscard]] const ServerConfig& config() const noexcept { return config_; }

 private:
  struct SessionSlot {
    std::shared_ptr<TcpStream> stream;
    std::thread worker;
    std::atomic<bool> finished{false};
  };

  void accept_loop();
  void serve_session(std::shared_ptr<SessionSlot> slot);
  void reap_finished_sessions();
  void join_all();

  ServerConfig config_;
  MessageHandler* handler_;
  TcpListener listener_;
  std::uint16_t bound_port_ = 0;
  std::atomic<bool> running_{false};
  std::atomic<bool> stopping_{false};
  std::thread acceptor_;
  mutable std::mutex sessions_mutex_;
  std::vector<std::shared_ptr<SessionSlot>> sessions_;
  std::atomic<std::uint32_t> active_sessions_{0};
  std::atomic<std::uint32_t> peak_sessions_{0};
  SessionStats stats_;
  mutable std::mutex stats_mutex_;
};

}  // namespace itf::net

#endif  // ITF_SESSION_HPP
