// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <string>
#include <vector>

#include "itf/coordinator.hpp"
#include "support/cluster.hpp"
#include "support/test.hpp"
#include "support/test_support.hpp"

namespace {

using namespace itf;

constexpr std::uint64_t kModelHash = 0x1111222233334444ULL;
constexpr std::uint64_t kStateHash = 0x5555666677778888ULL;
const std::string kTransferHex = "11111111222222223333333344444444";
constexpr std::uint64_t kPayloadBytes = 1U << 20;
constexpr std::uint32_t kChunks = 16;

/// Starts a decode worker that owns the data plane for the transfer.
itf::test::ChildProcess start_decode(const itf::test::Cluster& cluster, std::uint16_t data_port,
                                     const std::string& result_path, std::uint64_t wait_ms) {
  auto spawned = itf::test::ChildProcess::spawn(
      itf::test::tool_path("itf_sim"),
      {"decode", "--token", cluster.token(), "--data-port", std::to_string(data_port), "--transfer",
       kTransferHex, "--result-file", result_path, "--wait-ms", std::to_string(wait_ms), "--name",
       "decode-1"});
  return spawned.ok() ? std::move(spawned).value() : itf::test::ChildProcess{};
}

std::vector<std::string> prefill_arguments(const itf::test::Cluster& cluster,
                                           const std::string& request_hex,
                                           const std::string& attempt_hex, std::uint16_t peer_port,
                                           const std::string& result_path,
                                           std::uint64_t chunk_dwell_ms,
                                           const std::string& progress_path = std::string()) {
  std::vector<std::string> progress;
  if (!progress_path.empty()) {
    progress.push_back("--progress-file");
    progress.push_back(progress_path);
  }
  std::vector<std::string> arguments = {"prefill",
          "--coordinator",
          "127.0.0.1:" + std::to_string(cluster.port()),
          "--token",
          cluster.token(),
          "--name",
          "prefill-1",
          "--request",
          request_hex,
          "--attempt",
          attempt_hex,
          "--transfer",
          kTransferHex,
          "--model-hash",
          std::to_string(kModelHash),
          "--state-hash",
          std::to_string(kStateHash),
          "--peer-port",
          std::to_string(peer_port),
          "--bytes",
          std::to_string(kPayloadBytes),
          "--chunks",
          std::to_string(kChunks),
          "--chunk-dwell-ms",
          std::to_string(chunk_dwell_ms),
          "--register-request",
          "--result-file",
          result_path};
  arguments.insert(arguments.end(), progress.begin(), progress.end());
  return arguments;
}

ITF_TEST(a_real_kv_transfer_completes_across_processes) {
  itf::test::Cluster cluster;
  ITF_REQUIRE_STATUS_OK(cluster.start());
  auto driver = cluster.connect("driver");
  ITF_REQUIRE(driver.ok());
  ITF_REQUIRE_STATUS_OK(itf::test::prepare_fabric(driver.value(), kModelHash, kStateHash));

  const RequestId request = itf::test::make_request_id(101, 1);
  const AttemptId attempt = itf::test::make_attempt_id(101, 1);
  const std::uint16_t data_port = itf::test::find_free_port();
  ITF_REQUIRE(data_port != 0);

  const std::string decode_result = cluster.result_path("decode-happy");
  const std::string prefill_result = cluster.result_path("prefill-happy");
  auto decode = start_decode(cluster, data_port, decode_result, 20000);
  ITF_REQUIRE(decode.valid());
  ITF_CHECK(itf::test::wait_until(
      [&] {
        auto probe = net::TcpStream::connect("127.0.0.1", data_port, 300000000LL);
        if (!probe.ok()) return false;
        probe.value().close();
        return true;
      },
      15000));

  auto prefill = itf::test::ChildProcess::spawn(
      itf::test::tool_path("itf_sim"),
      prefill_arguments(cluster, request.to_string(), attempt.to_string(), data_port, prefill_result,
                        0));
  ITF_REQUIRE(prefill.ok());

  const int prefill_code = prefill.value().wait();
  const int decode_code = decode.wait();
  std::printf("  prefill exit=%d result=%s\n", prefill_code,
              itf::test::read_text_file(prefill_result).c_str());
  std::printf("  decode exit=%d result=%s\n", decode_code,
              itf::test::read_text_file(decode_result).c_str());
  ITF_CHECK_EQ(prefill_code, 0);
  ITF_CHECK_EQ(decode_code, 0);
  ITF_CHECK(itf::test::read_text_file(decode_result).find("status=ok") == 0);
  ITF_CHECK(itf::test::read_text_file(decode_result).find("bytes=1048576") != std::string::npos);

  const auto snapshot = driver.value().accounting();
  ITF_REQUIRE(snapshot.ok());
  ITF_CHECK_EQ(snapshot.value().transfers_started, 1ULL);
  ITF_CHECK_EQ(snapshot.value().transfers_completed, 1ULL);
  ITF_CHECK_EQ(snapshot.value().requests_completed, 1ULL);
  ITF_CHECK_EQ(snapshot.value().active_flows_total, 0U);
  ITF_CHECK(snapshot.value().closed());
}

ITF_TEST(cancellation_after_a_real_transfer_refuses_the_commit) {
  itf::test::Cluster cluster;
  ITF_REQUIRE_STATUS_OK(cluster.start());
  auto driver = cluster.connect("driver");
  ITF_REQUIRE(driver.ok());
  ITF_REQUIRE_STATUS_OK(itf::test::prepare_fabric(driver.value(), kModelHash, kStateHash));

  const RequestId request = itf::test::make_request_id(102, 1);
  const AttemptId attempt = itf::test::make_attempt_id(102, 1);
  const std::uint16_t data_port = itf::test::find_free_port();
  ITF_REQUIRE(data_port != 0);
  const std::string decode_result = cluster.result_path("decode-cancel");
  const std::string prefill_result = cluster.result_path("prefill-cancel");
  auto decode = start_decode(cluster, data_port, decode_result, 20000);
  ITF_REQUIRE(decode.valid());
  ITF_CHECK(itf::test::wait_until(
      [&] {
        auto probe = net::TcpStream::connect("127.0.0.1", data_port, 300000000LL);
        if (!probe.ok()) return false;
        probe.value().close();
        return true;
      },
      15000));

  auto prefill = itf::test::ChildProcess::spawn(
      itf::test::tool_path("itf_sim"),
      prefill_arguments(cluster, request.to_string(), attempt.to_string(), data_port, prefill_result,
                        20));
  ITF_REQUIRE(prefill.ok());

  // Cancel once the transfer has been authorized and the bytes are moving.
  ITF_CHECK(itf::test::wait_until(
      [&] {
        auto record = driver.value().accounting();
        return record.ok() && record.value().transfers_started >= 1ULL;
      },
      15000));
  const auto cancelled = driver.value().cancel_request(request, ReasonCode::Cancelled);
  ITF_REQUIRE(cancelled.ok());

  const int prefill_code = prefill.value().wait();
  const int decode_code = decode.wait();
  std::printf("  prefill exit=%d result=%s\n", prefill_code,
              itf::test::read_text_file(prefill_result).c_str());
  std::printf("  decode exit=%d result=%s\n", decode_code,
              itf::test::read_text_file(decode_result).c_str());
  // The payload still moved: cancellation removes authority, not physics.
  ITF_CHECK_EQ(decode_code, 0);
  ITF_CHECK_EQ(prefill_code, 3);
  ITF_CHECK(itf::test::read_text_file(prefill_result).find("status=refused") == 0);

  // The cancellation revoked the transfer's authority, so the commit is
  // refused even on behalf of the party that moved the bytes.
  CompletionPublication late;
  late.request = request;
  late.attempt = attempt;
  late.attempt_generation = AttemptGeneration::from(1);
  late.bytes_out = kPayloadBytes;
  const auto refused = driver.value().publish_completion(late);
  ITF_REQUIRE(refused.ok());
  ITF_CHECK(refused.value().refused());

  const auto snapshot = driver.value().accounting();
  ITF_REQUIRE(snapshot.ok());
  ITF_CHECK_EQ(snapshot.value().requests_completed, 0ULL);
  ITF_CHECK_EQ(snapshot.value().requests_cancelled, 1ULL);
  ITF_CHECK_EQ(snapshot.value().transfers_completed, 0ULL);
  ITF_CHECK(snapshot.value().completions_refused >= 1ULL);
  ITF_CHECK_EQ(snapshot.value().active_flows_total, 0U);
  ITF_CHECK(snapshot.value().closed());
}

ITF_TEST(a_dead_prefill_worker_cannot_publish_success_later) {
  itf::test::Cluster cluster;
  ITF_REQUIRE_STATUS_OK(cluster.start());
  auto driver = cluster.connect("driver");
  ITF_REQUIRE(driver.ok());
  ITF_REQUIRE_STATUS_OK(itf::test::prepare_fabric(driver.value(), kModelHash, kStateHash));

  const RequestId request = itf::test::make_request_id(103, 1);
  const AttemptId attempt = itf::test::make_attempt_id(103, 1);
  const std::uint16_t data_port = itf::test::find_free_port();
  ITF_REQUIRE(data_port != 0);
  const std::string decode_result = cluster.result_path("decode-death");
  const std::string prefill_result = cluster.result_path("prefill-death");
  auto decode = start_decode(cluster, data_port, decode_result, 20000);
  ITF_REQUIRE(decode.valid());
  ITF_CHECK(itf::test::wait_until(
      [&] {
        auto probe = net::TcpStream::connect("127.0.0.1", data_port, 300000000LL);
        if (!probe.ok()) return false;
        probe.value().close();
        return true;
      },
      15000));

  const std::string progress = cluster.result_path("prefill-death-progress");
  auto prefill = itf::test::ChildProcess::spawn(
      itf::test::tool_path("itf_sim"),
      prefill_arguments(cluster, request.to_string(), attempt.to_string(), data_port, prefill_result,
                        60, progress));
  ITF_REQUIRE(prefill.ok());

  // Wait until the worker reports bytes in flight, then kill it mid-stream.
  ITF_CHECK(itf::test::wait_until([&] { return itf::test::file_exists(progress); }, 20000));
  prefill.value().kill();
  ITF_CHECK(!prefill.value().running());

  const int decode_code = decode.wait();
  std::printf("  decode exit=%d result=%s\n", decode_code,
              itf::test::read_text_file(decode_result).c_str());
  ITF_CHECK_EQ(decode_code, 3);
  ITF_CHECK(itf::test::read_text_file(decode_result).find("status=incomplete") == 0);

  const auto record_before = driver.value().status();
  ITF_REQUIRE(record_before.ok());
  const auto snapshot_before = driver.value().accounting();
  ITF_REQUIRE(snapshot_before.ok());
  ITF_CHECK_EQ(snapshot_before.value().transfers_completed, 0ULL);
  ITF_CHECK_EQ(snapshot_before.value().requests_completed, 0ULL);

  // The owner observes the death and retires the attempt.
  const auto failed = driver.value().fail_attempt(request, attempt, ReasonCode::AttemptFailed);
  ITF_REQUIRE(failed.ok());

  CompletionPublication late;
  late.request = request;
  late.attempt = attempt;
  late.attempt_generation = AttemptGeneration::from(1);
  late.bytes_in = kPayloadBytes;
  const auto refused = driver.value().publish_completion(late);
  ITF_REQUIRE(refused.ok());
  ITF_CHECK(refused.value().refused());
  ITF_CHECK(refused.value().reason == ReasonCode::RejectedRequestTerminal ||
             refused.value().reason == ReasonCode::AttemptFailed ||
             refused.value().reason == ReasonCode::RejectedStaleAttempt);

  // A failed request is terminal: replacing its attempt is refused, and a
  // retry must use a fresh request identity.
  const AttemptId replacement = itf::test::make_attempt_id(103, 2);
  const auto bound = driver.value().replace_attempt(request, attempt, replacement, kModelHash);
  ITF_CHECK(!bound.ok());

  const auto final_snapshot = driver.value().accounting();
  ITF_REQUIRE(final_snapshot.ok());
  ITF_CHECK_EQ(final_snapshot.value().requests_failed, 1ULL);
  ITF_CHECK_EQ(final_snapshot.value().requests_completed, 0ULL);
  ITF_CHECK_EQ(final_snapshot.value().active_flows_total, 0U);
  ITF_CHECK(final_snapshot.value().closed());
}

ITF_TEST(a_live_decode_attempt_can_be_replaced_and_the_old_one_is_fenced) {
  itf::test::Cluster cluster;
  ITF_REQUIRE_STATUS_OK(cluster.start());
  auto driver = cluster.connect("driver");
  ITF_REQUIRE(driver.ok());
  ITF_REQUIRE_STATUS_OK(itf::test::prepare_fabric(driver.value(), kModelHash, kStateHash));

  const RequestId request = itf::test::make_request_id(104, 1);
  const AttemptId first = itf::test::make_attempt_id(104, 1);
  const AttemptId second = itf::test::make_attempt_id(104, 2);
  ITF_REQUIRE(driver.value().register_request(request, 0).ok());
  ITF_REQUIRE(driver.value().register_attempt(request, first, kModelHash).ok());
  ITF_REQUIRE(driver.value().issue_route_decision(request, first, 1, true, true).ok());
  ITF_REQUIRE(driver.value().publish_slo_contract(request, 5000000000ULL, 5000000000ULL, 200).ok());
  ITF_REQUIRE(driver.value().advance_stage(request, first, ServingStage::Admitted).ok());

  const auto replacement = driver.value().replace_attempt(request, first, second, kModelHash);
  ITF_REQUIRE(replacement.ok());
  ITF_CHECK_EQ(replacement.value().generation.value(), 2ULL);
  ITF_REQUIRE(driver.value().issue_route_decision(request, second, 1, true, true).ok());
  ITF_REQUIRE(driver.value().publish_slo_contract(request, 5000000000ULL, 5000000000ULL, 200).ok());

  // Traffic bound to the retired attempt is refused by identity, not by name.
  TrafficRequest stale;
  TrafficSubject stale_subject;
  stale_subject.kind = SubjectKind::Serving;
  stale_subject.request = request;
  stale_subject.attempt = first;
  stale_subject.request_generation = RequestGeneration::from(1);
  stale.subject = stale_subject;
  stale.declared_class = TrafficClass::Control;
  stale.declared_stage = ServingStage::Admitted;
  stale.payload_bytes = 128;
  stale.binding.policy = driver.value().hello().policy_generation;
  stale.binding.coordinator_epoch = driver.value().hello().epoch;
  const auto stale_decision = driver.value().evaluate(stale);
  ITF_REQUIRE(stale_decision.ok());
  ITF_CHECK_EQ(static_cast<int>(stale_decision.value().outcome),
               static_cast<int>(DecisionOutcome::Rejected));
  ITF_CHECK(stale_decision.value().is_authority_denial());

  // The replacement attempt has to establish its own serving stage.
  ITF_REQUIRE(driver.value().advance_stage(request, second, ServingStage::Admitted).ok());
  TrafficRequest fresh = stale;
  fresh.subject->attempt = second;
  // A replacement attempt advances the request generation.
  fresh.subject->request_generation = RequestGeneration::from(2);
  const auto fresh_decision = driver.value().evaluate(fresh);
  ITF_REQUIRE(fresh_decision.ok());
  ITF_CHECK(fresh_decision.value().permits_traffic());

  CompletionPublication publication;
  publication.request = request;
  publication.attempt = second;
  publication.attempt_generation = replacement.value().generation;
  publication.bytes_out = 4096;
  const auto outcome = driver.value().publish_completion(publication);
  ITF_REQUIRE(outcome.ok());
  ITF_CHECK(outcome.value().committed());

  const auto snapshot = driver.value().accounting();
  ITF_REQUIRE(snapshot.ok());
  ITF_CHECK_EQ(snapshot.value().attempts_superseded, 1ULL);
  ITF_CHECK_EQ(snapshot.value().requests_completed, 1ULL);
  ITF_CHECK_EQ(snapshot.value().active_flows_total, 0U);
  ITF_CHECK(snapshot.value().closed());
}

}  // namespace

int main(int argc, char** argv) {
  return itf::test::run_all("test_multiprocess_serving", argc, argv);
}
