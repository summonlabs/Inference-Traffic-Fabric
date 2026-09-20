// Inference Traffic Fabric - versioned, integrity-checked durable state.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef ITF_PERSISTENCE_HPP
#define ITF_PERSISTENCE_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "itf/codec.hpp"
#include "itf/error.hpp"
#include "itf/evidence.hpp"
#include "itf/ids.hpp"
#include "itf/ledger.hpp"
#include "itf/policy.hpp"
#include "itf/version.hpp"

namespace itf {

/// Logical durable document. It contains only state that belongs to this
/// boundary: authority incarnation, policy, topology evidence, generation
/// registries and bounded request/accounting history.
struct PersistedCoordinatorState {
  std::uint16_t format_version = kSnapshotFormatVersion;
  CoordinatorEpoch epoch;
  BootId previous_incarnation;
  std::uint64_t boot_count = 0;
  PolicyConfig policy;
  bool topology_present = false;
  TopologyEvidence topology;
  std::vector<ModelState> models;
  std::vector<StateGenerationRecord> states;
  std::vector<RequestRecord> terminal_requests;
  std::vector<RequestRecord> interrupted_requests;

  void encode_into(Writer& writer) const;
  [[nodiscard]] static Result<PersistedCoordinatorState> decode_from(Reader& reader);
  [[nodiscard]] std::string to_string() const;
};

/// Framed snapshot file:
///   magic[4]="ITFS" | format_version u16 | flags u16 | payload_len u64 |
///   payload_crc32c u32 | header_crc32c u32 | payload
/// The header CRC covers the fixed header; the payload CRC covers the payload.
class SnapshotStore {
 public:
  SnapshotStore(std::string path, std::size_t max_payload_bytes = kMaxSnapshotPayloadBytes);

  [[nodiscard]] const std::string& path() const noexcept { return path_; }
  [[nodiscard]] std::size_t max_payload_bytes() const noexcept { return max_payload_bytes_; }

  /// Writes through a temporary file, flushes it to stable storage, then
  /// atomically replaces the target. A partially written snapshot is never
  /// visible under the target name.
  Status save(const PersistedCoordinatorState& state);

  /// Missing file yields StatusCode::NotFound. Corrupt, truncated, oversized,
  /// version-incompatible or internally impossible state is refused with a
  /// specific code and no partial application.
  Result<PersistedCoordinatorState> load() const;

  [[nodiscard]] bool exists() const;

  Status save_raw(ByteSpan payload);
  Result<std::vector<std::uint8_t>> load_raw() const;

  static constexpr std::uint8_t kMagic0 = 'I';
  static constexpr std::uint8_t kMagic1 = 'T';
  static constexpr std::uint8_t kMagic2 = 'F';
  static constexpr std::uint8_t kMagic3 = 'S';
  static constexpr std::size_t kHeaderBytes = 24;

 private:
  std::string path_;
  std::size_t max_payload_bytes_;
};

/// Atomically replaces the target path with the source path. Exposed because
/// the recovery tests exercise abrupt death around both sides of this
/// operation.
Status atomic_replace_file(const std::string& source, const std::string& target);

/// Flushes a file's contents to stable storage.
Status sync_file(const std::string& path);

}  // namespace itf

#endif  // ITF_PERSISTENCE_HPP
