// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "itf/net.hpp"

#include <atomic>
#include <cstring>
#include <string>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace itf::net {
namespace {

#if defined(_WIN32)
using native_socket = SOCKET;
constexpr native_socket kInvalidNative = INVALID_SOCKET;
std::atomic<int> g_socket_references{0};
#else
using native_socket = int;
constexpr native_socket kInvalidNative = -1;
#endif

[[nodiscard]] native_socket to_native(std::intptr_t handle) noexcept {
  return static_cast<native_socket>(handle);
}

[[nodiscard]] std::intptr_t from_native(native_socket handle) noexcept {
  return static_cast<std::intptr_t>(handle);
}

[[nodiscard]] bool is_valid(native_socket handle) noexcept { return handle != kInvalidNative; }

[[nodiscard]] std::string last_socket_error() {
#if defined(_WIN32)
  return std::to_string(static_cast<unsigned long>(WSAGetLastError()));
#else
  return std::to_string(errno);
#endif
}

void close_native(native_socket handle) noexcept {
  if (!is_valid(handle)) return;
#if defined(_WIN32)
  (void)closesocket(handle);
#else
  (void)::close(handle);
#endif
}

void shutdown_native(native_socket handle) noexcept {
  if (!is_valid(handle)) return;
#if defined(_WIN32)
  (void)shutdown(handle, SD_BOTH);
#else
  (void)shutdown(handle, SHUT_RDWR);
#endif
}

[[nodiscard]] bool would_block() noexcept {
#if defined(_WIN32)
  const int error = WSAGetLastError();
  return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS;
#else
  return errno == EWOULDBLOCK || errno == EAGAIN || errno == EINPROGRESS;
#endif
}

Status set_blocking(native_socket handle, bool blocking) {
#if defined(_WIN32)
  u_long mode = blocking ? 0UL : 1UL;
  if (ioctlsocket(handle, FIONBIO, &mode) != 0) {
    return Status(StatusCode::IoError, "cannot change socket blocking mode");
  }
  return Status::success();
#else
  const int flags = fcntl(handle, F_GETFL, 0);
  if (flags < 0) return Status(StatusCode::IoError, "cannot read socket flags");
  const int updated = blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK);
  if (fcntl(handle, F_SETFL, updated) < 0) {
    return Status(StatusCode::IoError, "cannot change socket blocking mode");
  }
  return Status::success();
#endif
}

[[nodiscard]] std::int64_t clamp_select_timeout(std::int64_t timeout_nanos) noexcept {
  if (timeout_nanos <= 0) return 0;
  const std::int64_t kMax = 3600LL * 1000000000LL;
  return timeout_nanos > kMax ? kMax : timeout_nanos;
}

/// Waits until the socket is ready for the requested direction.
/// Returns 1 when ready, 0 on timeout and -1 on error.
int wait_socket(native_socket handle, bool for_read, std::int64_t timeout_nanos) {
  if (!is_valid(handle)) return -1;
  fd_set read_set;
  fd_set write_set;
  FD_ZERO(&read_set);
  FD_ZERO(&write_set);
  if (for_read) {
    FD_SET(handle, &read_set);
  } else {
    FD_SET(handle, &write_set);
  }
  const std::int64_t bounded = clamp_select_timeout(timeout_nanos);
  timeval timeout;
  timeout.tv_sec = static_cast<long>(bounded / 1000000000LL);
  timeout.tv_usec = static_cast<long>((bounded % 1000000000LL) / 1000LL);
  const int ready = ::select(static_cast<int>(handle) + 1, &read_set, &write_set, nullptr, &timeout);
  return ready;
}

[[nodiscard]] std::string format_endpoint(const sockaddr_storage& address) {
  char host[INET6_ADDRSTRLEN] = {0};
  const void* source = nullptr;
  std::uint16_t port = 0;
  if (address.ss_family == AF_INET) {
    const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(&address);
    source = &ipv4->sin_addr;
    port = ntohs(ipv4->sin_port);
  } else if (address.ss_family == AF_INET6) {
    const auto* ipv6 = reinterpret_cast<const sockaddr_in6*>(&address);
    source = &ipv6->sin6_addr;
    port = ntohs(ipv6->sin6_port);
  } else {
    return "unknown";
  }
  if (::inet_ntop(address.ss_family, source, host, sizeof(host)) == nullptr) {
    return "unknown";
  }
  std::string out(host);
  out.push_back(':');
  out.append(std::to_string(port));
  return out;
}

}  // namespace

Status initialize_sockets() {
#if defined(_WIN32)
  if (g_socket_references.fetch_add(1) == 0) {
    WSADATA data{};
    const int result = WSAStartup(MAKEWORD(2, 2), &data);
    if (result != 0) {
      g_socket_references.fetch_sub(1);
      return Status(StatusCode::Unsupported, "Winsock initialisation failed");
    }
  }
#endif
  return Status::success();
}

