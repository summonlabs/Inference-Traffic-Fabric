// Inference Traffic Fabric - multiprocess serving simulator.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Each role is a real operating-system process that talks to the coordinator
// and to its peers over real loopback TCP with the fabric's framed transport.
// Nothing in this tool models the network in-process.

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "itf/client.hpp"
#include "itf/crc32c.hpp"
#include "itf/log.hpp"
#include "itf/serialize.hpp"
#include "itf/session.hpp"
#include "itf/version.hpp"

namespace {

using namespace itf;

constexpr int kExitOk = 0;
constexpr int kExitUsage = 2;
constexpr int kExitTransferFailed = 3;
constexpr int kExitCoordinator = 4;

struct Options {
  std::string coordinator_host = "127.0.0.1";
  std::uint16_t coordinator_port = 7777;
  std::string token;
  std::string name = "sim";
  std::uint16_t data_port = 0;
  std::uint16_t peer_port = 0;
  std::uint64_t bytes = 1U << 20;
  std::uint32_t chunks = 16;
  std::uint64_t chunk_dwell_ms = 0;
  std::uint64_t wait_ms = 10000;
  std::string request_hex;
  std::string attempt_hex;
  std::string transfer_hex;
  std::uint64_t model_hash = 0x1111222233334444ULL;
  std::uint64_t state_hash = 0x5555666677778888ULL;
  std::string stop_after;
  std::string result_file;
  std::string progress_file;
  bool init_generations = false;
  bool publish_topology = false;
  bool register_request = false;
  bool verbose = false;
};

[[nodiscard]] bool parse_u64(std::string_view text, std::uint64_t& out) {
  if (text.empty()) return false;
  std::uint64_t value = 0;
  for (const char raw : text) {
    if (raw < '0' || raw > '9') return false;
    const std::uint64_t digit = static_cast<std::uint64_t>(raw - '0');
    if (value > (UINT64_MAX - digit) / 10ULL) return false;
    value = value * 10ULL + digit;
  }
  out = value;
  return true;
}

void print_usage() {
  std::printf(
      "itf_sim - Inference Traffic Fabric multiprocess serving simulator\n"
      "\n"
      "usage: itf_sim decode  --token S --data-port N [--name N] [--wait-ms N]"
      " [--transfer HEX] [--result-file PATH]\n"
      "       itf_sim prefill --token S --coordinator H:P [--name N] --request HEX --attempt HEX"
      " [--transfer HEX]\n"
      "                       [--peer-port N] [--bytes N] [--chunks N] [--chunk-dwell-ms N]\n"
      "                       [--register-request] [--init-generations] [--publish-topology]\n"
      "                       [--stop-after PHASE] [--result-file PATH] [--progress-file PATH]\n"
      "\n"
      "phases for --stop-after: register-request register-attempt route slo stage"
      " register-transfer authorize stream complete\n");
}

[[nodiscard]] bool parse_options(int argc, char** argv, Options& options) {
  for (int index = 2; index < argc; ++index) {
    const std::string_view flag(argv[index]);
    const auto next = [&](const char* name) -> const char* {
      if (index + 1 >= argc) {
        std::fprintf(stderr, "missing value for %s\n", name);
        std::exit(kExitUsage);
      }
      return argv[++index];
    };
    std::uint64_t number = 0;
    if (flag == "--coordinator") {
      const std::string endpoint = next("--coordinator");
      const std::size_t colon = endpoint.rfind(':');
      if (colon == std::string::npos) return false;
      options.coordinator_host = endpoint.substr(0, colon);
      if (!parse_u64(endpoint.substr(colon + 1), number) || number > 65535) return false;
      options.coordinator_port = static_cast<std::uint16_t>(number);
    } else if (flag == "--token") {
      options.token = next("--token");
    } else if (flag == "--name") {
      options.name = next("--name");
    } else if (flag == "--data-port") {
      if (!parse_u64(next("--data-port"), number) || number > 65535) return false;
      options.data_port = static_cast<std::uint16_t>(number);
    } else if (flag == "--peer-port") {
      if (!parse_u64(next("--peer-port"), number) || number > 65535) return false;
      options.peer_port = static_cast<std::uint16_t>(number);
    } else if (flag == "--bytes") {
      if (!parse_u64(next("--bytes"), number) || number == 0) return false;
      options.bytes = number;
    } else if (flag == "--chunks") {
      if (!parse_u64(next("--chunks"), number) || number == 0 || number > 65536) return false;
      options.chunks = static_cast<std::uint32_t>(number);
    } else if (flag == "--chunk-dwell-ms") {
      if (!parse_u64(next("--chunk-dwell-ms"), number)) return false;
      options.chunk_dwell_ms = number;
    } else if (flag == "--wait-ms") {
      if (!parse_u64(next("--wait-ms"), number) || number == 0) return false;
      options.wait_ms = number;
    } else if (flag == "--request") {
      options.request_hex = next("--request");
    } else if (flag == "--attempt") {
      options.attempt_hex = next("--attempt");
    } else if (flag == "--transfer") {
      options.transfer_hex = next("--transfer");
    } else if (flag == "--model-hash") {
      if (!parse_u64(next("--model-hash"), number)) return false;
      options.model_hash = number;
    } else if (flag == "--state-hash") {
      if (!parse_u64(next("--state-hash"), number)) return false;
      options.state_hash = number;
    } else if (flag == "--stop-after") {
      options.stop_after = next("--stop-after");
    } else if (flag == "--result-file") {
      options.result_file = next("--result-file");
    } else if (flag == "--progress-file") {
      options.progress_file = next("--progress-file");
    } else if (flag == "--register-request") {
      options.register_request = true;
    } else if (flag == "--init-generations") {
      options.init_generations = true;
    } else if (flag == "--publish-topology") {
      options.publish_topology = true;
    } else if (flag == "--verbose") {
      options.verbose = true;
    } else {
      std::fprintf(stderr, "unknown option: %s\n", argv[index]);
      return false;
    }
  }
  return true;
}

void write_result(const Options& options, const std::string& text) {
  if (options.verbose) std::printf("%s\n", text.c_str());
  if (options.result_file.empty()) return;
  std::ofstream file(options.result_file, std::ios::trunc);
  file << text << "\n";
}

// ---------------------------------------------------------------------------
// Data plane receiver
// ---------------------------------------------------------------------------

class DataPlaneHandler final : public net::MessageHandler {
 public:
  explicit DataPlaneHandler(StateTransferId expected) : expected_(expected) {}

