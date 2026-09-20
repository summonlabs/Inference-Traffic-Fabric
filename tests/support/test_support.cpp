// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "support/test_support.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "itf/net.hpp"

#include "support/test.hpp"

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
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace itf::test {
namespace {

std::atomic<std::uint64_t> g_temp_serial{0};

std::string environment_value(const char* name) {
#if defined(_WIN32)
  char* buffer = nullptr;
  std::size_t size = 0;
  if (_dupenv_s(&buffer, &size, name) == 0 && buffer != nullptr) {
    const std::string value(buffer);
    std::free(buffer);
    return value;
  }
  return std::string();
#else
  const char* value = std::getenv(name);
  return value != nullptr ? std::string(value) : std::string();
#endif
}

std::string temp_root() {
  const std::string value = environment_value(
#if defined(_WIN32)
      "TEMP"
#else
      "TMPDIR"
#endif
  );
  if (!value.empty()) return value;
#if defined(_WIN32)
  return ".";
#else
  return "/tmp";
#endif
}

}  // namespace

std::string make_temp_dir(const std::string& tag) {
  const std::uint64_t serial = g_temp_serial.fetch_add(1) + 1;
#if defined(_WIN32)
  const auto pid = static_cast<std::uint64_t>(_getpid());
#else
  const auto pid = static_cast<std::uint64_t>(getpid());
#endif
  std::string path = temp_root();
  path.push_back('/');
  path.append("itf-");
  path.append(tag);
  path.push_back('-');
  path.append(std::to_string(pid));
  path.push_back('-');
  path.append(std::to_string(serial));
#if defined(_WIN32)
  (void)CreateDirectoryA(path.c_str(), nullptr);
#else
  (void)::mkdir(path.c_str(), 0700);
#endif
  return path;
}

void remove_dir(const std::string& path) {
  if (path.empty()) return;
#if defined(_WIN32)
  const std::string pattern = path + "\\*";
  WIN32_FIND_DATAA data{};
  HANDLE handle = FindFirstFileA(pattern.c_str(), &data);
  if (handle != INVALID_HANDLE_VALUE) {
    do {
      const std::string name(data.cFileName);
      if (name == "." || name == "..") continue;
      const std::string child = path + "\\" + name;
      if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        remove_dir(child);
      } else {
        (void)DeleteFileA(child.c_str());
      }
    } while (FindNextFileA(handle, &data) != 0);
    (void)FindClose(handle);
  }
  (void)RemoveDirectoryA(path.c_str());
#else
  const std::string command = "rm -rf '" + path + "'";
  (void)std::system(command.c_str());
#endif
}

std::uint16_t find_free_port() {
  auto listener = net::TcpListener::bind(net::ListenConfig{"127.0.0.1", 0, 8});
  if (!listener.ok()) return 0;
  const std::uint16_t port = listener.value().port();
  listener.value().close();
  return port;
}

bool file_exists(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  return file.good();
}

std::string read_text_file(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file.good()) return std::string();
  std::string text;
  std::getline(file, text);
  return text;
}

void write_text_file(const std::string& path, const std::string& text) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file << text;
}

std::uint64_t file_size(const std::string& path) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file.good()) return 0;
  return static_cast<std::uint64_t>(file.tellg());
}

void truncate_file(const std::string& path, std::uint64_t bytes) {
  std::ifstream source(path, std::ios::binary);
  if (!source.good()) return;
  std::vector<char> buffer(static_cast<std::size_t>(bytes));
  source.read(buffer.data(), static_cast<std::streamsize>(bytes));
  const std::streamsize read = source.gcount();
  source.close();
  std::ofstream target(path, std::ios::binary | std::ios::trunc);
  target.write(buffer.data(), read);
}

void flip_byte(const std::string& path, std::uint64_t offset) {
  std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
  if (!file.good()) return;
  file.seekg(static_cast<std::streamoff>(offset));
  char value = 0;
  file.read(&value, 1);
  value = static_cast<char>(value ^ 0x5A);
  file.seekp(static_cast<std::streamoff>(offset));
  file.write(&value, 1);
}

std::string executable_directory() {
#if defined(_WIN32)
  char buffer[MAX_PATH] = {0};
  const DWORD length = GetModuleFileNameA(nullptr, buffer, MAX_PATH);
  if (length == 0) return ".";
  std::string path(buffer, length);
#else
  char buffer[4096] = {0};
  const ssize_t length = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
  if (length <= 0) return ".";
  std::string path(buffer, static_cast<std::size_t>(length));
#endif
  const std::size_t separator = path.find_last_of("/\\");
  if (separator == std::string::npos) return ".";
  return path.substr(0, separator);
}

std::string tool_path(const std::string& tool_name) {
  std::string path = executable_directory();
  path.push_back('/');
  path.append(tool_name);
#if defined(_WIN32)
  path.append(".exe");
#endif
  return path;
}

ChildProcess::~ChildProcess() {
  if (valid() && !reaped_) kill();
}

ChildProcess::ChildProcess(ChildProcess&& other) noexcept
    : handle_(other.handle_), pid_(other.pid_), reaped_(other.reaped_), exit_code_(other.exit_code_) {
  other.handle_ = 0;
  other.pid_ = 0;
  other.reaped_ = true;
}

