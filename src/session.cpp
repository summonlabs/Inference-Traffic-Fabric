// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "itf/session.hpp"

#include <algorithm>
#include <mutex>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "itf/crc32c.hpp"
#include "itf/log.hpp"
#include "itf/version.hpp"

namespace itf::net {
namespace {

std::mt19937_64& random_source() {
  static std::mt19937_64 engine(std::random_device{}());
  return engine;
}

template <class Tag>
Id128<Tag> next_random_id() {
  static std::mutex mutex;
  std::lock_guard<std::mutex> guard(mutex);
  std::uint64_t high = random_source()();
  std::uint64_t low = random_source()();
  if (high == 0 && low == 0) low = 1;
  return Id128<Tag>(high, low);
}

/// Constant-time comparison for the shared session secret. The length is
/// public; only the content is compared without an early exit.
[[nodiscard]] bool secret_matches(std::string_view expected, std::string_view provided) noexcept {
  if (expected.size() != provided.size()) return false;
  unsigned char difference = 0;
  for (std::size_t index = 0; index < expected.size(); ++index) {
    difference = static_cast<unsigned char>(
        difference | static_cast<unsigned char>(expected[index] ^ provided[index]));
  }
  return difference == 0;
}

}  // namespace

std::string SessionStats::to_string() const {
  std::string out;
  out.reserve(256);
  out.append("accepted=");
  out.append(std::to_string(accepted));
  out.append(" rejected_capacity=");
  out.append(std::to_string(rejected_capacity));
  out.append(" rejected_auth=");
  out.append(std::to_string(rejected_auth));
  out.append(" rejected_protocol=");
  out.append(std::to_string(rejected_protocol));
  out.append(" frames_in=");
  out.append(std::to_string(frames_in));
  out.append(" frames_out=");
  out.append(std::to_string(frames_out));
  out.append(" frames_invalid=");
  out.append(std::to_string(frames_invalid));
  out.append(" replays_rejected=");
  out.append(std::to_string(replays_rejected));
  out.append(" duplicates_served=");
  out.append(std::to_string(duplicates_served));
  out.append(" bytes_in=");
  out.append(std::to_string(bytes_in));
  out.append(" bytes_out=");
  out.append(std::to_string(bytes_out));
  out.append(" active_sessions=");
  out.append(std::to_string(active_sessions));
  out.append(" peak_sessions=");
  out.append(std::to_string(peak_sessions));
  return out;
}

Server::Server(ServerConfig config, MessageHandler* handler)
    : config_(std::move(config)), handler_(handler) {
  if (config_.max_sessions == 0) config_.max_sessions = 1;
  if (config_.max_frame_payload == 0 ||
      config_.max_frame_payload > wire::kAbsoluteMaxPayloadBytes) {
    config_.max_frame_payload = wire::kAbsoluteMaxPayloadBytes;
  }
}

Server::~Server() { stop(); }

Status Server::start() {
  if (running_.load()) return Status(StatusCode::Conflict, "server is already running");
  if (handler_ == nullptr) return Status(StatusCode::InvalidArgument, "server has no handler");
  if (config_.require_auth && config_.auth_token.empty()) {
    return Status(StatusCode::InvalidArgument,
                  "authenticated sessions require a non-empty shared secret");
  }
  const Status initialized = initialize_sockets();
  if (!initialized.ok()) return initialized;
  ListenConfig listen_config;
  listen_config.bind_host = config_.bind_host;
  listen_config.port = config_.port;
  listen_config.backlog = config_.accept_backlog;
  auto listener = TcpListener::bind(listen_config);
  if (!listener.ok()) return listener.status();
  listener_ = std::move(listener).value();
  bound_port_ = listener_.port();
  stopping_.store(false);
  running_.store(true);
  acceptor_ = std::thread([this] { accept_loop(); });
  return Status::success();
}

void Server::stop() {
  if (!running_.exchange(false)) return;
  stopping_.store(true);
  listener_.close();
  if (acceptor_.joinable()) acceptor_.join();
  std::vector<std::shared_ptr<SessionSlot>> slots;
  {
    std::lock_guard<std::mutex> guard(sessions_mutex_);
    slots = sessions_;
  }
  // Closing a session socket unblocks a reader that is waiting inside recv,
  // so joining can never hang on a silent peer.
  for (const auto& slot : slots) {
    if (slot->stream) slot->stream->close();
  }
  for (const auto& slot : slots) {
    if (slot->worker.joinable()) slot->worker.join();
  }
  {
    std::lock_guard<std::mutex> guard(sessions_mutex_);
    sessions_.clear();
  }
  bound_port_ = 0;
  shutdown_sockets();
}

SessionStats Server::stats() const {
  std::lock_guard<std::mutex> guard(stats_mutex_);
  SessionStats snapshot = stats_;
  snapshot.active_sessions = active_sessions_.load();
  snapshot.peak_sessions = peak_sessions_.load();
  return snapshot;
}

void Server::reap_finished_sessions() {
  std::vector<std::shared_ptr<SessionSlot>> retired;
  {
    std::lock_guard<std::mutex> guard(sessions_mutex_);
    for (auto iterator = sessions_.begin(); iterator != sessions_.end();) {
      if ((*iterator)->finished.load()) {
        retired.push_back(*iterator);
        iterator = sessions_.erase(iterator);
      } else {
        ++iterator;
      }
    }
  }
  for (const auto& slot : retired) {
    if (slot->worker.joinable()) slot->worker.join();
  }
}

void Server::join_all() { reap_finished_sessions(); }

void Server::accept_loop() {
  for (;;) {
    if (stopping_.load()) break;
    auto accepted = listener_.accept(config_.accept_poll_nanos);
    if (!accepted.ok()) {
      reap_finished_sessions();
      if (accepted.code() == StatusCode::NotFound) continue;
      if (stopping_.load()) break;
      log_warn("session", std::string("accept failed: ") + accepted.status().to_string());
      continue;
    }
    reap_finished_sessions();
    auto slot = std::make_shared<SessionSlot>();
    slot->stream = std::make_shared<TcpStream>(std::move(accepted).value());
    const std::uint32_t active = active_sessions_.load();
    if (active >= config_.max_sessions) {
      {
        std::lock_guard<std::mutex> guard(stats_mutex_);
        stats_.rejected_capacity += 1;
      }
      // Refuse deterministically instead of queueing unbounded work.
      std::vector<std::uint8_t> header(wire::kHeaderBytes, 0);
      wire::FrameHeader refusal;
      refusal.type = wire::MessageType::ErrorResponse;
      wire::ErrorResponse body;
      body.code = StatusCode::CapacityExhausted;
      body.detail = "session capacity is exhausted";
      Writer writer;
      body.encode(writer);
      refusal.payload_length = static_cast<std::uint32_t>(writer.size());
      refusal.payload_crc32c = Crc32c::compute(ByteSpan(writer.buffer().data(), writer.size()));
      refusal.sequence = 1;
      wire::encode_header(refusal, MutableByteSpan(header.data(), header.size()));
      (void)slot->stream->write_all(ByteSpan(header.data(), header.size()));
      (void)slot->stream->write_all(ByteSpan(writer.buffer().data(), writer.size()));
      slot->stream->close();
      continue;
    }
    {
      std::lock_guard<std::mutex> guard(sessions_mutex_);
      sessions_.push_back(slot);
    }
    const std::uint32_t now_active = active_sessions_.fetch_add(1) + 1;
    std::uint32_t peak = peak_sessions_.load();
    while (now_active > peak && !peak_sessions_.compare_exchange_weak(peak, now_active)) {
    }
    {
      std::lock_guard<std::mutex> guard(stats_mutex_);
      stats_.accepted += 1;
    }
    slot->worker = std::thread([this, slot] { serve_session(slot); });
  }
}

void Server::serve_session(std::shared_ptr<SessionSlot> slot) {
  TcpStream& stream = *slot->stream;
  MessageHandler::Context context;
  context.shutdown_requested = &stopping_;
  std::uint32_t max_payload = static_cast<std::uint32_t>(config_.max_frame_payload);
  std::uint64_t inbound_sequence = 0;
  std::uint64_t outbound_sequence = 0;
  std::uint64_t replays = 0;
  bool handshaken = false;
  auto bump = [&](std::uint64_t SessionStats::*field, std::uint64_t amount) {
    std::lock_guard<std::mutex> guard(stats_mutex_);
    stats_.*field += amount;
  };

  auto send = [&](wire::MessageType type, ByteSpan payload, std::uint64_t correlation) -> Status {
    std::vector<std::uint8_t> header(wire::kHeaderBytes, 0);
    wire::FrameHeader frame;
    frame.type = type;
    frame.payload_length = static_cast<std::uint32_t>(payload.size());
    frame.correlation = correlation;
    outbound_sequence += 1;
    frame.sequence = outbound_sequence;
    frame.payload_crc32c = Crc32c::compute(payload);
    wire::encode_header(frame, MutableByteSpan(header.data(), header.size()));
    Status status = stream.write_all(ByteSpan(header.data(), header.size()));
    if (status.ok() && !payload.empty()) status = stream.write_all(payload);
    if (status.ok()) {
      bump(&SessionStats::frames_out, 1);
      bump(&SessionStats::bytes_out, header.size() + payload.size());
    }
    return status;
  };

  auto send_error = [&](StatusCode code, wire::MessageType offending, std::string detail,
                        std::uint64_t correlation) {
    wire::ErrorResponse body;
    body.code = code;
    body.offending_type = offending;
    body.detail = std::move(detail);
    Writer writer;
    body.encode(writer);
    (void)send(wire::MessageType::ErrorResponse, ByteSpan(writer.buffer().data(), writer.size()),
               correlation);
  };

  for (;;) {
    if (stopping_.load()) break;
    std::vector<std::uint8_t> header(wire::kHeaderBytes, 0);
    const Status read_header =
        stream.read_exact(MutableByteSpan(header.data(), header.size()), config_.io_timeout_nanos);
    if (!read_header.ok()) {
      if (read_header.code() == StatusCode::NotFound) continue;
      break;
    }
    const auto decoded = wire::decode_header(ByteSpan(header.data(), header.size()), max_payload);
    if (!decoded.ok()) {
      bump(&SessionStats::frames_invalid, 1);
      bump(&SessionStats::bytes_in, header.size());
      send_error(decoded.code(), wire::MessageType::Invalid, decoded.status().message(), 0);
      break;
    }
    const wire::FrameHeader frame = decoded.value();
    std::vector<std::uint8_t> payload(frame.payload_length, 0);
    if (!payload.empty()) {
      const Status read_payload =
          stream.read_exact(MutableByteSpan(payload.data(), payload.size()), config_.io_timeout_nanos);
      if (!read_payload.ok()) {
        bump(&SessionStats::frames_invalid, 1);
        break;
      }
    }
    bump(&SessionStats::frames_in, 1);
    bump(&SessionStats::bytes_in, header.size() + payload.size());
    if (Crc32c::compute(ByteSpan(payload.data(), payload.size())) != frame.payload_crc32c) {
      bump(&SessionStats::frames_invalid, 1);
      send_error(StatusCode::IntegrityMismatch, frame.type, "frame payload checksum mismatch",
                 frame.sequence);
      break;
    }
    if (frame.sequence <= inbound_sequence) {
      replays += 1;
      bump(&SessionStats::replays_rejected, 1);
      send_error(StatusCode::ReplayRejected, frame.type,
                 "frame sequence is not greater than the last accepted sequence", frame.sequence);
      if (replays > 8) break;
      continue;
    }
    inbound_sequence = frame.sequence;

    const ByteSpan body(payload.data(), payload.size());
    if (!handshaken) {
      if (frame.type != wire::MessageType::HelloRequest) {
        bump(&SessionStats::rejected_protocol, 1);
        send_error(StatusCode::NotAuthorized, frame.type, "session handshake is required",
                   frame.sequence);
        break;
      }
      Reader reader(body);
      auto hello = wire::HelloRequest::decode(reader);
      if (!hello.ok() || !reader.finish().ok()) {
        bump(&SessionStats::rejected_protocol, 1);
        send_error(hello.ok() ? StatusCode::TrailingGarbage : hello.status().code(),
                   frame.type, "handshake body is not acceptable", frame.sequence);
        break;
      }
      if (config_.require_auth &&
          !secret_matches(config_.auth_token, hello.value().auth_token)) {
        bump(&SessionStats::rejected_auth, 1);
        send_error(StatusCode::NotAuthorized, frame.type, "session secret was not accepted",
                   frame.sequence);
        break;
      }
      max_payload = std::min<std::uint32_t>(
          static_cast<std::uint32_t>(config_.max_frame_payload), hello.value().max_frame_payload);
      if (max_payload == 0) max_payload = static_cast<std::uint32_t>(config_.max_frame_payload);
      context.session = next_random_id<SessionIdTag>();
      context.peer = next_random_id<PeerIdTag>();
      context.peer_name = hello.value().peer_name;
      context.peer_boot = hello.value().client_boot;
      context.max_frame_payload = max_payload;
      wire::HelloResponse response;
      response.session = context.session;
      response.peer = context.peer;
      response.max_frame_payload = max_payload;
      response.server_version = std::string(kVersionString);
      handler_->describe_authority(response);
      Writer writer;
      response.encode(writer);
      const Status sent = send(wire::MessageType::HelloResponse,
                               ByteSpan(writer.buffer().data(), writer.size()), frame.sequence);
      if (!sent.ok()) break;
      handshaken = true;
      continue;
    }

    if (frame.type == wire::MessageType::HelloRequest) {
      send_error(StatusCode::AlreadyExists, frame.type, "session is already established",
                 frame.sequence);
      continue;
    }
    if (handler_ == nullptr) {
      send_error(StatusCode::Unsupported, frame.type, "no message handler is installed",
                 frame.sequence);
      continue;
    }
    auto response = handler_->handle(context, frame.type, body);
    if (!response.ok()) {
      send_error(response.status().code(), frame.type, response.status().message(), frame.sequence);
      continue;
    }
    const std::vector<std::uint8_t>& reply = response.value();
    const wire::MessageType reply_type = wire::response_type_for(frame.type);
    if (reply_type == wire::MessageType::Invalid) {
      send_error(StatusCode::Unsupported, frame.type, "message type has no response mapping",
                 frame.sequence);
      continue;
    }
    if (!send(reply_type, ByteSpan(reply.data(), reply.size()), frame.sequence).ok()) break;
  }

  if (handshaken && handler_ != nullptr) handler_->on_session_closed(context);
  stream.close();
  active_sessions_.fetch_sub(1);
  slot->finished.store(true);
}

}  // namespace itf::net
