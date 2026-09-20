// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "itf/persistence.hpp"

#include <atomic>
#include <string>
#include <utility>

#include "itf/crc32c.hpp"
#include "itf/serialize.hpp"
#include "itf/version.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <process.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace itf {
namespace {

std::uint64_t current_process_id() noexcept {
#if defined(_WIN32)
  return static_cast<std::uint64_t>(_getpid());
#else
  return static_cast<std::uint64_t>(getpid());
#endif
}

std::uint64_t next_temporary_serial() noexcept {
  static std::atomic<std::uint64_t> serial{0};
  return serial.fetch_add(1) + 1;
}

void store_u16(std::uint8_t* out, std::uint16_t value) noexcept {
  out[0] = static_cast<std::uint8_t>(value & 0xFFU);
  out[1] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
}

void store_u32(std::uint8_t* out, std::uint32_t value) noexcept {
  for (int index = 0; index < 4; ++index) {
    out[index] = static_cast<std::uint8_t>((value >> static_cast<unsigned>(index * 8)) & 0xFFU);
  }
}

void store_u64(std::uint8_t* out, std::uint64_t value) noexcept {
  for (int index = 0; index < 8; ++index) {
    out[index] = static_cast<std::uint8_t>((value >> static_cast<unsigned>(index * 8)) & 0xFFU);
  }
}

std::uint16_t load_u16(const std::uint8_t* in) noexcept {
  return static_cast<std::uint16_t>(
      static_cast<std::uint16_t>(in[0]) |
      static_cast<std::uint16_t>(static_cast<std::uint16_t>(in[1]) << 8U));
}

std::uint32_t load_u32(const std::uint8_t* in) noexcept {
  std::uint32_t value = 0;
  for (int index = 0; index < 4; ++index) {
    value |= static_cast<std::uint32_t>(in[index]) << static_cast<unsigned>(index * 8);
  }
  return value;
}

std::uint64_t load_u64(const std::uint8_t* in) noexcept {
  std::uint64_t value = 0;
  for (int index = 0; index < 8; ++index) {
    value |= static_cast<std::uint64_t>(in[index]) << static_cast<unsigned>(index * 8);
  }
  return value;
}

/// Minimal OS file handle. The runtime deliberately uses the platform file
/// API rather than the C stdio layer so that durability (FlushFileBuffers /
/// fsync) is exactly what the snapshot semantics claim.
class FileHandle {
 public:
  FileHandle() = default;
  ~FileHandle() { close(); }
  FileHandle(FileHandle&& other) noexcept { adopt(other); }
  FileHandle& operator=(FileHandle&& other) noexcept {
    if (this != &other) {
      close();
      adopt(other);
    }
    return *this;
  }
  FileHandle(const FileHandle&) = delete;
  FileHandle& operator=(const FileHandle&) = delete;

  [[nodiscard]] static Result<FileHandle> open_existing(const std::string& path) {
    FileHandle handle;
#if defined(_WIN32)
    handle.handle_ = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle.handle_ == INVALID_HANDLE_VALUE) {
      handle.handle_ = nullptr;
      return Result<FileHandle>::failure(StatusCode::NotFound, "cannot open file for reading");
    }
#else
    handle.fd_ = ::open(path.c_str(), O_RDONLY);
    if (handle.fd_ < 0) {
      return Result<FileHandle>::failure(StatusCode::NotFound, "cannot open file for reading");
    }
#endif
    return Result<FileHandle>::success(std::move(handle));
  }