void shutdown_sockets() {
#if defined(_WIN32)
  if (g_socket_references.load() > 0 && g_socket_references.fetch_sub(1) == 1) {
    (void)WSACleanup();
  }
#endif
}

TcpStream::~TcpStream() { close(); }

TcpStream::TcpStream(TcpStream&& other) noexcept
    : handle_(other.handle_), peer_(std::move(other.peer_)) {
  other.handle_ = kInvalidHandle;
  other.peer_.clear();
}

TcpStream& TcpStream::operator=(TcpStream&& other) noexcept {
  if (this != &other) {
    close();
    handle_ = other.handle_;
    peer_ = std::move(other.peer_);
    other.handle_ = kInvalidHandle;
    other.peer_.clear();
  }
  return *this;
}

TcpStream TcpStream::adopt(std::intptr_t handle, std::string peer) {
  TcpStream stream;
  stream.handle_ = handle;
  stream.peer_ = std::move(peer);
  return stream;
}

Result<TcpStream> TcpStream::connect(std::string_view host, std::uint16_t port,
                                     std::int64_t connect_timeout_nanos) {
  const Status initialized = initialize_sockets();
  if (!initialized.ok()) return Result<TcpStream>::failure(initialized);
  const std::string host_text(host);
  const std::string port_text = std::to_string(port);
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  addrinfo* addresses = nullptr;
  if (::getaddrinfo(host_text.c_str(), port_text.c_str(), &hints, &addresses) != 0) {
    return Result<TcpStream>::failure(StatusCode::InvalidArgument,
                                      "cannot resolve the requested endpoint");
  }
  Status last_failure(StatusCode::IoError, "no usable address for the endpoint");
  for (addrinfo* candidate = addresses; candidate != nullptr; candidate = candidate->ai_next) {
    const native_socket handle =
        ::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
    if (!is_valid(handle)) {
      last_failure = Status(StatusCode::IoError, "cannot create a socket: " + last_socket_error());
      continue;
    }
    const Status non_blocking = set_blocking(handle, false);
    if (!non_blocking.ok()) {
      close_native(handle);
      last_failure = non_blocking;
      continue;
    }
    int result = ::connect(handle, candidate->ai_addr,
                           static_cast<int>(candidate->ai_addrlen));
    if (result != 0 && would_block()) {
      const int ready = wait_socket(handle, false, connect_timeout_nanos);
      if (ready == 1) {
        int error = 0;
#if defined(_WIN32)
        int length = static_cast<int>(sizeof(error));
#else
        socklen_t length = sizeof(error);
#endif
        if (::getsockopt(handle, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &length) ==
                0 &&
            error == 0) {
          result = 0;
        }
      }
    }
    if (result != 0) {
      last_failure = Status(StatusCode::NotFound, "connection attempt failed");
      close_native(handle);
      continue;
    }
    const Status blocking = set_blocking(handle, true);
    if (!blocking.ok()) {
      close_native(handle);
      last_failure = blocking;
      continue;
    }
    sockaddr_storage local{};
#if defined(_WIN32)
    int local_length = static_cast<int>(sizeof(local));
#else
    socklen_t local_length = sizeof(local);
#endif
    std::string description(host_text);
    description.push_back(':');
    description.append(port_text);
    if (::getsockname(handle, reinterpret_cast<sockaddr*>(&local), &local_length) == 0) {
      description = format_endpoint(local);
    }
    ::freeaddrinfo(addresses);
    TcpStream stream = TcpStream::adopt(from_native(handle), description);
    stream.set_nodelay(true);
    return Result<TcpStream>::success(std::move(stream));
  }
  ::freeaddrinfo(addresses);
  return Result<TcpStream>::failure(last_failure);
}

bool TcpStream::wait_readable(std::int64_t timeout_nanos) const {
  return wait_socket(to_native(handle_), true, timeout_nanos) == 1;
}

Result<std::size_t> TcpStream::read_some(MutableByteSpan buffer, std::int64_t timeout_nanos) {
  if (!valid()) return Result<std::size_t>::failure(StatusCode::Closed, "socket is closed");
  if (buffer.empty()) return Result<std::size_t>::success(0);
  const int ready = wait_socket(to_native(handle_), true, timeout_nanos);
  if (ready == 0) {
    return Result<std::size_t>::failure(StatusCode::NotFound, "no data available before the wait");
  }
  if (ready < 0) {
    return Result<std::size_t>::failure(StatusCode::Closed, "socket wait failed");
  }
  const int received = ::recv(to_native(handle_), reinterpret_cast<char*>(buffer.data()),
                              static_cast<int>(buffer.size()), 0);
  if (received == 0) return Result<std::size_t>::success(0);
  if (received < 0) {
    return Result<std::size_t>::failure(StatusCode::IoError,
                                        "receive failed: " + last_socket_error());
  }
  return Result<std::size_t>::success(static_cast<std::size_t>(received));
}

