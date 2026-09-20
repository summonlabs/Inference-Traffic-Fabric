// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "support/cluster.hpp"

#include <string>
#include <vector>

namespace itf::test {

Cluster::Cluster() {
  directory_ = make_temp_dir("cluster");
  token_ = "cluster-secret-" + std::to_string(reinterpret_cast<std::uintptr_t>(this));
  snapshot_path_ = directory_ + "/coordinator.bin";
  coordinator_log_ = directory_ + "/coordinator.log";
}

Cluster::~Cluster() {
  if (coordinator_.valid() && coordinator_.running()) coordinator_.kill();
  remove_dir(directory_);
}

Status Cluster::start(const std::vector<std::string>& extra_arguments) {
  port_ = find_free_port();
  if (port_ == 0) return Status(StatusCode::IoError, "no free loopback port was available");
  std::vector<std::string> arguments = {"--port", std::to_string(port_), "--token", token_,
                                        "--state-dir", directory_, "--snapshot-interval-ms", "0",
                                        "--verbose"};
  arguments.insert(arguments.end(), extra_arguments.begin(), extra_arguments.end());
  auto spawned = ChildProcess::spawn(tool_path("itf_coordinator"), arguments);
  if (!spawned.ok()) return spawned.status();
  coordinator_ = std::move(spawned).value();

  const bool ready = wait_until(
      [this] {
        auto probe = connect("cluster-probe");
        return probe.ok();
      },
      20000);
  if (!ready) {
    return Status(StatusCode::IoError, "the coordinator did not become reachable");
  }
  return Status::success();
}

void Cluster::stop() {
  if (!coordinator_.valid()) return;
  if (coordinator_.running()) {
    auto client = connect("cluster-shutdown");
    if (client.ok()) {
      Writer writer;
      wire::HeartbeatMessage heartbeat;
      heartbeat.encode(writer);
      (void)client.value().exchange(wire::MessageType::Shutdown,
                                    ByteSpan(writer.buffer().data(), writer.buffer().size()));
      (void)wait_until([this] { return !coordinator_.running(); }, 10000);
    }
    if (coordinator_.running()) coordinator_.kill();
  }
  (void)coordinator_.wait();
}

void Cluster::kill() { coordinator_.kill(); }

bool Cluster::running() { return coordinator_.valid() && coordinator_.running(); }

Result<net::Client> Cluster::connect(const std::string& peer_name) const {
  net::ClientConfig config;
  config.host = "127.0.0.1";
  config.port = port_;
  config.peer_name = peer_name;
  config.auth_token = token_;
  return net::Client::connect(config);
}

std::string Cluster::result_path(const std::string& tag) const {
  return directory_ + "/" + tag + ".result";
}

Status prepare_fabric(net::Client& client, std::uint64_t model_hash, std::uint64_t state_hash) {
  TopologyEvidence topology;
  topology.provenance = EvidenceProvenance::Synthetic;
  topology.node_count = 2;
  topology.link_count = 1;
  topology.accelerator_count = 2;
  topology.fabric_bytes_per_sec = 100ULL * 1000ULL * 1000ULL * 1000ULL;
  topology.max_age_nanos = 60000000000ULL;
  topology.disaggregated = true;
  auto published = client.publish_topology(topology);
  if (!published.ok()) return published.status();
  auto model = client.advance_model_generation(model_hash, std::nullopt, false);
  if (!model.ok()) return model.status();
  auto state = client.advance_state_generation(state_hash, std::nullopt, false);
  if (!state.ok()) return state.status();
  return Status::success();
}

}  // namespace itf::test