  [[nodiscard]] static Result<FileHandle> create_truncate(const std::string& path) {
    FileHandle handle;
#if defined(_WIN32)
    handle.handle_ = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle.handle_ == INVALID_HANDLE_VALUE) {
      handle.handle_ = nullptr;
      return Result<FileHandle>::failure(StatusCode::IoError, "cannot create file");
    }
#else
    handle.fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (handle.fd_ < 0) {
      return Result<FileHandle>::failure(StatusCode::IoError, "cannot create file");
    }
#endif
    return Result<FileHandle>::success(std::move(handle));
  }

  [[nodiscard]] bool valid() const noexcept {
#if defined(_WIN32)
    return handle_ != nullptr;
#else
    return fd_ >= 0;
#endif
  }

  Status write_all(ByteSpan data) {
    std::size_t offset = 0;
    while (offset < data.size()) {
#if defined(_WIN32)
      DWORD written = 0;
      const DWORD chunk = static_cast<DWORD>(
          data.size() - offset > 0xFFFFFFFFULL ? 0xFFFFFFFFULL : data.size() - offset);
      if (WriteFile(handle_, data.data() + offset, chunk, &written, nullptr) == 0) {
        return Status(StatusCode::IoError, "write failed");
      }
      if (written == 0) return Status(StatusCode::IoError, "write made no progress");
      offset += static_cast<std::size_t>(written);
#else
      const ssize_t written = ::write(fd_, data.data() + offset, data.size() - offset);
      if (written <= 0) return Status(StatusCode::IoError, "write failed");
      offset += static_cast<std::size_t>(written);
#endif
    }
    return Status::success();
  }

  Status read_exact(MutableByteSpan buffer) {
    std::size_t offset = 0;
    while (offset < buffer.size()) {
#if defined(_WIN32)
      DWORD read = 0;
      const DWORD chunk = static_cast<DWORD>(
          buffer.size() - offset > 0xFFFFFFFFULL ? 0xFFFFFFFFULL : buffer.size() - offset);
      if (ReadFile(handle_, buffer.data() + offset, chunk, &read, nullptr) == 0) {
        return Status(StatusCode::IoError, "read failed");
      }
      if (read == 0) return Status(StatusCode::MalformedInput, "file ended early");
      offset += static_cast<std::size_t>(read);
#else
      const ssize_t read = ::read(fd_, buffer.data() + offset, buffer.size() - offset);
      if (read < 0) return Status(StatusCode::IoError, "read failed");
      if (read == 0) return Status(StatusCode::MalformedInput, "file ended early");
      offset += static_cast<std::size_t>(read);
#endif
    }
    return Status::success();
  }

  /// True when the stream still holds at least one byte.
  Result<bool> has_more() {
    std::uint8_t probe = 0;
#if defined(_WIN32)
    DWORD read = 0;
    if (ReadFile(handle_, &probe, 1, &read, nullptr) == 0) {
      return Result<bool>::failure(StatusCode::IoError, "read failed");
    }
    return Result<bool>::success(read == 1);
#else
    const ssize_t read = ::read(fd_, &probe, 1);
    if (read < 0) return Result<bool>::failure(StatusCode::IoError, "read failed");
    return Result<bool>::success(read == 1);
#endif
  }

  Status sync() {
#if defined(_WIN32)
    if (FlushFileBuffers(handle_) == 0) {
      return Status(StatusCode::IoError, "flush to stable storage failed");
    }
    return Status::success();
#else
    if (::fsync(fd_) != 0) {
      return Status(StatusCode::IoError, "fsync failed");
    }
    return Status::success();
#endif
  }

  void close() noexcept {
#if defined(_WIN32)
    if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
      (void)CloseHandle(handle_);
    }
    handle_ = nullptr;
#else
    if (fd_ >= 0) {
      (void)::close(fd_);
      fd_ = -1;
    }
#endif
  }

 private:
  void adopt(FileHandle& other) noexcept {
#if defined(_WIN32)
    handle_ = other.handle_;
    other.handle_ = nullptr;
#else
    fd_ = other.fd_;
    other.fd_ = -1;
#endif
  }

#if defined(_WIN32)
  void* handle_ = nullptr;
#else
  int fd_ = -1;
#endif
};

}  // namespace

