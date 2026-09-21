// Degraded Mode Fabric - scale and benchmark suite.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Measures completed work, not submission latency, at several sizes so an
// accidental O(N^2) shows up as a ratio. Retained state must stay bounded and
// accounting must close exactly at every size.
#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdio>
#include <string>
#include <vector>

#include "testkit/fixtures.hpp"
#include "testkit/testkit.hpp"

using namespace dmf;
using namespace dmf::test;

namespace {

/// Prints immediately: the benchmark reports progress so a slow phase is
/// identifiable from the output alone, without a watchdog.
void trace(const char* format, ...) {
  va_list arguments;
  va_start(arguments, format);
  std::vfprintf(stdout, format, arguments);
  va_end(arguments);
  std::fflush(stdout);
}

struct Measurement {
  std::uint64_t contracts = 0;
  double allocate_seconds = 0;
  double evaluate_seconds = 0;
  double publish_seconds = 0;
  std::uint64_t nodes = 0;
  std::uint64_t retained_grants = 0;
  std::uint64_t retained_fences = 0;
  std::uint64_t live_grants = 0;
};

double seconds_since(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

Measurement measure(std::uint64_t count, std::size_t max_live_grants) {
  Measurement measurement;
  measurement.contracts = count;
  TempDir directory{"scale"};
  CoordinatorConfig config;
  config.store.root = directory.path();
  config.allow_anonymous_sessions = true;
  config.max_live_grants = max_live_grants;
  config.store.max_retained_grants = max_live_grants;
  config.store.max_records_per_segment = 1U << 30;  // keep one segment for this run
  auto created = Coordinator::create(config);
  if (!created.ok()) return measurement;
  std::unique_ptr<Coordinator> coordinator = std::move(created.value());
  (void)coordinator->install_policy(demonstration_policy(PolicyGeneration::from_value(1), 1000000));

  const std::uint64_t scopes = 50;
  std::vector<ServiceContract> definitions;
  definitions.reserve(static_cast<std::size_t>(count));
  for (std::uint64_t i = 1; i <= count; ++i) {
    const ServiceClass service_class =
        (i % 11 == 0) ? ServiceClass::Protected : ((i % 3 == 0) ? ServiceClass::Standard
                                                                : ServiceClass::BestEffort);
    const std::uint64_t bandwidth = 500 + (i * 37U) % 20000U;
    // The concession floor must itself be a weakening of the original value,
    // otherwise the contract definition is contradictory.
    const std::uint64_t floor = std::min<std::uint64_t>(200 + (i * 11U) % 500U, bandwidth);
    definitions.push_back(contract(i, service_class, static_cast<std::uint32_t>((i * 7919U) % 1000U),
                                   bandwidth, 500 + (i * 13U) % 4000U, floor, 4, 1 + (i % scopes)));
  }
  {
    const auto registration_start = std::chrono::steady_clock::now();
    const Status registered = coordinator->register_contracts(definitions);
    if (!registered.ok()) {
      trace("scale registration failed at %llu: %s\n",
            static_cast<unsigned long long>(count), registered.render().c_str());
    }
    DMF_CHECK(registered.ok());
    trace("scale %llu registered in %.3fs\n", static_cast<unsigned long long>(count),
                seconds_since(registration_start));
  }

  const auto publish_start = std::chrono::steady_clock::now();
  for (std::uint64_t scope = 1; scope <= scopes; ++scope) {
    CapabilitySpec spec;
    spec.bandwidth_kbps = 20000;
    spec.rtt_us = 2000;
    spec.scope = scope;
    (void)publish_capability(*coordinator, scope, 1, spec);
  }
  measurement.publish_seconds = seconds_since(publish_start);
  trace("scale %llu published in %.3fs\n", static_cast<unsigned long long>(count),
              measurement.publish_seconds);

  const auto evaluate_start = std::chrono::steady_clock::now();
  std::uint64_t nodes = 0;
  const std::uint64_t sample = std::min<std::uint64_t>(count, 500);
  for (std::uint64_t i = 0; i < sample; ++i) {
    const std::uint64_t id = 1 + (i * count) / std::max<std::uint64_t>(sample, 1);
    auto decision = evaluate_view(*coordinator, contract_id(std::min<std::uint64_t>(id, count)));
    if (decision.ok()) nodes += decision.value().plan.work.nodes_visited;
  }
  measurement.evaluate_seconds = seconds_since(evaluate_start);
  measurement.nodes = nodes;
  measurement.allocate_seconds = measurement.evaluate_seconds;
  trace("scale %llu evaluated %llu in %.3fs\n", static_cast<unsigned long long>(count),
              static_cast<unsigned long long>(sample), measurement.evaluate_seconds);

  const std::uint64_t grant_target = std::min<std::uint64_t>(count / 2 + 1, 1200);
  const auto acquire_start = std::chrono::steady_clock::now();
  for (std::uint64_t i = 1; i <= grant_target; ++i) {
    const std::uint64_t id = 1 + ((i - 1) * count) / grant_target;
    (void)coordinator->acquire(contract_id(std::min<std::uint64_t>(id, count)), coordinator->now());
    if (i % 250 == 0) {
      std::fflush(stdout);
      trace("scale %llu acquire %llu/%llu at %.3fs\n",
                  static_cast<unsigned long long>(count), static_cast<unsigned long long>(i),
                  static_cast<unsigned long long>(grant_target), seconds_since(acquire_start));
    }
  }
  trace("scale %llu acquired %llu\n", static_cast<unsigned long long>(count),
              static_cast<unsigned long long>(grant_target));
  const StatusBody body = coordinator->describe(coordinator->now());
  measurement.retained_grants = body.retained_grants;
  measurement.retained_fences = body.retained_fences;
  measurement.live_grants = body.live_grants;
  DMF_CHECK(body.accounting_closed);
  DMF_CHECK_EQ(body.retained_contracts, count);

  // Every retained table is bounded.
  DMF_CHECK(measurement.retained_grants <= max_live_grants);
  DMF_CHECK(measurement.retained_fences <= config.store.max_retained_fences);
  const auto shutdown_start = std::chrono::steady_clock::now();
  (void)coordinator->stop();
  trace("scale %llu shutdown in %.3fs\n", static_cast<unsigned long long>(count),
              seconds_since(shutdown_start));
  return measurement;
}

void report(const Measurement& measurement) {
  char buffer[320];
  std::snprintf(buffer, sizeof(buffer),
                "scale contracts=%llu evaluate=%.4fs publishes=%.4fs nodes=%llu live=%llu "
                "retained=%llu fences=%llu",
                static_cast<unsigned long long>(measurement.contracts), measurement.evaluate_seconds,
                measurement.publish_seconds,
                static_cast<unsigned long long>(measurement.nodes),
                static_cast<unsigned long long>(measurement.live_grants),
                static_cast<unsigned long long>(measurement.retained_grants),
                static_cast<unsigned long long>(measurement.retained_fences));
  trace("%s\n", buffer);
}

}  // namespace

DMF_TEST(scale, thousands_of_contracts_stay_bounded_and_close) {
  const std::uint64_t sizes[] = {500, 1000, 2000, 4000};
  std::vector<Measurement> measurements;
  for (const std::uint64_t size : sizes) {
    const Measurement measurement = measure(size, 2048);
    report(measurement);
    DMF_CHECK_EQ(measurement.contracts, size);
    DMF_CHECK(measurement.live_grants <= std::uint64_t{2048});
    measurements.push_back(measurement);
  }
  // Doubling the number of contracts must not square the evaluation cost. The
  // bound is deliberately loose: it exists to catch an accidental quadratic, not
  // to benchmark the host.
  for (std::size_t i = 1; i < measurements.size(); ++i) {
    const double previous = measurements[i - 1].evaluate_seconds;
    const double current = measurements[i].evaluate_seconds;
    if (previous <= 0.0) continue;
    DMF_CHECK(current <= previous * 6.0 + 1.0);
  }
}

DMF_TEST(scale, retention_bounds_hold_under_sustained_grant_pressure) {
  const Measurement measurement = measure(1500, 256);
  report(measurement);
  DMF_CHECK(measurement.live_grants <= std::uint64_t{256});
  DMF_CHECK(measurement.retained_grants <= std::uint64_t{256});
}

DMF_TEST(scale, a_snapshot_over_a_large_state_round_trips) {
  TempDir directory{"scalesnapshot"};
  CoordinatorConfig config;
  config.store.root = directory.path();
  config.allow_anonymous_sessions = true;
  auto created = Coordinator::create(config);
  DMF_CHECK(created.ok());
  std::unique_ptr<Coordinator> coordinator = std::move(created.value());
  (void)coordinator->install_policy(demonstration_policy());
  constexpr std::uint64_t kContracts = 2000;
  std::vector<ServiceContract> definitions;
  definitions.reserve(static_cast<std::size_t>(kContracts));
  for (std::uint64_t i = 1; i <= kContracts; ++i) {
    definitions.push_back(contract(i, ServiceClass::Standard, 10, 1000 + i, 2000, 500, 3));
  }
  (void)coordinator->register_contracts(definitions);
  DMF_CHECK_OK(coordinator->stop());

  StoreConfig store_config;
  store_config.root = directory.path();
  store_config.read_only = true;
  auto store = StateStore::open(store_config);
  DMF_CHECK(store.ok());
  DMF_CHECK_EQ(store.value()->state().contracts.size(), static_cast<std::size_t>(kContracts));
  DMF_CHECK_EQ(store.value()->recovery(), RecoveryOutcome::CleanReopen);
  const DurableState& state = store.value()->state();
  const Accounting accounting(state.counters);
  ClosureInputs inputs;
  inputs.live_grants = state.live_grant_count();
  inputs.retained_terminated_grants = state.terminated_grant_count();
  inputs.retained_fences = state.fences.size();
  inputs.retained_decisions = state.decisions.size();
  DMF_CHECK(accounting.check_closure(inputs).closed);
  DMF_CHECK_OK(store.value()->close());
}