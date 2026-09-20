// Inference Traffic Fabric - coordinator service used by the tools.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef ITF_TOOLS_SERVICE_HPP
#define ITF_TOOLS_SERVICE_HPP

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

#include "itf/client.hpp"
#include "itf/coordinator.hpp"
#include "itf/persistence.hpp"
#include "itf/session.hpp"

namespace itf::service {

struct ServiceConfig {
  net::ServerConfig server;
  CoordinatorConfig coordinator;
  /// Durable state file. An empty path disables persistence entirely.
  std::string snapshot_path;
  /// Minimum interval between automatic snapshots after a mutating message.
  /// Zero snapshots after every mutating message.
  std::int64_t snapshot_interval_nanos = 0;
  bool persist = false;
  /// Refuse sessions from peers whose address is not a loopback address.
  bool loopback_only = true;
};

/// Translates framed session messages into coordinator operations. The service
/// owns the coordinator and the server; it never holds its own lock while the
/// coordinator or the server is called.
class CoordinatorService final : public net::MessageHandler {
 public:
  CoordinatorService(ServiceConfig config, Clock* clock);
  ~CoordinatorService() override;

  CoordinatorService(const CoordinatorService&) = delete;
  CoordinatorService& operator=(const CoordinatorService&) = delete;

  /// Loads durable state when persistence is enabled, boots the coordinator
  /// and starts the session server.
  Status start();
  void stop();

  [[nodiscard]] Result<std::vector<std::uint8_t>> handle(Context& context, wire::MessageType type,
                                                         ByteSpan payload) override;
  void describe_authority(wire::HelloResponse& response) override;

  [[nodiscard]] Coordinator& coordinator() noexcept { return *coordinator_; }
  [[nodiscard]] const Coordinator& coordinator() const noexcept { return *coordinator_; }
  [[nodiscard]] net::Server& server() noexcept { return *server_; }
  [[nodiscard]] bool shutdown_requested() const noexcept { return shutdown_requested_.load(); }
  void request_shutdown() noexcept { shutdown_requested_.store(true); }
  [[nodiscard]] const ServiceConfig& config() const noexcept { return config_; }

  /// Copies durable state under the coordinator lock and writes it outside.
  Status save_snapshot();
  [[nodiscard]] const std::string& last_snapshot_error() const noexcept {
    return last_snapshot_error_;
  }

 private:
  void maybe_snapshot(bool mutated);

  ServiceConfig config_;
  Clock* clock_;
  std::unique_ptr<Coordinator> coordinator_;
  std::unique_ptr<net::Server> server_;
  std::atomic<bool> shutdown_requested_{false};
  std::mutex snapshot_mutex_;
  std::int64_t last_snapshot_nanos_ = 0;
  std::string last_snapshot_error_;
};

/// Runs the coordinator service until a shutdown request or an interrupt.
/// Returns the process exit code.
int run_coordinator_service(const ServiceConfig& config, bool verbose);

}  // namespace itf::service

#endif  // ITF_TOOLS_SERVICE_HPP
