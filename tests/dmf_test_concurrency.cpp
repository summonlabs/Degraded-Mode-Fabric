// Degraded Mode Fabric - concurrency and lifecycle suite.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Deterministic coordination only: barriers and latches, never sleeps. A test
// that needs luck to pass is a test that hides a defect.
#include <atomic>
#include <latch>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "testkit/fixtures.hpp"
#include "testkit/testkit.hpp"

using namespace dmf;
using namespace dmf::test;

namespace {

struct Live {
  TempDir directory{"concurrency"};
  std::unique_ptr<Coordinator> coordinator{};

  Status start() {
    CoordinatorConfig config;
    config.store.root = directory.path();
    config.allow_anonymous_sessions = true;
    auto created = Coordinator::create(config);
    if (!created.ok()) return created.status();
    coordinator = std::move(created.value());
    Status status = coordinator->install_policy(demonstration_policy());
    if (!status.ok()) return status;
    for (std::uint64_t i = 1; i <= 8; ++i) {
      status = coordinator->register_contract(
          contract(i, i == 1 ? ServiceClass::Protected : ServiceClass::Standard,
                   static_cast<std::uint32_t>(100 - i), 4000 * i, 2000, 1000, 3));
      if (!status.ok()) return status;
    }
    CapabilitySpec spec;
    spec.bandwidth_kbps = 60000;
    spec.rtt_us = 2000;
    return publish_capability(*coordinator, 1, 1, spec);
  }

  ~Live() {
    if (coordinator) {
      const Status status = coordinator->stop();
      (void)status;
    }
  }
};

}  // namespace

DMF_TEST(concurrency, many_threads_evaluate_the_same_scope_without_losing_state) {
  Live live;
  DMF_CHECK_OK(live.start());
  constexpr int kThreads = 8;
  constexpr int kIterations = 40;
  std::latch gate(kThreads);
  std::atomic<int> failures{0};
  std::vector<std::thread> workers;
  workers.reserve(kThreads);
  for (int thread = 0; thread < kThreads; ++thread) {
    workers.emplace_back([&live, &gate, &failures, thread]() {
      gate.arrive_and_wait();
      for (int iteration = 0; iteration < kIterations; ++iteration) {
        const std::uint64_t id = 1 + static_cast<std::uint64_t>((thread + iteration) % 8);
        auto decision = evaluate_view(*live.coordinator, contract_id(id));
        if (!decision.ok()) {
          failures.fetch_add(1);
          continue;
        }
        if (!never_exceeds(decision.value().original, decision.value().approved)) {
          failures.fetch_add(1);
        }
        if (decision.value().plan.allocated_bandwidth_kbps >
            decision.value().plan.available_bandwidth_kbps) {
          failures.fetch_add(1);
        }
      }
    });
  }
  for (std::thread& worker : workers) worker.join();
  DMF_CHECK_EQ(failures.load(), 0);
  const StatusBody body = live.coordinator->describe(live.coordinator->now());
  DMF_CHECK(body.accounting_closed);
  DMF_CHECK_EQ(body.retained_contracts, std::uint64_t{8});
}

DMF_TEST(concurrency, concurrent_grants_keep_their_own_attempt_identities) {
  Live live;
  DMF_CHECK_OK(live.start());
  constexpr int kThreads = 6;
  std::latch gate(kThreads);
  std::atomic<int> mismatches{0};
  std::vector<std::thread> workers;
  workers.reserve(kThreads);
  for (int thread = 0; thread < kThreads; ++thread) {
    workers.emplace_back([&live, &gate, &mismatches, thread]() {
      gate.arrive_and_wait();
      const std::uint64_t id = 2 + static_cast<std::uint64_t>(thread % 7);
      auto grant = live.coordinator->acquire(contract_id(id), live.coordinator->now());
      if (!grant.ok()) return;
      auto acknowledged = live.coordinator->acknowledge(
          grant.value().id, grant.value().last_attempt, live.coordinator->now());
      if (!acknowledged.ok()) {
        mismatches.fetch_add(1);
        return;
      }
      if (!(acknowledged.value().sequence > grant.value().sequence)) mismatches.fetch_add(1);
      if (acknowledged.value().last_attempt == grant.value().last_attempt) {
        mismatches.fetch_add(1);
      }
    });
  }
  for (std::thread& worker : workers) worker.join();
  DMF_CHECK_EQ(mismatches.load(), 0);
  DMF_CHECK(live.coordinator->describe(live.coordinator->now()).accounting_closed);
}

