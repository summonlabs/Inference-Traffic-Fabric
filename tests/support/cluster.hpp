// Inference Traffic Fabric - multiprocess cluster fixture.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef ITF_TEST_CLUSTER_HPP
#define ITF_TEST_CLUSTER_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "itf/client.hpp"
#include "support/test_support.hpp"

namespace itf::test {

/// A real coordinator process plus worker processes, all talking over real
/// loopback TCP. Nothing here is simulated in-process.
class Cluster {
 public:
  Cluster();
  ~Cluster();

  Cluster(const Cluster&) = delete;
  Cluster& operator=(const Cluster&) = delete;

  /// Spawns the coordinator and waits until it accepts connections.
  Status start(const std::vector<std::string>& extra_arguments = {});
  /// Stops the coordinator gracefully through its shutdown message.
  void stop();
  /// Force-kills the coordinator, simulating abrupt death.
  void kill();

  [[nodiscard]] bool running();
  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
  [[nodiscard]] const std::string& token() const noexcept { return token_; }
  [[nodiscard]] const std::string& directory() const noexcept { return directory_; }
  [[nodiscard]] const std::string& snapshot_path() const noexcept { return snapshot_path_; }
  [[nodiscard]] const std::string& coordinator_log() const noexcept { return coordinator_log_; }

  [[nodiscard]] Result<net::Client> connect(const std::string& peer_name) const;
  [[nodiscard]] std::string result_path(const std::string& tag) const;

  /// The coordinator child process, exposed so tests can observe exit codes.
  [[nodiscard]] ChildProcess& process() noexcept { return coordinator_; }

 private:
  std::string directory_;
  std::string token_;
  std::string snapshot_path_;
  std::string coordinator_log_;
  std::uint16_t port_ = 0;
  ChildProcess coordinator_;
};

/// Convenience: publishes a synthetic disaggregated topology and binds model
/// and state generations, which most scenarios need before traffic is legal.
Status prepare_fabric(net::Client& client, std::uint64_t model_hash, std::uint64_t state_hash);

}  // namespace itf::test

#endif  // ITF_TEST_CLUSTER_HPP
