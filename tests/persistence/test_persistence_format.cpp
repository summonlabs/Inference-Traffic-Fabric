// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <string>
#include <vector>

#include "itf/crc32c.hpp"
#include "itf/coordinator.hpp"
#include "itf/persistence.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include "support/test.hpp"
#include "support/test_support.hpp"

namespace {

using namespace itf;

void store_u16(std::uint8_t* out, std::uint16_t value) {
  out[0] = static_cast<std::uint8_t>(value & 0xFFU);
  out[1] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
}

void store_u32(std::uint8_t* out, std::uint32_t value) {
  for (int index = 0; index < 4; ++index) {
    out[index] = static_cast<std::uint8_t>((value >> static_cast<unsigned>(index * 8)) & 0xFFU);
  }
}

void store_u64(std::uint8_t* out, std::uint64_t value) {
  for (int index = 0; index < 8; ++index) {
    out[index] = static_cast<std::uint8_t>((value >> static_cast<unsigned>(index * 8)) & 0xFFU);
  }
}

std::vector<std::uint8_t> build_raw_snapshot(ByteSpan payload, std::uint16_t version,
                                             std::uint64_t declared_length, bool corrupt_header_crc,
                                             bool corrupt_payload_crc) {
  std::vector<std::uint8_t> header(SnapshotStore::kHeaderBytes, 0);
  header[0] = SnapshotStore::kMagic0;
  header[1] = SnapshotStore::kMagic1;
  header[2] = SnapshotStore::kMagic2;
  header[3] = SnapshotStore::kMagic3;
  store_u16(header.data() + 4, version);
  store_u16(header.data() + 6, 0);
  store_u64(header.data() + 8, declared_length);
  std::uint32_t payload_crc = Crc32c::compute(payload);
  if (corrupt_payload_crc) payload_crc ^= 0xFFFFFFFFU;
  store_u32(header.data() + 16, payload_crc);
  std::uint32_t header_crc =
      Crc32c::compute(ByteSpan(header.data(), SnapshotStore::kHeaderBytes - sizeof(std::uint32_t)));
  if (corrupt_header_crc) header_crc ^= 0xFFFFFFFFU;
  store_u32(header.data() + 20, header_crc);
  std::vector<std::uint8_t> file = header;
  file.insert(file.end(), payload.begin(), payload.end());
  return file;
}

ITF_TEST(snapshot_round_trip_and_determinism) {
  VirtualClock clock;
  CoordinatorConfig config;
  config.policy = PolicyConfig::canonical_defaults(PolicyGeneration::from(1));
  config.boot_seed = 3;
  Coordinator coordinator(config, &clock);
  (void)coordinator.boot_fresh();
  (void)coordinator.set_model_generation(0xAAULL, std::nullopt);
  (void)coordinator.advance_state_generation(0xBBULL, std::nullopt);

  auto first = coordinator.export_state();
  ITF_REQUIRE(first.ok());
  auto second = coordinator.export_state();
  ITF_REQUIRE(second.ok());
  Writer first_writer;
  Writer second_writer;
  first.value().encode_into(first_writer);
  second.value().encode_into(second_writer);
  ITF_REQUIRE(first_writer.ok());
  ITF_CHECK(first_writer.buffer() == second_writer.buffer());

  Reader reader(ByteSpan(first_writer.buffer().data(), first_writer.buffer().size()));
  const auto decoded = PersistedCoordinatorState::decode_from(reader);
  ITF_REQUIRE(decoded.ok());
  ITF_REQUIRE_STATUS_OK(reader.finish());
  ITF_CHECK_EQ(decoded.value().boot_count, first.value().boot_count);
  ITF_CHECK_EQ(decoded.value().models.size(), static_cast<std::size_t>(1));
  ITF_CHECK_EQ(decoded.value().states.size(), static_cast<std::size_t>(1));
}

ITF_TEST(snapshot_file_is_recoverable_and_atomic) {
  const std::string directory = itf::test::make_temp_dir("persistence-format");
  const std::string path = directory + "/state.bin";
  VirtualClock clock;
  CoordinatorConfig config;
  config.policy = PolicyConfig::canonical_defaults(PolicyGeneration::from(1));
  Coordinator coordinator(config, &clock);
  (void)coordinator.boot_fresh();
  auto state = coordinator.export_state();
  ITF_REQUIRE(state.ok());

  SnapshotStore store(path);
  ITF_CHECK(!store.exists());
  ITF_CHECK_EQ(static_cast<int>(store.load().code()), static_cast<int>(StatusCode::NotFound));
  ITF_REQUIRE_STATUS_OK(store.save(state.value()));
  ITF_CHECK(store.exists());
  const auto loaded = store.load();
  ITF_REQUIRE(loaded.ok());

  // Writing again replaces the document atomically and leaves no debris.
  ITF_REQUIRE_STATUS_OK(store.save(loaded.value()));
  const auto reloaded = store.load();
  ITF_REQUIRE(reloaded.ok());
  const std::uint64_t size = itf::test::file_size(path);
  std::uint64_t total = 0;
#if defined(_WIN32)
  // Temporary files carry a ".tmp." infix next to the target.
  const std::string pattern = directory + "\\*.tmp.*";
  WIN32_FIND_DATAA data{};
  HANDLE handle = FindFirstFileA(pattern.c_str(), &data);
  if (handle != INVALID_HANDLE_VALUE) {
    do {
      ++total;
    } while (FindNextFileA(handle, &data) != 0);
    (void)FindClose(handle);
  }
#endif
  ITF_CHECK_EQ(total, 0ULL);
  ITF_CHECK(size > SnapshotStore::kHeaderBytes);
  itf::test::remove_dir(directory);
}

ITF_TEST(header_defects_are_refused_with_specific_codes) {
  const std::string directory = itf::test::make_temp_dir("persistence-header");
  const std::string path = directory + "/state.bin";
  VirtualClock clock;
  CoordinatorConfig config;
  config.policy = PolicyConfig::canonical_defaults(PolicyGeneration::from(1));
  Coordinator coordinator(config, &clock);
  (void)coordinator.boot_fresh();
  auto state = coordinator.export_state();
  ITF_REQUIRE(state.ok());
  Writer writer;
  state.value().encode_into(writer);
  ITF_REQUIRE(writer.ok());
  const std::vector<std::uint8_t> payload = writer.buffer();

  const auto write_case = [&](const std::vector<std::uint8_t>& bytes) {
    itf::test::write_text_file(path, std::string(bytes.begin(), bytes.end()));
  };

  {
    write_case(build_raw_snapshot(ByteSpan(payload.data(), payload.size()), 99,
                                  payload.size(), false, false));
    ITF_CHECK_EQ(static_cast<int>(SnapshotStore(path).load().code()),
                 static_cast<int>(StatusCode::UnsupportedVersion));
  }
  {
    write_case(build_raw_snapshot(ByteSpan(payload.data(), payload.size()),
                                  kSnapshotFormatVersion, payload.size(), true, false));
    ITF_CHECK_EQ(static_cast<int>(SnapshotStore(path).load().code()),
                 static_cast<int>(StatusCode::IntegrityMismatch));
  }
  {
    write_case(build_raw_snapshot(ByteSpan(payload.data(), payload.size()),
                                  kSnapshotFormatVersion, payload.size(), false, true));
    ITF_CHECK_EQ(static_cast<int>(SnapshotStore(path).load().code()),
                 static_cast<int>(StatusCode::IntegrityMismatch));
  }
  {
    write_case(build_raw_snapshot(ByteSpan(payload.data(), payload.size()),
                                  kSnapshotFormatVersion, 4096, false, false));
    ITF_CHECK_EQ(static_cast<int>(SnapshotStore(path).load().code()),
                 static_cast<int>(StatusCode::MalformedInput));
  }
  {
    auto bytes = build_raw_snapshot(ByteSpan(payload.data(), payload.size()),
                                    kSnapshotFormatVersion, payload.size(), false, false);
    bytes.push_back(0x00);
    write_case(bytes);
    ITF_CHECK_EQ(static_cast<int>(SnapshotStore(path).load().code()),
                 static_cast<int>(StatusCode::TrailingGarbage));
  }
  {
    auto bytes = build_raw_snapshot(ByteSpan(payload.data(), payload.size()),
                                    kSnapshotFormatVersion, payload.size(), false, false);
    bytes.resize(SnapshotStore::kHeaderBytes - 4);
    write_case(bytes);
    ITF_CHECK_EQ(static_cast<int>(SnapshotStore(path).load().code()),
                 static_cast<int>(StatusCode::MalformedInput));
  }
  {
    auto bytes = build_raw_snapshot(ByteSpan(payload.data(), payload.size()),
                                    kSnapshotFormatVersion, payload.size(), false, false);
    bytes.resize(bytes.size() / 2);
    write_case(bytes);
    ITF_CHECK_EQ(static_cast<int>(SnapshotStore(path).load().code()),
                 static_cast<int>(StatusCode::MalformedInput));
  }
  {
    write_case(build_raw_snapshot(ByteSpan(payload.data(), payload.size()),
                                  kSnapshotFormatVersion, UINT64_MAX, false, false));
    ITF_CHECK_EQ(static_cast<int>(SnapshotStore(path).load().code()),
                 static_cast<int>(StatusCode::BoundsExceeded));
  }
  {
    std::vector<std::uint8_t> empty;
    write_case(empty);
    ITF_CHECK_EQ(static_cast<int>(SnapshotStore(path).load().code()),
                 static_cast<int>(StatusCode::MalformedInput));
  }
  itf::test::remove_dir(directory);
}

ITF_TEST(impossible_documents_are_refused) {
  VirtualClock clock;
  CoordinatorConfig config;
  config.policy = PolicyConfig::canonical_defaults(PolicyGeneration::from(1));
  Coordinator coordinator(config, &clock);
  (void)coordinator.boot_fresh();
  auto state = coordinator.export_state();
  ITF_REQUIRE(state.ok());

  {
    // A live record cannot appear in the terminal history.
    PersistedCoordinatorState hostile = state.value();
    RequestRecord record;
    record.id = itf::test::make_request_id(81, 1);
    record.generation = RequestGeneration::from(1);
    record.lifecycle = RequestLifecycle::Active;
    hostile.terminal_requests.push_back(record);
    Writer writer;
    hostile.encode_into(writer);
    ITF_REQUIRE(writer.ok());
    Reader reader(ByteSpan(writer.buffer().data(), writer.buffer().size()));
    const auto decoded = PersistedCoordinatorState::decode_from(reader);
    ITF_CHECK(!decoded.ok());
    ITF_CHECK_EQ(static_cast<int>(decoded.code()), static_cast<int>(StatusCode::MalformedInput));
  }
  {
    // A terminal record may not claim active flows.
    PersistedCoordinatorState hostile = state.value();
    RequestRecord record;
    record.id = itf::test::make_request_id(82, 1);
    record.generation = RequestGeneration::from(1);
    record.lifecycle = RequestLifecycle::Completed;
    record.active_flows = 2;
    hostile.terminal_requests.push_back(record);
    Writer writer;
    hostile.encode_into(writer);
    ITF_REQUIRE(writer.ok());
    Reader reader(ByteSpan(writer.buffer().data(), writer.buffer().size()));
    const auto decoded = PersistedCoordinatorState::decode_from(reader);
    ITF_CHECK(!decoded.ok());
    ITF_CHECK_EQ(static_cast<int>(decoded.code()), static_cast<int>(StatusCode::MalformedInput));
  }
  {
    // The attempt count may not exceed the compile-time ceiling.
    PersistedCoordinatorState hostile = state.value();
    RequestRecord record;
    record.id = itf::test::make_request_id(83, 1);
    record.generation = RequestGeneration::from(1);
    record.lifecycle = RequestLifecycle::Completed;
    record.attempt_count = kMaxAttemptsPerRequest + 1;
    hostile.terminal_requests.push_back(record);
    Writer writer;
    hostile.encode_into(writer);
    ITF_REQUIRE(writer.ok());
    Reader reader(ByteSpan(writer.buffer().data(), writer.buffer().size()));
    const auto decoded = PersistedCoordinatorState::decode_from(reader);
    ITF_CHECK(!decoded.ok());
  }
}

}  // namespace

int main(int argc, char** argv) {
  return itf::test::run_all("test_persistence_format", argc, argv);
}