  Result<std::vector<std::uint8_t>> handle(Context& context, wire::MessageType type,
                                           ByteSpan payload) override {
    (void)context;
    if (type == wire::MessageType::DataChunk) return handle_chunk(payload);
    if (type == wire::MessageType::DataComplete) return handle_complete(payload);
    return Result<std::vector<std::uint8_t>>::failure(
        StatusCode::Unsupported, "the data plane accepts only data frames");
  }

  void on_session_closed(const Context& context) override {
    (void)context;
    std::lock_guard<std::mutex> guard(mutex_);
    if (!complete_ && !failed_) {
      failed_ = true;
      reason_ = "the data connection closed before the transfer completed";
      condition_.notify_all();
    }
  }

  void fail_with(const std::string& reason) {
    std::lock_guard<std::mutex> guard(mutex_);
    failed_ = true;
    reason_ = reason;
    condition_.notify_all();
  }

  [[nodiscard]] bool wait_for_outcome(std::uint64_t wait_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_for(lock, std::chrono::milliseconds(wait_ms),
                               [this] { return complete_ || failed_; });
  }

  [[nodiscard]] bool complete() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return complete_;
  }
  [[nodiscard]] bool verified() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return verified_;
  }
  [[nodiscard]] std::uint64_t received_bytes() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return received_bytes_;
  }
  [[nodiscard]] std::uint32_t received_chunks() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return received_chunks_;
  }
  [[nodiscard]] std::uint32_t content_crc() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return content_crc_;
  }
  [[nodiscard]] std::string reason() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return reason_;
  }

 private:
  Result<std::vector<std::uint8_t>> ack(std::uint32_t chunk_index, StatusCode code) {
    wire::DataAckMessage body;
    body.transfer = expected_;
    body.chunk_index = chunk_index;
    body.received_bytes = received_bytes();
    body.code = code;
    Writer writer;
    body.encode(writer);
    if (!writer.ok()) {
      return Result<std::vector<std::uint8_t>>::failure(writer.fail_code(), "ack overflow");
    }
    return Result<std::vector<std::uint8_t>>::success(writer.take());
  }

  Result<std::vector<std::uint8_t>> handle_chunk(ByteSpan payload) {
    Reader reader(payload);
    auto chunk = wire::DataChunkMessage::decode(reader);
    if (!chunk.ok()) return Result<std::vector<std::uint8_t>>::failure(chunk.status());
    const Status finished = reader.finish();
    if (!finished.ok()) return Result<std::vector<std::uint8_t>>::failure(finished);
    {
      std::lock_guard<std::mutex> guard(mutex_);
      if (complete_) {
        return Result<std::vector<std::uint8_t>>::failure(StatusCode::Conflict,
                                                          "the transfer is already complete");
      }
      if (chunk.value().transfer != expected_) {
        failed_ = true;
        reason_ = "chunk belongs to a different transfer";
        condition_.notify_all();
        return Result<std::vector<std::uint8_t>>::failure(StatusCode::NotAuthorized,
                                                          "chunk belongs to a different transfer");
      }
      if (chunk.value().chunk_index != received_chunks_) {
        failed_ = true;
        reason_ = "chunks arrived out of order";
        condition_.notify_all();
        return Result<std::vector<std::uint8_t>>::failure(StatusCode::MalformedInput,
                                                          "chunks arrived out of order");
      }
      crc_.update(chunk.value().data);
      received_bytes_ += static_cast<std::uint64_t>(chunk.value().data.size());
      expected_bytes_ = chunk.value().total_bytes;
      expected_chunks_ = chunk.value().total_chunks;
      received_chunks_ += 1;
      content_crc_ = crc_.value();
    }
    return ack(chunk.value().chunk_index, StatusCode::Ok);
  }

  Result<std::vector<std::uint8_t>> handle_complete(ByteSpan payload) {
    Reader reader(payload);
    auto complete = wire::DataCompleteMessage::decode(reader);
    if (!complete.ok()) return Result<std::vector<std::uint8_t>>::failure(complete.status());
    const Status finished = reader.finish();
    if (!finished.ok()) return Result<std::vector<std::uint8_t>>::failure(finished);
    StatusCode verdict = StatusCode::Ok;
    {
      std::lock_guard<std::mutex> guard(mutex_);
      content_crc_ = crc_.value();
      if (complete.value().transfer != expected_) {
        verified_ = false;
        reason_ = "completion names a different transfer";
        verdict = StatusCode::NotAuthorized;
      } else if (received_bytes_ != complete.value().total_bytes) {
        verified_ = false;
        reason_ = "received byte count does not match the declared total";
        verdict = StatusCode::IntegrityMismatch;
      } else if (received_chunks_ != complete.value().total_chunks) {
        verified_ = false;
        reason_ = "received chunk count does not match the declared total";
        verdict = StatusCode::IntegrityMismatch;
      } else if (content_crc_ != complete.value().content_crc32c) {
        verified_ = false;
        reason_ = "content checksum does not match";
        verdict = StatusCode::IntegrityMismatch;
      } else {
        verified_ = true;
        reason_.clear();
      }
      complete_ = true;
      condition_.notify_all();
    }
    return ack(complete.value().total_chunks, verdict);
  }

  StateTransferId expected_;
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  Crc32c crc_;
  std::uint64_t received_bytes_ = 0;
  std::uint64_t expected_bytes_ = 0;
  std::uint32_t received_chunks_ = 0;
  std::uint32_t expected_chunks_ = 0;
  std::uint32_t content_crc_ = 0;
  bool complete_ = false;
  bool verified_ = false;
  bool failed_ = false;
  std::string reason_;
};

