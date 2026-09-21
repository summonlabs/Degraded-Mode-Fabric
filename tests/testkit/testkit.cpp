// Degraded Mode Fabric - test harness implementation.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "testkit/testkit.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <system_error>

namespace dmf::test {

namespace {
std::atomic<std::uint64_t> g_temp_counter{0};
}

void fail(const char* file, int line, const std::string& message) {
  std::string text(file);
  text.push_back(':');
  text.append(std::to_string(line));
  text.append(": ");
  text.append(message);
  throw AssertionFailure{text};
}

std::string show(std::uint64_t value) { return std::to_string(value); }

std::uint32_t process_id() noexcept {
#ifdef _WIN32
  return static_cast<std::uint32_t>(::GetCurrentProcessId());
#else
  return static_cast<std::uint32_t>(::getpid());
#endif
}

std::vector<TestCase>& registry() {
  static std::vector<TestCase> tests;
  return tests;
}

Registrar::Registrar(const char* suite, const char* name, std::function<void()> body) {
  registry().push_back(TestCase{suite, name, std::move(body)});
}

int run_all(int argc, char** argv) {
  std::string filter;
  if (argc > 1) filter = argv[1];
  std::vector<TestCase> tests = registry();
  std::sort(tests.begin(), tests.end(), [](const TestCase& a, const TestCase& b) {
    if (a.suite != b.suite) return a.suite < b.suite;
    return a.name < b.name;
  });
  std::size_t passed = 0;
  std::vector<std::string> failures;
  for (const TestCase& test : tests) {
    const std::string label = test.suite + "." + test.name;
    if (!filter.empty() && label.find(filter) == std::string::npos) continue;
    try {
      test.body();
      ++passed;
      std::cout << "[ PASS ] " << label << '\n' << std::flush;
    } catch (const AssertionFailure& failure) {
      failures.push_back(label + " -> " + failure.message);
      std::cout << "[ FAIL ] " << label << " -> " << failure.message << '\n' << std::flush;
    } catch (const std::exception& error) {
      failures.push_back(label + " -> unexpected exception: " + error.what());
      std::cout << "[ FAIL ] " << label << " -> unexpected exception: " << error.what() << '\n' << std::flush;
    } catch (...) {
      failures.push_back(label + " -> unknown exception");
      std::cout << "[ FAIL ] " << label << " -> unknown exception\n" << std::flush;
    }
  }
  std::cout << "\n" << passed << " passed, " << failures.size() << " failed, " << tests.size()
            << " registered\n";
  if (!failures.empty()) {
    std::cout << "failures:\n";
    for (const std::string& failure : failures) std::cout << "  " << failure << '\n';
    return 1;
  }
  return 0;
}

TempDir::TempDir(const std::string& label) {
  const std::uint64_t counter = g_temp_counter.fetch_add(1);
  std::error_code error;
  const std::filesystem::path base = std::filesystem::current_path(error);
  // The process identity is part of the name so two suites running at once can
  // never collide on a scratch directory.
  const std::uint64_t process = static_cast<std::uint64_t>(::dmf::test::process_id());
  std::filesystem::path candidate =
      base / ("dmf-" + label + "-" + std::to_string(process) + "-" + std::to_string(counter));
  std::filesystem::remove_all(candidate, error);
  std::filesystem::create_directories(candidate, error);
  path_ = candidate.string();
}

TempDir::~TempDir() {
  std::error_code error;
  std::filesystem::remove_all(path_, error);
}

std::string TempDir::file(const std::string& name) const {
  return (std::filesystem::path(path_) / name).string();
}

std::uint64_t Rng::next() noexcept {
  // splitmix64: tiny, deterministic and well distributed.
  state_ += 0x9E3779B97F4A7C15ULL;
  std::uint64_t value = state_;
  value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ULL;
  value = (value ^ (value >> 27)) * 0x94D049BB133111EBULL;
  return value ^ (value >> 31);
}

std::uint64_t Rng::below(std::uint64_t bound) noexcept {
  if (bound == 0) return 0;
  return next() % bound;
}

std::uint32_t Rng::u32(std::uint32_t bound) noexcept {
  if (bound == 0) return 0;
  return static_cast<std::uint32_t>(below(bound));
}

bool Rng::coin() noexcept { return (next() & 1U) != 0U; }

}  // namespace dmf::test

int main(int argc, char** argv) { return ::dmf::test::run_all(argc, argv); }