DMF_TEST(concurrency, shutdown_releases_blocked_sessions_and_is_idempotent) {
  TempDir directory{"shutdown"};
  CoordinatorConfig config;
  config.store.root = directory.path();
  config.allow_anonymous_sessions = true;
  auto created = Coordinator::create(config);
  DMF_CHECK(created.ok());
  std::unique_ptr<Coordinator> coordinator = std::move(created.value());
  DMF_CHECK_OK(coordinator->start());

  constexpr int kSessions = 6;
  std::latch ready(kSessions);
  std::atomic<int> opened{0};
  std::vector<std::unique_ptr<Client>> clients;
  std::vector<std::thread> threads;
  for (int i = 0; i < kSessions; ++i) {
    clients.push_back(std::make_unique<Client>());
    threads.emplace_back([&, i]() {
      ClientConfig client_config;
      client_config.port = coordinator->port();
      client_config.process = ProcessId::from_value(static_cast<std::uint64_t>(i + 1));
      client_config.boot = BootIncarnation::from_value(1);
      client_config.principal = PrincipalId::from_value(static_cast<std::uint64_t>(i + 1));
      const std::size_t index = static_cast<std::size_t>(i);
      if (!clients[index]->connect(client_config).ok()) return;
      HelloResponse hello;
      if (!clients[index]->handshake(hello).ok()) return;
      opened.fetch_add(1);
      ready.arrive_and_wait();
      // Block on a request that will never be answered because the server is
      // being stopped: the read must be released, not timed out.
      (void)clients[index]->call(MessageType::QueryStatus, {});
    });
  }
  ready.wait();
  DMF_CHECK_EQ(opened.load(), kSessions);
  DMF_CHECK_OK(coordinator->stop());
  for (std::thread& thread : threads) thread.join();
  // A second and third stop are no-ops, never a double close or a hang.
  DMF_CHECK_OK(coordinator->stop());
  DMF_CHECK_OK(coordinator->stop());
  for (std::unique_ptr<Client>& client : clients) {
    const Status status = client->close();
    (void)status;
  }
}

DMF_TEST(concurrency, revalidation_and_evaluation_interleave_safely) {
  Live live;
  DMF_CHECK_OK(live.start());
  std::latch gate(3);
  std::atomic<int> failures{0};
  std::atomic<bool> stop{false};
  std::thread evaluator([&]() {
    gate.arrive_and_wait();
    for (int i = 0; i < 60; ++i) {
      auto decision = evaluate_view(*live.coordinator, contract_id(2));
      if (!decision.ok()) failures.fetch_add(1);
    }
    stop.store(true);
  });
  std::thread sweeper([&]() {
    gate.arrive_and_wait();
    while (!stop.load()) {
      (void)live.coordinator->revalidate(live.coordinator->now());
    }
  });
  std::thread clock([&]() {
    gate.arrive_and_wait();
    while (!stop.load()) {
      (void)live.coordinator->advance(1);
    }
  });
  evaluator.join();
  sweeper.join();
  clock.join();
  DMF_CHECK_EQ(failures.load(), 0);
  DMF_CHECK(live.coordinator->describe(live.coordinator->now()).accounting_closed);
}

DMF_TEST(concurrency, a_client_can_be_closed_while_the_server_is_stopping) {
  TempDir directory{"closerace"};
  CoordinatorConfig config;
  config.store.root = directory.path();
  config.allow_anonymous_sessions = true;
  auto created = Coordinator::create(config);
  DMF_CHECK(created.ok());
  std::unique_ptr<Coordinator> coordinator = std::move(created.value());
  DMF_CHECK_OK(coordinator->start());
  ClientConfig client_config;
  client_config.port = coordinator->port();
  client_config.process = ProcessId::from_value(1);
  client_config.boot = BootIncarnation::from_value(1);
  client_config.principal = PrincipalId::from_value(1);
  Client client;
  DMF_CHECK_OK(client.connect(client_config));
  HelloResponse hello;
  DMF_CHECK_OK(client.handshake(hello));
  std::latch gate(2);
  std::thread stopping([&]() {
    gate.arrive_and_wait();
    (void)coordinator->stop();
  });
  std::thread closing([&]() {
    gate.arrive_and_wait();
    (void)client.close();
  });
  stopping.join();
  closing.join();
  DMF_CHECK_OK(coordinator->stop());
}