ChildProcess& ChildProcess::operator=(ChildProcess&& other) noexcept {
  if (this != &other) {
    if (valid() && !reaped_) kill();
    handle_ = other.handle_;
    pid_ = other.pid_;
    reaped_ = other.reaped_;
    exit_code_ = other.exit_code_;
    other.handle_ = 0;
    other.pid_ = 0;
    other.reaped_ = true;
  }
  return *this;
}

bool ChildProcess::valid() const noexcept { return handle_ != 0; }

Result<ChildProcess> ChildProcess::spawn(const std::string& executable,
                                         const std::vector<std::string>& arguments) {
  if (!file_exists(executable)) {
    return Result<ChildProcess>::failure(StatusCode::NotFound,
                                         "child executable does not exist: " + executable);
  }
  std::string command = "\"" + executable + "\"";
  for (const std::string& argument : arguments) {
    command.push_back(' ');
    command.push_back('"');
    command.append(argument);
    command.push_back('"');
  }
#if defined(_WIN32)
  STARTUPINFOA startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION information{};
  std::vector<char> mutable_command(command.begin(), command.end());
  mutable_command.push_back('\0');
  if (CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr,
                     &startup, &information) == 0) {
    return Result<ChildProcess>::failure(StatusCode::IoError, "CreateProcess failed");
  }
  (void)CloseHandle(information.hThread);
  ChildProcess child;
  child.handle_ = reinterpret_cast<std::intptr_t>(information.hProcess);
  child.pid_ = static_cast<std::uint64_t>(information.dwProcessId);
  return Result<ChildProcess>::success(std::move(child));
#else
  const pid_t pid = ::fork();
  if (pid < 0) {
    return Result<ChildProcess>::failure(StatusCode::IoError, "fork failed");
  }
  if (pid == 0) {
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(executable.c_str()));
    for (const std::string& argument : arguments) {
      argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);
    ::execv(executable.c_str(), argv.data());
    _exit(127);
  }
  ChildProcess child;
  child.pid_ = static_cast<std::uint64_t>(pid);
  child.handle_ = static_cast<std::intptr_t>(pid);
  return Result<ChildProcess>::success(std::move(child));
#endif
}

bool ChildProcess::running() {
  if (!valid() || reaped_) return false;
#if defined(_WIN32)
  const DWORD state = WaitForSingleObject(reinterpret_cast<HANDLE>(handle_), 0);
  return state == WAIT_TIMEOUT;
#else
  int status = 0;
  const pid_t result = ::waitpid(static_cast<pid_t>(pid_), &status, WNOHANG);
  if (result == 0) return true;
  if (result == static_cast<pid_t>(pid_)) {
    reaped_ = true;
    exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return false;
  }
  return false;
#endif
}

int ChildProcess::wait() {
  if (!valid()) return -1;
  if (reaped_) return exit_code_;
#if defined(_WIN32)
  (void)WaitForSingleObject(reinterpret_cast<HANDLE>(handle_), INFINITE);
  DWORD code = 0;
  (void)GetExitCodeProcess(reinterpret_cast<HANDLE>(handle_), &code);
  (void)CloseHandle(reinterpret_cast<HANDLE>(handle_));
  handle_ = 0;
  reaped_ = true;
  exit_code_ = static_cast<int>(code);
  return exit_code_;
#else
  int status = 0;
  (void)::waitpid(static_cast<pid_t>(pid_), &status, 0);
  reaped_ = true;
  exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  return exit_code_;
#endif
}

void ChildProcess::kill() {
  if (!valid()) return;
#if defined(_WIN32)
  (void)TerminateProcess(reinterpret_cast<HANDLE>(handle_), 137);
  (void)WaitForSingleObject(reinterpret_cast<HANDLE>(handle_), INFINITE);
  (void)CloseHandle(reinterpret_cast<HANDLE>(handle_));
  handle_ = 0;
  reaped_ = true;
  exit_code_ = 137;
#else
  (void)::kill(static_cast<pid_t>(pid_), SIGKILL);
  int status = 0;
  (void)::waitpid(static_cast<pid_t>(pid_), &status, 0);
  handle_ = 0;
  reaped_ = true;
  exit_code_ = -1;
#endif
}

bool wait_until(const std::function<bool()>& predicate, std::uint64_t budget_ms) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budget_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return predicate();
}

RequestId make_request_id(std::uint64_t seed, std::uint64_t index) {
  const std::uint64_t mixed = seed * 0x9E3779B97F4A7C15ULL + index * 0xBF58476D1CE4E5B9ULL;
  RequestId id(mixed ^ 0xA5A5A5A5A5A5A5A5ULL, mixed * 0x94D049BB133111EBULL + 1ULL);
  return id;
}

AttemptId make_attempt_id(std::uint64_t seed, std::uint64_t index) {
  const std::uint64_t mixed = seed * 0xD6E8FEB86659FD93ULL + index * 0x2545F4914F6CDD1DULL;
  AttemptId id(mixed ^ 0x5A5A5A5A5A5A5A5AULL, mixed * 0xC2B2AE3D27D4EB4FULL + 1ULL);
  return id;
}

StateTransferId make_transfer_id(std::uint64_t seed, std::uint64_t index) {
  const std::uint64_t mixed = seed * 0x27D4EB2F165667C5ULL + index * 0x9E3779B97F4A7C15ULL;
  StateTransferId id(mixed ^ 0x3C3C3C3C3C3C3C3CULL, mixed * 0x100000001B3ULL + 1ULL);
  return id;
}

}  // namespace itf::test