void PersistedCoordinatorState::encode_into(Writer& writer) const {
  writer.u16(format_version);
  writer.generation(epoch);
  writer.id(previous_incarnation);
  writer.u64(boot_count);
  policy.encode(writer);
  writer.boolean(topology_present);
  encode(writer, topology);
  writer.u32(static_cast<std::uint32_t>(models.size()));
  for (const ModelState& entry : models) encode(writer, entry);
  writer.u32(static_cast<std::uint32_t>(states.size()));
  for (const StateGenerationRecord& entry : states) encode(writer, entry);
  writer.u32(static_cast<std::uint32_t>(terminal_requests.size()));
  for (const RequestRecord& entry : terminal_requests) encode(writer, entry);
  writer.u32(static_cast<std::uint32_t>(interrupted_requests.size()));
  for (const RequestRecord& entry : interrupted_requests) encode(writer, entry);
}

Result<PersistedCoordinatorState> PersistedCoordinatorState::decode_from(Reader& reader) {
  PersistedCoordinatorState state;
  state.format_version = reader.u16();
  if (!reader.ok()) {
    return Result<PersistedCoordinatorState>::failure(reader.fail_code(), "snapshot header");
  }
  if (state.format_version != kSnapshotFormatVersion) {
    return Result<PersistedCoordinatorState>::failure(
        StatusCode::UnsupportedVersion,
        "snapshot format version " + std::to_string(state.format_version) +
            " is not supported by this build");
  }
  state.epoch = reader.generation<CoordinatorEpochTag>();
  state.previous_incarnation = reader.id<BootIdTag>();
  state.boot_count = reader.u64();
  auto policy_result = PolicyConfig::decode(reader);
  if (!policy_result.ok()) return Result<PersistedCoordinatorState>::failure(policy_result.status());
  state.policy = std::move(policy_result).value();
  state.topology_present = reader.boolean();
  auto topology = decode_topology_evidence(reader, state.topology_present);
  if (!topology.ok()) return Result<PersistedCoordinatorState>::failure(topology.status());
  state.topology = std::move(topology).value();
  const std::uint32_t model_count = reader.count(65536U);
  if (!reader.ok()) return Result<PersistedCoordinatorState>::failure(reader.fail_code(), "models");
  state.models.reserve(model_count);
  for (std::uint32_t index = 0; index < model_count; ++index) {
    auto entry = decode_model_state(reader);
    if (!entry.ok()) return Result<PersistedCoordinatorState>::failure(entry.status());
    state.models.push_back(std::move(entry).value());
  }
  const std::uint32_t state_count = reader.count(65536U);
  if (!reader.ok()) return Result<PersistedCoordinatorState>::failure(reader.fail_code(), "states");
  state.states.reserve(state_count);
  for (std::uint32_t index = 0; index < state_count; ++index) {
    auto entry = decode_state_generation_record(reader);
    if (!entry.ok()) return Result<PersistedCoordinatorState>::failure(entry.status());
    state.states.push_back(std::move(entry).value());
  }
  const std::uint32_t terminal_count = reader.count(65536U);
  if (!reader.ok()) {
    return Result<PersistedCoordinatorState>::failure(reader.fail_code(), "terminal records");
  }
  state.terminal_requests.reserve(terminal_count);
  for (std::uint32_t index = 0; index < terminal_count; ++index) {
    auto entry = decode_request_record(reader);
    if (!entry.ok()) return Result<PersistedCoordinatorState>::failure(entry.status());
    RequestRecord record = std::move(entry).value();
    if (!is_terminal(record.lifecycle)) {
      return Result<PersistedCoordinatorState>::failure(
          StatusCode::MalformedInput, "terminal history contains a non-terminal record");
    }
    if (record.active_flows != 0) {
      return Result<PersistedCoordinatorState>::failure(
          StatusCode::MalformedInput, "terminal record retains active flows");
    }
    state.terminal_requests.push_back(record);
  }
  const std::uint32_t interrupted_count = reader.count(65536U);
  if (!reader.ok()) {
    return Result<PersistedCoordinatorState>::failure(reader.fail_code(), "interrupted records");
  }
  state.interrupted_requests.reserve(interrupted_count);
  for (std::uint32_t index = 0; index < interrupted_count; ++index) {
    auto entry = decode_request_record(reader);
    if (!entry.ok()) return Result<PersistedCoordinatorState>::failure(entry.status());
    state.interrupted_requests.push_back(std::move(entry).value());
  }
  if (!reader.ok()) {
    return Result<PersistedCoordinatorState>::failure(reader.fail_code(), "interrupted records");
  }
  return Result<PersistedCoordinatorState>::success(std::move(state));
}