Status TcpStream::read_exact(MutableByteSpan buffer, std::int64_t timeout_nanos) {
  std::size_t offset = 0;
  while (offset < buffer.size()) {
    if (!valid()) return Status(StatusCode::Closed, "socket is closed");
    const int ready = wait_socket(to_native(handle_), true, timeout_nanos);
    if (ready == 0) {
      return Status(StatusCode::NotFound, "read wait elapsed");
    }
    if (ready < 0) return Status(StatusCode::Closed, "socket wait failed");
    const int received = ::recv(to_native(handle_), reinterpret_cast<char*>(buffer.data() + offset),
                                static_cast<int>(buffer.size() - offset), 0);
    if (received == 0) return Status(StatusCode::Closed, "peer closed the connection");
    if (received < 0) {
      return Status(StatusCode::IoError, "receive failed: " + last_socket_error());
    }
    offset += static_cast<std::size_t>(received);
  }
  return Status::success();
}

Result<std::size_t> TcpStream::write(ByteSpan buffer) {
  if (!valid()) return Result<std::size_t>::failure(StatusCode::Closed, "socket is closed");
  if (buffer.empty()) return Result<std::size_t>::success(0);
  const int sent = ::send(to_native(handle_), reinterpret_cast<const char*>(buffer.data()),
                          static_cast<int>(buffer.size()), 0);
  if (sent < 0) {
    return Result<std::size_t>::failure(StatusCode::IoError, "send failed: " + last_socket_error());
  }
  return Result<std::size_t>::success(static_cast<std::size_t>(sent));
}

Status TcpStream::write_all(ByteSpan buffer) {
  std::size_t offset = 0;
  while (offset < buffer.size()) {
    if (!valid()) return Status(StatusCode::Closed, "socket is closed");
    const int sent = ::send(to_native(handle_), reinterpret_cast<const char*>(buffer.data() + offset),
                            static_cast<int>(buffer.size() - offset), 0);
    if (sent <= 0) {
      if (sent < 0 && would_block()) {
        const int ready = wait_socket(to_native(handle_), false, 5000000000LL);
        if (ready == 1) continue;
      }
      return Status(StatusCode::IoError, "send failed: " + last_socket_error());
    }
    offset += static_cast<std::size_t>(sent);
  }
  return Status::success();
}

void TcpStream::close() noexcept {
  const native_socket handle = to_native(handle_);
  handle_ = kInvalidHandle;
  if (!is_valid(handle)) return;
  shutdown_native(handle);
  close_native(handle);
}

void TcpStream::set_nodelay(bool enabled) noexcept {
  if (!valid()) return;
  const int value = enabled ? 1 : 0;
  (void)::setsockopt(to_native(handle_), IPPROTO_TCP, TCP_NODELAY,
                     reinterpret_cast<const char*>(&value), static_cast<int>(sizeof(value)));
}

void TcpStream::set_keepalive(bool enabled) noexcept {
  if (!valid()) return;
  const int value = enabled ? 1 : 0;
  (void)::setsockopt(to_native(handle_), SOL_SOCKET, SO_KEEPALIVE,
                     reinterpret_cast<const char*>(&value), static_cast<int>(sizeof(value)));
}

std::uint16_t TcpStream::local_port() const noexcept {
  if (!valid()) return 0;
  sockaddr_storage local{};
#if defined(_WIN32)
  int length = static_cast<int>(sizeof(local));
#else
  socklen_t length = sizeof(local);
#endif
  if (::getsockname(to_native(handle_), reinterpret_cast<sockaddr*>(&local), &length) != 0) return 0;
  if (local.ss_family == AF_INET) {
    return ntohs(reinterpret_cast<const sockaddr_in*>(&local)->sin_port);
  }
  if (local.ss_family == AF_INET6) {
    return ntohs(reinterpret_cast<const sockaddr_in6*>(&local)->sin6_port);
  }
  return 0;
}

std::uint16_t TcpStream::remote_port() const noexcept {
  if (!valid()) return 0;
  sockaddr_storage remote{};
#if defined(_WIN32)
  int length = static_cast<int>(sizeof(remote));
#else
  socklen_t length = sizeof(remote);
#endif
  if (::getpeername(to_native(handle_), reinterpret_cast<sockaddr*>(&remote), &length) != 0) return 0;
  if (remote.ss_family == AF_INET) {
    return ntohs(reinterpret_cast<const sockaddr_in*>(&remote)->sin_port);
  }
  if (remote.ss_family == AF_INET6) {
    return ntohs(reinterpret_cast<const sockaddr_in6*>(&remote)->sin6_port);
  }
  return 0;
}

