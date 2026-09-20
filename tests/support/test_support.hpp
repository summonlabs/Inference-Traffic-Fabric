// Inference Traffic Fabric - shared test support services.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef ITF_TEST_SUPPORT_HPP
#define ITF_TEST_SUPPORT_HPP

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "itf/error.hpp"
#include "itf/ids.hpp"

namespace itf::test {

/// Creates a unique scratch directory under the system temporary directory.
[[nodiscard]] std::string make_temp_dir(const std::string& tag);

/// Recursively removes a directory created by make_temp_dir.
void remove_dir(const std::string& path);

/// Binds an ephemeral loopback port and returns it. The socket is closed
/// before returning, so the port is free for a child process to bind.
[[nodiscard]] std::uint16_t find_free_port();

[[nodiscard]] bool file_exists(const std::string& path);
[[nodiscard]] std::string read_text_file(const std::string& path);
void write_text_file(const std::string& path, const std::string& text);
[[nodiscard]] std::uint64_t file_size(const std::string& path);
void truncate_file(const std::string& path, std::uint64_t bytes);
void flip_byte(const std::string& path, std::uint64_t offset);

/// Directory of the running test executable, used to locate the sibling tool
/// executables that the multiprocess tests drive.
[[nodiscard]] std::string executable_directory();
[[nodiscard]] std::string tool_path(const std::string& tool_name);

/// A real operating-system child process.
class ChildProcess {
 public:
  ChildProcess() = default;
  ~ChildProcess();
  ChildProcess(ChildProcess&& other) noexcept;
  ChildProcess& operator=(ChildProcess&& other) noexcept;
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;

  /// Spawns the executable with the given arguments. The child inherits the
  /// parent's standard streams so its diagnostics appear in the test log.
  [[nodiscard]] static Result<ChildProcess> spawn(const std::string& executable,
                                                  const std::vector<std::string>& arguments);

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] bool running();
  /// Waits for termination and returns the exit code. Reliable only on a
  /// process that was not killed through the platform's force-kill path.
  int wait();
  /// Forcefully terminates the process and reaps it.
  void kill();
  [[nodiscard]] std::uint64_t pid() const noexcept { return pid_; }

 private:
  std::intptr_t handle_ = 0;
  std::uint64_t pid_ = 0;
  bool reaped_ = false;
  int exit_code_ = -1;
};

/// Waits until the predicate holds or the budget is exhausted. Returns whether
/// the predicate held. The budget is an operational bound for a test helper,
/// not a test timeout: a false result is asserted by the caller.
bool wait_until(const std::function<bool()>& predicate, std::uint64_t budget_ms);

/// Deterministic identity helpers. Identities are derived from the seed so a
/// failing run reproduces exactly from its printed seed.
[[nodiscard]] itf::RequestId make_request_id(std::uint64_t seed, std::uint64_t index);
[[nodiscard]] itf::AttemptId make_attempt_id(std::uint64_t seed, std::uint64_t index);
[[nodiscard]] itf::StateTransferId make_transfer_id(std::uint64_t seed, std::uint64_t index);

}  // namespace itf::test

#endif  // ITF_TEST_SUPPORT_HPP