std::string PersistedCoordinatorState::to_string() const {
  std::string out;
  out.reserve(256);
  out.append("snapshot format=");
  out.append(std::to_string(format_version));
  out.append(" epoch=");
  out.append(std::to_string(epoch.value()));
  out.append(" boot_count=");
  out.append(std::to_string(boot_count));
  out.append(" previous_incarnation=");
  out.append(previous_incarnation.to_string());
  out.append(" policy_generation=");
  out.append(std::to_string(policy.generation().value()));
  out.append(" topology_present=");
  out.append(topology_present ? "true" : "false");
  if (topology_present) {
    out.append(" topology_generation=");
    out.append(std::to_string(topology.generation.value()));
    out.append(" topology_provenance=");
    out.append(itf::to_string(topology.provenance));
  }
  out.append(" models=");
  out.append(std::to_string(models.size()));
  out.append(" states=");
  out.append(std::to_string(states.size()));
  out.append(" terminal_requests=");
  out.append(std::to_string(terminal_requests.size()));
  out.append(" interrupted_requests=");
  out.append(std::to_string(interrupted_requests.size()));
  return out;
}

SnapshotStore::SnapshotStore(std::string path, std::size_t max_payload_bytes)
    : path_(std::move(path)), max_payload_bytes_(max_payload_bytes) {}

bool SnapshotStore::exists() const {
  return FileHandle::open_existing(path_).ok();
}

Status SnapshotStore::save(const PersistedCoordinatorState& state) {
  Writer writer(max_payload_bytes_);
  state.encode_into(writer);
  if (!writer.ok()) {
    return Status(writer.fail_code(), "snapshot document exceeds the configured size ceiling");
  }
  return save_raw(ByteSpan(writer.buffer().data(), writer.buffer().size()));
}

Status SnapshotStore::save_raw(ByteSpan payload) {
  if (payload.size() > max_payload_bytes_) {
    return Status(StatusCode::BoundsExceeded, "snapshot payload exceeds the configured ceiling");
  }
  std::vector<std::uint8_t> header(kHeaderBytes, 0);
  header[0] = kMagic0;
  header[1] = kMagic1;
  header[2] = kMagic2;
  header[3] = kMagic3;
  store_u16(header.data() + 4, kSnapshotFormatVersion);
  store_u16(header.data() + 6, 0);
  store_u64(header.data() + 8, static_cast<std::uint64_t>(payload.size()));
  store_u32(header.data() + 16, Crc32c::compute(payload));
  store_u32(header.data() + 20,
            Crc32c::compute(ByteSpan(header.data(), kHeaderBytes - sizeof(std::uint32_t))));

  const std::string temporary = path_ + ".tmp." + std::to_string(current_process_id()) + "." +
                                std::to_string(next_temporary_serial());
  {
    auto handle = FileHandle::create_truncate(temporary);
    if (!handle.ok()) return handle.status();
    FileHandle file = std::move(handle).value();
    const Status header_written = file.write_all(ByteSpan(header.data(), header.size()));
    if (!header_written.ok()) return header_written;
    const Status payload_written = file.write_all(payload);
    if (!payload_written.ok()) return payload_written;
    const Status flushed = file.sync();
    if (!flushed.ok()) return flushed;
  }
  const Status replaced = atomic_replace_file(temporary, path_);
  if (!replaced.ok()) {
    (void)std::remove(temporary.c_str());
    return replaced;
  }
  return Status::success();
}

