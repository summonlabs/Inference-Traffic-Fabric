// Inference Traffic Fabric - portable loopback TCP transport primitives.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef ITF_NET_HPP
#define ITF_NET_HPP

#include <cstdint>
#include <string>
#include <string_view>

#include "itf/bytes.hpp"
#include "itf/error.hpp"

namespace itf::net {

inline constexpr std::intptr_t kInvalidHandle = -1;

/// Idempotent process-wide socket subsystem initialisation. Every entry point
/// that touches sockets calls this; it is safe under concurrent calls.
Status initialize_sockets();
void shutdown_sockets();

/// Connected TCP stream. Readers blocked in read_some observe a close() from
/// another thread as an immediate end-of-stream, which is what makes shutdown
/// with blocked readers possible without relying on socket timeouts.
class TcpStream {
 public:
  TcpStream() noexcept = default;
  ~TcpStream();

  TcpStream(TcpStream&& other) noexcept;
  TcpStream& operator=(TcpStream&& other) noexcept;
  TcpStream(const TcpStream&) = delete;
  TcpStream& operator=(const TcpStream&) = delete;

  [[nodiscard]] static Result<TcpStream> connect(std::string_view host, std::uint16_t port,
                                                 std::int64_t connect_timeout_nanos);
  [[nodiscard]] static TcpStream adopt(std::intptr_t handle, std::string peer);

  [[nodiscard]] bool valid() const noexcept { return handle_ != kInvalidHandle; }
  [[nodiscard]] std::intptr_t native_handle() const noexcept { return handle_; }
  [[nodiscard]] const std::string& peer() const noexcept { return peer_; }

  Result<std::size_t> read_some(MutableByteSpan buffer, std::int64_t timeout_nanos);
  Result<std::size_t> write(ByteSpan buffer);
  Status write_all(ByteSpan buffer);
  Status read_exact(MutableByteSpan buffer, std::int64_t timeout_nanos);
  [[nodiscard]] bool wait_readable(std::int64_t timeout_nanos) const;

  /// Shuts the connection down and closes the handle. Safe to call repeatedly
  /// and from another thread while a reader is blocked.
  void close() noexcept;

  void set_nodelay(bool enabled) noexcept;
  void set_keepalive(bool enabled) noexcept;
  [[nodiscard]] std::uint16_t local_port() const noexcept;
  [[nodiscard]] std::uint16_t remote_port() const noexcept;

 private:
  std::intptr_t handle_ = kInvalidHandle;
  std::string peer_;
};

struct ListenConfig {
  std::string bind_host = "127.0.0.1";
  std::uint16_t port = 0;
  std::uint32_t backlog = 32;
};

class TcpListener {
 public:
  TcpListener() noexcept = default;
  ~TcpListener();

  TcpListener(TcpListener&& other) noexcept;
  TcpListener& operator=(TcpListener&& other) noexcept;
  TcpListener(const TcpListener&) = delete;
  TcpListener& operator=(const TcpListener&) = delete;

  [[nodiscard]] static Result<TcpListener> bind(const ListenConfig& config);

  /// Accepts with a bounded wait. Returns StatusCode::NotFound when the wait
  /// elapsed without a connection, so callers can re-check their stop flag.
  Result<TcpStream> accept(std::int64_t timeout_nanos);

  [[nodiscard]] bool valid() const noexcept { return handle_ != kInvalidHandle; }
  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
  void close() noexcept;

 private:
  std::intptr_t handle_ = kInvalidHandle;
  std::uint16_t port_ = 0;
};

/// True for 127.0.0.0/8 and ::1 textual addresses.
[[nodiscard]] bool is_loopback_address(std::string_view address) noexcept;

}  // namespace itf::net

#endif  // ITF_NET_HPP
