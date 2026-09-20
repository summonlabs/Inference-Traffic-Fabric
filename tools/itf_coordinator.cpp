// Inference Traffic Fabric - coordinator service process.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "service.hpp"

namespace {

void print_usage() {
  std::printf(
      "itf_coordinator - Inference Traffic Fabric coordinator service\n"
      "\n"
      "usage: itf_coordinator --token SECRET [options]\n"
      "\n"
      "options:\n"
      "  --port N                  listen port (default 7777, 0 selects an ephemeral port)\n"
      "  --bind ADDRESS            bind address (default 127.0.0.1)\n"
      "  --token SECRET            shared session secret (required)\n"
      "  --state PATH              durable snapshot file (enables persistence)\n"
      "  --state-dir DIR           durable snapshot directory (coordinator.bin inside it)\n"
      "  --snapshot-interval-ms N  minimum interval between automatic snapshots (default 0)\n"
      "  --max-sessions N          concurrent session ceiling (default 32)\n"
      "  --max-frame-bytes N       negotiated frame payload ceiling (default 262144)\n"
      "  --idle-seconds N          per-read wait before a session is retired (default 30)\n"
      "  --verbose                 enable informational logging\n"
      "  --help                    print this message\n");
}

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

}  // namespace

int main(int argc, char** argv) {
  itf::service::ServiceConfig config;
  config.server.port = 7777;
  config.server.bind_host = "127.0.0.1";
  config.persist = false;
  bool verbose = false;

  for (int index = 1; index < argc; ++index) {
    const std::string_view flag(argv[index]);
    const auto next = [&](const char* name) -> const char* {
      if (index + 1 >= argc) {
        std::fprintf(stderr, "missing value for %s\n", name);
        std::exit(2);
      }
      return argv[++index];
    };
    std::uint64_t number = 0;
    if (flag == "--help" || flag == "-h") {
      print_usage();
      return 0;
    }
    if (flag == "--port") {
      if (!parse_u64(next("--port"), number) || number > 65535) {
        std::fprintf(stderr, "invalid --port\n");
        return 2;
      }
      config.server.port = static_cast<std::uint16_t>(number);
    } else if (flag == "--bind") {
      config.server.bind_host = next("--bind");
    } else if (flag == "--token") {
      config.server.auth_token = next("--token");
    } else if (flag == "--state") {
      config.snapshot_path = next("--state");
      config.persist = true;
    } else if (flag == "--state-dir") {
      std::string directory = next("--state-dir");
      if (!directory.empty() && directory.back() != '/' && directory.back() != '\\') {
        directory.push_back('/');
      }
      config.snapshot_path = directory + "coordinator.bin";
      config.persist = true;
    } else if (flag == "--snapshot-interval-ms") {
      if (!parse_u64(next("--snapshot-interval-ms"), number)) {
        std::fprintf(stderr, "invalid --snapshot-interval-ms\n");
        return 2;
      }
      config.snapshot_interval_nanos = static_cast<std::int64_t>(number) * 1000000LL;
    } else if (flag == "--max-sessions") {
      if (!parse_u64(next("--max-sessions"), number) || number == 0) {
        std::fprintf(stderr, "invalid --max-sessions\n");
        return 2;
      }
      config.server.max_sessions = static_cast<std::uint32_t>(number);
    } else if (flag == "--max-frame-bytes") {
      if (!parse_u64(next("--max-frame-bytes"), number) || number == 0) {
        std::fprintf(stderr, "invalid --max-frame-bytes\n");
        return 2;
      }
      config.server.max_frame_payload = static_cast<std::size_t>(number);
    } else if (flag == "--idle-seconds") {
      if (!parse_u64(next("--idle-seconds"), number) || number == 0) {
        std::fprintf(stderr, "invalid --idle-seconds\n");
        return 2;
      }
      config.server.idle_timeout_nanos = static_cast<std::int64_t>(number) * 1000000000LL;
    } else if (flag == "--verbose") {
      verbose = true;
    } else {
      std::fprintf(stderr, "unknown option: %s\n", argv[index]);
      print_usage();
      return 2;
    }
  }

  if (config.server.auth_token.empty()) {
    std::fprintf(stderr, "--token is required: sessions are authenticated by default\n");
    print_usage();
    return 2;
  }
  return itf::service::run_coordinator_service(config, verbose);
}