Result<std::vector<std::uint8_t>> SnapshotStore::load_raw() const {
  auto opened = FileHandle::open_existing(path_);
  if (!opened.ok()) return Result<std::vector<std::uint8_t>>::failure(opened.status());
  FileHandle file = std::move(opened).value();
  std::vector<std::uint8_t> header(kHeaderBytes, 0);
  const Status header_read = file.read_exact(MutableByteSpan(header.data(), header.size()));
  if (!header_read.ok()) {
    return Result<std::vector<std::uint8_t>>::failure(StatusCode::MalformedInput,
                                                      "snapshot header is truncated");
  }
  if (header[0] != kMagic0 || header[1] != kMagic1 || header[2] != kMagic2 ||
      header[3] != kMagic3) {
    return Result<std::vector<std::uint8_t>>::failure(StatusCode::MalformedInput,
                                                      "snapshot magic does not match");
  }
  const std::uint16_t version = load_u16(header.data() + 4);
  if (version != kSnapshotFormatVersion) {
    return Result<std::vector<std::uint8_t>>::failure(
        StatusCode::UnsupportedVersion,
        "snapshot format version " + std::to_string(version) + " is not supported");
  }
  const std::uint32_t expected_header_crc = load_u32(header.data() + 20);
  const std::uint32_t actual_header_crc =
      Crc32c::compute(ByteSpan(header.data(), kHeaderBytes - sizeof(std::uint32_t)));
  if (expected_header_crc != actual_header_crc) {
    return Result<std::vector<std::uint8_t>>::failure(StatusCode::IntegrityMismatch,
                                                      "snapshot header checksum mismatch");
  }
  const std::uint64_t payload_length = load_u64(header.data() + 8);
  if (payload_length > static_cast<std::uint64_t>(max_payload_bytes_)) {
    return Result<std::vector<std::uint8_t>>::failure(
        StatusCode::BoundsExceeded, "snapshot payload exceeds the configured ceiling");
  }
  const std::uint32_t expected_payload_crc = load_u32(header.data() + 16);
  std::vector<std::uint8_t> payload(static_cast<std::size_t>(payload_length), 0);
  if (!payload.empty()) {
    const Status payload_read = file.read_exact(MutableByteSpan(payload.data(), payload.size()));
    if (!payload_read.ok()) {
      return Result<std::vector<std::uint8_t>>::failure(StatusCode::MalformedInput,
                                                        "snapshot payload is truncated");
    }
  }
  auto more = file.has_more();
  if (!more.ok()) return Result<std::vector<std::uint8_t>>::failure(more.status());
  if (more.value()) {
    return Result<std::vector<std::uint8_t>>::failure(StatusCode::TrailingGarbage,
                                                      "snapshot has trailing bytes");
  }
  if (Crc32c::compute(ByteSpan(payload.data(), payload.size())) != expected_payload_crc) {
    return Result<std::vector<std::uint8_t>>::failure(StatusCode::IntegrityMismatch,
                                                      "snapshot payload checksum mismatch");
  }
  return Result<std::vector<std::uint8_t>>::success(std::move(payload));
}

Result<PersistedCoordinatorState> SnapshotStore::load() const {
  auto payload = load_raw();
  if (!payload.ok()) return Result<PersistedCoordinatorState>::failure(payload.status());
  const std::vector<std::uint8_t>& bytes = payload.value();
  Reader reader(ByteSpan(bytes.data(), bytes.size()), max_payload_bytes_);
  auto state = PersistedCoordinatorState::decode_from(reader);
  if (!state.ok()) return state;
  const Status finished = reader.finish();
  if (!finished.ok()) return Result<PersistedCoordinatorState>::failure(finished);
  return state;
}

Status atomic_replace_file(const std::string& source, const std::string& target) {
#if defined(_WIN32)
  if (MoveFileExA(source.c_str(), target.c_str(),
                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
    return Status(StatusCode::IoError, "atomic replace failed");
  }
  return Status::success();
#else
  if (::rename(source.c_str(), target.c_str()) != 0) {
    return Status(StatusCode::IoError, "atomic replace failed");
  }
  return Status::success();
#endif
}

Status sync_file(const std::string& path) {
  auto opened = FileHandle::open_existing(path);
  if (!opened.ok()) return opened.status();
  FileHandle file = std::move(opened).value();
  return file.sync();
}

}  // namespace itf