TcpListener::~TcpListener() { close(); }

TcpListener::TcpListener(TcpListener&& other) noexcept
    : handle_(other.handle_), port_(other.port_) {
  other.handle_ = kInvalidHandle;
  other.port_ = 0;
}

TcpListener& TcpListener::operator=(TcpListener&& other) noexcept {
  if (this != &other) {
    close();
    handle_ = other.handle_;
    port_ = other.port_;
    other.handle_ = kInvalidHandle;
    other.port_ = 0;
  }
  return *this;
}

Result<TcpListener> TcpListener::bind(const ListenConfig& config) {
  const Status initialized = initialize_sockets();
  if (!initialized.ok()) return Result<TcpListener>::failure(initialized);
  const std::string port_text = std::to_string(config.port);
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  hints.ai_flags = AI_PASSIVE;
  addrinfo* addresses = nullptr;
  const char* host = config.bind_host.empty() ? nullptr : config.bind_host.c_str();
  if (::getaddrinfo(host, port_text.c_str(), &hints, &addresses) != 0) {
    return Result<TcpListener>::failure(StatusCode::InvalidArgument,
                                        "cannot resolve the bind address");
  }
  Status last_failure(StatusCode::IoError, "no usable bind address");
  for (addrinfo* candidate = addresses; candidate != nullptr; candidate = candidate->ai_next) {
    const native_socket handle =
        ::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
    if (!is_valid(handle)) {
      last_failure = Status(StatusCode::IoError, "cannot create a listener socket");
      continue;
    }
#if defined(_WIN32)
    const int exclusive = 1;
    (void)::setsockopt(handle, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                       reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
#else
    const int reuse = 1;
    (void)::setsockopt(handle, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
                       sizeof(reuse));
#endif
    if (::bind(handle, candidate->ai_addr, static_cast<int>(candidate->ai_addrlen)) != 0) {
      last_failure = Status(StatusCode::IoError, "bind failed: " + last_socket_error());
      close_native(handle);
      continue;
    }
    if (::listen(handle, static_cast<int>(config.backlog)) != 0) {
      last_failure = Status(StatusCode::IoError, "listen failed: " + last_socket_error());
      close_native(handle);
      continue;
    }
    sockaddr_storage local{};
#if defined(_WIN32)
    int length = static_cast<int>(sizeof(local));
#else
    socklen_t length = sizeof(local);
#endif
    std::uint16_t bound = 0;
    if (::getsockname(handle, reinterpret_cast<sockaddr*>(&local), &length) == 0) {
      if (local.ss_family == AF_INET) {
        bound = ntohs(reinterpret_cast<const sockaddr_in*>(&local)->sin_port);
      } else if (local.ss_family == AF_INET6) {
        bound = ntohs(reinterpret_cast<const sockaddr_in6*>(&local)->sin6_port);
      }
    }
    ::freeaddrinfo(addresses);
    TcpListener listener;
    listener.handle_ = from_native(handle);
    listener.port_ = bound;
    return Result<TcpListener>::success(std::move(listener));
  }
  ::freeaddrinfo(addresses);
  return Result<TcpListener>::failure(last_failure);
}

Result<TcpStream> TcpListener::accept(std::int64_t timeout_nanos) {
  if (!valid()) return Result<TcpStream>::failure(StatusCode::Closed, "listener is closed");
  const int ready = wait_socket(to_native(handle_), true, timeout_nanos);
  if (ready == 0) {
    return Result<TcpStream>::failure(StatusCode::NotFound, "no connection within the wait");
  }
  if (ready < 0) return Result<TcpStream>::failure(StatusCode::Closed, "listener wait failed");
  sockaddr_storage remote{};
#if defined(_WIN32)
  int length = static_cast<int>(sizeof(remote));
#else
  socklen_t length = sizeof(remote);
#endif
  const native_socket accepted =
      ::accept(to_native(handle_), reinterpret_cast<sockaddr*>(&remote), &length);
  if (!is_valid(accepted)) {
    return Result<TcpStream>::failure(StatusCode::IoError,
                                      "accept failed: " + last_socket_error());
  }
  TcpStream stream = TcpStream::adopt(from_native(accepted), format_endpoint(remote));
  stream.set_nodelay(true);
  return Result<TcpStream>::success(std::move(stream));
}

void TcpListener::close() noexcept {
  const native_socket handle = to_native(handle_);
  handle_ = kInvalidHandle;
  close_native(handle);
}

bool is_loopback_address(std::string_view address) noexcept {
  if (address == "::1") return true;
  if (address == "localhost") return true;
  if (address.size() >= 4 && address.compare(0, 4, "127.") == 0) return true;
  return false;
}

}  // namespace itf::net