int run_decode(const Options& options) {
  if (options.token.empty()) {
    std::fprintf(stderr, "--token is required\n");
    return kExitUsage;
  }
  StateTransferId expected(0x1111111122222222ULL, 0x3333333344444444ULL);
  if (!options.transfer_hex.empty()) {
    const auto parsed = StateTransferId::parse(options.transfer_hex);
    if (!parsed.has_value()) {
      std::fprintf(stderr, "--transfer must be 32 hexadecimal characters\n");
      return kExitUsage;
    }
    expected = *parsed;
  }
  DataPlaneHandler handler(expected);
  net::ServerConfig server_config;
  server_config.bind_host = "127.0.0.1";
  server_config.port = options.data_port;
  server_config.max_sessions = 4;
  server_config.auth_token = options.token;
  server_config.require_auth = true;
  net::Server server(server_config, &handler);
  const Status started = server.start();
  if (!started.ok()) {
    std::fprintf(stderr, "data plane failed to start: %s\n", started.to_string().c_str());
    return kExitCoordinator;
  }
  std::printf("DATA_PORT %u\n", static_cast<unsigned>(server.bound_port()));
  (void)std::fflush(stdout);

  const bool observed = handler.wait_for_outcome(options.wait_ms);
  server.stop();
  if (!observed) {
    write_result(options, "status=incomplete reason=no transfer arrived");
    return kExitTransferFailed;
  }
  if (!handler.complete() || !handler.verified()) {
    std::string text = "status=incomplete bytes=";
    text.append(std::to_string(handler.received_bytes()));
    text.append(" chunks=");
    text.append(std::to_string(handler.received_chunks()));
    text.append(" reason=");
    text.append(handler.reason().empty() ? "transfer did not verify" : handler.reason());
    write_result(options, text);
    return kExitTransferFailed;
  }
  std::string text = "status=ok bytes=";
  text.append(std::to_string(handler.received_bytes()));
  text.append(" chunks=");
  text.append(std::to_string(handler.received_chunks()));
  text.append(" crc=");
  text.append(std::to_string(handler.content_crc()));
  write_result(options, text);
  return kExitOk;
}

// ---------------------------------------------------------------------------
// Serving peer
// ---------------------------------------------------------------------------

int run_prefill(const Options& options) {
  if (options.token.empty()) {
    std::fprintf(stderr, "--token is required\n");
    return kExitUsage;
  }
  const auto request_id = RequestId::parse(options.request_hex);
  const auto attempt_id = AttemptId::parse(options.attempt_hex);
  if (!request_id.has_value() || !attempt_id.has_value()) {
    std::fprintf(stderr, "--request and --attempt must be 32 hexadecimal characters\n");
    return kExitUsage;
  }
  StateTransferId transfer_id(0x1111111122222222ULL, 0x3333333344444444ULL);
  if (!options.transfer_hex.empty()) {
    const auto parsed = StateTransferId::parse(options.transfer_hex);
    if (!parsed.has_value()) {
      std::fprintf(stderr, "--transfer must be 32 hexadecimal characters\n");
      return kExitUsage;
    }
    transfer_id = *parsed;
  }

  net::ClientConfig client_config;
  client_config.host = options.coordinator_host;
  client_config.port = options.coordinator_port;
  client_config.peer_name = options.name;
  client_config.auth_token = options.token;
  client_config.client_version = std::string(kVersionString);
  auto connected = net::Client::connect(client_config);
  if (!connected.ok()) {
    std::fprintf(stderr, "worker cannot reach the coordinator: %s\n",
                 connected.status().to_string().c_str());
    return kExitCoordinator;
  }
  net::Client& client = connected.value();

  const auto reached = [&](const char* phase) {
    return !options.stop_after.empty() && options.stop_after == phase;
  };

  if (options.publish_topology) {
    TopologyEvidence evidence;
    evidence.provenance = EvidenceProvenance::Synthetic;
    evidence.node_count = 2;
    evidence.link_count = 1;
    evidence.accelerator_count = 1;
    evidence.fabric_bytes_per_sec = 50ULL * 1000ULL * 1000ULL * 1000ULL;
    evidence.max_age_nanos = 30000000000ULL;
    evidence.disaggregated = true;
    (void)client.publish_topology(evidence);
  }
  if (options.init_generations) {
    (void)client.advance_model_generation(options.model_hash, std::nullopt, false);
    (void)client.advance_model_generation(options.state_hash, std::nullopt, false);
  }
  if (options.register_request) {
    (void)client.register_request(*request_id, 5000000000ULL);
    if (reached("register-request")) return kExitOk;
  }
  auto attempt = client.register_attempt(*request_id, *attempt_id, options.model_hash);
  if (!attempt.ok()) {
    std::fprintf(stderr, "register_attempt failed: %s\n", attempt.status().to_string().c_str());
    return kExitCoordinator;
  }
  if (reached("register-attempt")) return kExitOk;

  auto route =
      client.issue_route_decision(*request_id, *attempt_id, 1, true, options.peer_port != 0);
  if (!route.ok()) {
    std::fprintf(stderr, "issue_route_decision failed: %s\n", route.status().to_string().c_str());
    return kExitCoordinator;
  }
  if (reached("route")) return kExitOk;

  auto slo = client.publish_slo_contract(*request_id, 5000000000ULL, 5000000000ULL, 200);
  if (!slo.ok()) {
    std::fprintf(stderr, "publish_slo_contract failed: %s\n", slo.status().to_string().c_str());
    return kExitCoordinator;
  }
  if (reached("slo")) return kExitOk;

  (void)client.advance_stage(*request_id, *attempt_id, ServingStage::Admitted);
  (void)client.advance_stage(*request_id, *attempt_id, ServingStage::PrefillQueued);
  (void)client.advance_stage(*request_id, *attempt_id, ServingStage::PrefillRunning);
  (void)client.advance_stage(*request_id, *attempt_id, ServingStage::HandoffPending);
  if (reached("stage")) return kExitOk;

  TransferRegistration registration;
  registration.id = transfer_id;
  registration.state_hash = options.state_hash;
  registration.model_hash = options.model_hash;
  registration.payload_bytes = options.bytes;
  registration.declared_class = TrafficClass::KvTransfer;
  registration.direction = FlowDirection::Lateral;
  registration.required_integrity = IntegrityClass::ChecksumAndVerify;
  registration.request = *request_id;
  registration.attempt = *attempt_id;
  auto registered = client.register_transfer(registration);
  if (!registered.ok()) {
    std::fprintf(stderr, "register_transfer failed: %s\n", registered.status().to_string().c_str());
    return kExitCoordinator;
  }
  if (reached("register-transfer")) return kExitOk;

  TrafficRequest authorization_request;
  TrafficSubject subject;
  subject.kind = SubjectKind::Transfer;
  subject.transfer = transfer_id;
  authorization_request.subject = subject;
  authorization_request.declared_class = TrafficClass::KvTransfer;
  authorization_request.direction = FlowDirection::Lateral;
  authorization_request.payload_bytes = options.bytes;
  authorization_request.model_hash = options.model_hash;
  authorization_request.binding.policy = client.hello().policy_generation;
  authorization_request.binding.coordinator_epoch = client.hello().epoch;
  authorization_request.binding.model_generation = registered.value().model_generation;
  authorization_request.binding.state_generation = registered.value().state_generation;
  auto authorization = client.authorize_transfer(authorization_request);
  if (!authorization.ok()) {
    std::fprintf(stderr, "authorize_transfer failed: %s\n",
                 authorization.status().to_string().c_str());
    return kExitCoordinator;
  }
  if (!authorization.value().authorization.authorized) {
    std::fprintf(stderr, "transfer was not authorized: %s\n",
                 std::string(to_string(authorization.value().authorization.reason)).c_str());
    write_result(options, "status=refused");
    return kExitTransferFailed;
  }
  if (reached("authorize")) return kExitOk;

  // Real loopback TCP data plane to the peer worker.
  if (options.peer_port == 0) {
    std::fprintf(stderr, "prefill requires --peer-port to stream the transfer\n");
    return kExitUsage;
  }
  net::ClientConfig data_config;
  data_config.host = "127.0.0.1";
  data_config.port = options.peer_port;
  data_config.peer_name = options.name + "-data";
  data_config.auth_token = options.token;
  auto data_client = net::Client::connect(data_config);
  if (!data_client.ok()) {
    std::fprintf(stderr, "data plane connection failed: %s\n",
                 data_client.status().to_string().c_str());
    return kExitCoordinator;
  }

  const std::uint64_t chunk_bytes = options.bytes / options.chunks;
  if (chunk_bytes == 0 || chunk_bytes > wire::kMaxChunkBytes) {
    std::fprintf(stderr, "--bytes/--chunks must be between 1 and %u\n",
                 static_cast<unsigned>(wire::kMaxChunkBytes));
    return kExitUsage;
  }
  Crc32c content_crc;
  std::uint64_t sent_bytes = 0;
  for (std::uint32_t index = 0; index < options.chunks; ++index) {
    std::vector<std::uint8_t> payload(static_cast<std::size_t>(chunk_bytes));
    for (std::size_t offset = 0; offset < payload.size(); ++offset) {
      payload[offset] = static_cast<std::uint8_t>(
          (static_cast<std::uint64_t>(index) * 31ULL + static_cast<std::uint64_t>(offset)) & 0xFFULL);
    }
    content_crc.update(ByteSpan(payload.data(), payload.size()));
    wire::DataChunkMessage chunk;
    chunk.transfer = transfer_id;
    chunk.chunk_index = index;
    chunk.total_chunks = options.chunks;
    chunk.total_bytes = options.bytes;
    chunk.data = ByteSpan(payload.data(), payload.size());
    Writer writer;
    chunk.encode(writer);
    auto response = data_client.value().exchange(
        wire::MessageType::DataChunk, ByteSpan(writer.buffer().data(), writer.buffer().size()));
    if (!response.ok()) {
      std::fprintf(stderr, "chunk %u was refused: %s\n", index,
                   response.status().to_string().c_str());
      write_result(options, "status=peer_refused");
      return kExitTransferFailed;
    }
    sent_bytes += chunk_bytes;
    if (!options.progress_file.empty()) {
      // The marker lets an orchestrator kill the worker while bytes are
      // provably in flight rather than guessing at timing.
      std::ofstream progress(options.progress_file, std::ios::trunc);
      progress << "streaming chunk=" << index << " bytes=" << sent_bytes << "\n";
    }
    if (options.chunk_dwell_ms != 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(options.chunk_dwell_ms));
    }
  }
  {
    wire::DataCompleteMessage completion;
    completion.transfer = transfer_id;
    completion.total_bytes = sent_bytes;
    completion.total_chunks = options.chunks;
    completion.content_crc32c = content_crc.value();
    Writer writer;
    completion.encode(writer);
    auto response = data_client.value().exchange(
        wire::MessageType::DataComplete, ByteSpan(writer.buffer().data(), writer.buffer().size()));
    if (!response.ok()) {
      std::fprintf(stderr, "data completion was refused: %s\n",
                   response.status().to_string().c_str());
      write_result(options, "status=content_mismatch");
      return kExitTransferFailed;
    }
  }
  if (reached("stream")) return kExitOk;

  auto completed = client.advance_stage(*request_id, *attempt_id, ServingStage::DecodeRunning);
  (void)completed;
  auto transfer_complete = [&]() {
    wire::TransferCompleteMessage body;
    body.transfer = transfer_id;
    body.verified_bytes = sent_bytes;
    Writer writer;
    body.encode(writer);
    return client.exchange(wire::MessageType::TransferComplete,
                           ByteSpan(writer.buffer().data(), writer.buffer().size()));
  }();
  if (!transfer_complete.ok()) {
    // The coordinator answered deterministically: the transfer was revoked,
    // fenced, or already terminal. That is a failed transfer, not a failure to
    // reach the coordinator.
    std::fprintf(stderr, "transfer completion was refused: %s\n",
                 transfer_complete.status().to_string().c_str());
    std::string text = "status=refused reason=";
    text.append(transfer_complete.status().message());
    write_result(options, text);
    return kExitTransferFailed;
  }
  if (reached("complete")) return kExitOk;

  CompletionPublication publication;
  publication.request = *request_id;
  publication.attempt = *attempt_id;
  publication.attempt_generation = attempt.value().generation;
  publication.bytes_in = 0;
  publication.bytes_out = sent_bytes;
  auto outcome = client.publish_completion(publication);
  if (!outcome.ok()) {
    std::fprintf(stderr, "completion publication failed: %s\n",
                 outcome.status().to_string().c_str());
    return kExitCoordinator;
  }
  if (outcome.value().refused()) {
    std::string text = "status=refused reason=";
    text.append(to_string(outcome.value().reason));
    write_result(options, text);
    return kExitTransferFailed;
  }
  std::string text = "status=committed disposition=";
  text.append(std::to_string(static_cast<unsigned>(outcome.value().disposition)));
  write_result(options, text);
  return kExitOk;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    print_usage();
    return kExitUsage;
  }
  const std::string_view role(argv[1]);
  Options options;
  if (!parse_options(argc, argv, options)) {
    print_usage();
    return kExitUsage;
  }
  if (options.verbose) Logger::instance().set_level(LogLevel::kInfo);
  if (role == "decode") return run_decode(options);
  if (role == "prefill") return run_prefill(options);
  std::fprintf(stderr, "unknown role: %.*s\n", static_cast<int>(role.size()), role.data());
  print_usage();
  return kExitUsage;
}
