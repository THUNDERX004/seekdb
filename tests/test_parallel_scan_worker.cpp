// Standalone C++20 test; intentionally independent of all database sources.
#include "../src/parallel_scan_worker.hpp"

#include <atomic>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <latch>
#include <stdexcept>
#include <type_traits>

namespace allocation_probe {
std::atomic<std::size_t> count{0};
thread_local bool fail_next = false;
}  // namespace allocation_probe

void *operator new(std::size_t bytes) {
  if (allocation_probe::fail_next) {
    allocation_probe::fail_next = false;
    throw std::bad_alloc();
  }
  if (void *memory = std::malloc(bytes == 0 ? 1 : bytes)) {
    allocation_probe::count.fetch_add(1, std::memory_order_relaxed);
    return memory;
  }
  throw std::bad_alloc();
}
void *operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete(void *memory) noexcept { std::free(memory); }
void operator delete[](void *memory) noexcept { std::free(memory); }
void operator delete(void *memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void *memory, std::size_t) noexcept { std::free(memory); }

namespace {
using mvd::ParallelScanWorker;
static_assert(!std::is_copy_constructible_v<ParallelScanWorker>);
static_assert(!std::is_move_constructible_v<ParallelScanWorker>);

void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

template <class First, class Second>
void run(ParallelScanWorker &worker, First &&local, Second &&remote) {
  require(worker.run_pair(std::forward<First>(local), std::forward<Second>(remote)),
          "unexpected worker startup failure");
}

void startup_failure_and_retry() {
  ParallelScanWorker worker;
  int local = 0, remote = 0;
  allocation_probe::fail_next = true;
  bool started;
  try {
    started = worker.run_pair([&] { ++local; }, [&] { ++remote; });
  } catch (...) {
    allocation_probe::fail_next = false;
    throw;
  }
  const bool injected = !std::exchange(allocation_probe::fail_next, false);
  // Standard libraries commonly allocate the initial std::thread state through
  // operator new. Implementations without that allocation still test startup.
  if (injected) {
    require(!started && local == 0 && remote == 0,
            "failed startup invoked a task or reported success");
  } else {
    require(started && local == 1 && remote == 1,
            "allocation-free thread startup lost a task");
  }
  const int before = local;
  run(worker, [&] { ++local; }, [&] { ++remote; });
  require(local == before + 1 && remote == before + 1,
          "retry after startup failure lost or duplicated a task");
}

void repeated_same_worker_without_allocations() {
  ParallelScanWorker worker;
  const auto caller = std::this_thread::get_id();
  std::thread::id worker_id;
  int local = 0, remote = 0;
  run(worker, [&] { ++local; }, [&] {
    worker_id = std::this_thread::get_id();
    ++remote;
  });
  require(worker_id != caller, "remote ran on submitting thread");
  const auto allocations = allocation_probe::count.load(std::memory_order_relaxed);
  for (int round = 0; round < 128; ++round) {
    auto first = [&] {
      require(std::this_thread::get_id() == caller, "local changed threads");
      ++local;
    };
    auto second = [&] {
      require(std::this_thread::get_id() == worker_id, "worker was recreated");
      ++remote;
    };
    run(worker, first, second);
    require(local == round + 2 && remote == round + 2,
            "repeated pair did not execute both tasks exactly once");
  }
  require(allocation_probe::count.load(std::memory_order_relaxed) == allocations,
          "warm submissions performed a heap allocation");
}

struct LocalFailure {};
struct RemoteFailure {};

void exceptions_and_reuse() {
  ParallelScanWorker worker;
  for (unsigned failures : {1U, 2U, 3U}) {
    std::latch entered(2);
    int local = 0, remote = 0;
    int caught = 0;
    try {
      run(worker, [&] {
        ++local;
        entered.arrive_and_wait();
        if (failures & 1) throw LocalFailure{};
      }, [&] {
        ++remote;
        entered.arrive_and_wait();
        if (failures & 2) throw RemoteFailure{};
      });
    } catch (const LocalFailure &) {
      caught = 1;
    } catch (const RemoteFailure &) {
      caught = 2;
    }
    require(caught == ((failures & 1) ? 1 : 2),
            "exception missing or local exception precedence changed");
    require(local == 1 && remote == 1, "exception cancelled or duplicated a task");
    run(worker, [&] { ++local; }, [&] { ++remote; });
    require(local == 2 && remote == 2, "worker unusable after task exception");
  }
}

void resource_exceptions_are_task_failures() {
  ParallelScanWorker worker;
  run(worker, [] {}, [] {});
  for (int side = 0; side < 2; ++side) {
    for (int kind = 1; kind <= 2; ++kind) {
      int local = 0, remote = 0, caught = 0;
      const auto raise = [&] {
        if (kind == 1) throw std::bad_alloc();
        throw std::system_error(std::make_error_code(std::errc::operation_canceled));
      };
      try {
        run(worker, [&] {
          ++local;
          if (side == 0) raise();
        }, [&] {
          ++remote;
          if (side == 1) raise();
        });
      } catch (const std::bad_alloc &) {
        caught = 1;
      } catch (const std::system_error &) {
        caught = 2;
      }
      require(caught == kind && local == 1 && remote == 1,
              "task resource exception became startup fallback or lost a task");
      run(worker, [] {}, [] {});
    }
  }
}

struct SignalOnExit {
  std::latch &signal;
  ~SignalOnExit() { signal.count_down(); }
};

struct BorrowedContext {
  std::atomic<bool> &remote_done;
  bool &completed_before_destruction;
  int value = 0;
  ~BorrowedContext() {
    completed_before_destruction =
        remote_done.load(std::memory_order_acquire) && value == 73;
  }
};

void borrowed_context_survives_completion_and_unwinding() {
  ParallelScanWorker worker;
  for (bool fail_local : {false, true}) {
    std::latch local_exited(1);
    std::atomic<bool> remote_done{false};
    bool completed_before_destruction = false, caught = false;
    try {
      BorrowedContext context{remote_done, completed_before_destruction};
      run(worker, [&] {
        SignalOnExit signal{local_exited};
        if (fail_local) throw LocalFailure{};
      }, [&] {
        // Remote cannot complete until local has returned or unwound.
        local_exited.wait();
        context.value = 73;
        remote_done.store(true, std::memory_order_release);
      });
      require(context.value == 73 && remote_done.load(),
              "run_pair returned before borrowed-context write completed");
    } catch (const LocalFailure &) {
      caught = true;
    }
    require(caught == fail_local && completed_before_destruction,
            "borrowed context was destroyed before remote completed");
  }
}

struct NonMovableRvalueTask {
  int &calls;
  explicit NonMovableRvalueTask(int &value) : calls(value) {}
  NonMovableRvalueTask(const NonMovableRvalueTask &) = delete;
  NonMovableRvalueTask(NonMovableRvalueTask &&) = delete;
  void operator()() && noexcept { ++calls; }
};

void forwarding_without_task_copies() {
  ParallelScanWorker worker;
  int local = 0, remote = 0;
  NonMovableRvalueTask first(local), second(remote);
  run(worker, std::move(first), std::move(second));
  require(local == 1 && remote == 1, "borrowed rvalue-only tasks lost forwarding");
}

void idle_and_busy_destruction() {
  { ParallelScanWorker never_started; }
  for (int round = 0; round < 16; ++round) {
    std::atomic<unsigned> completed{0};
    std::latch entered(2);
    {
      ParallelScanWorker worker;
      run(worker, [&] {
        entered.arrive_and_wait();
        completed.fetch_or(1);
      }, [&] {
        entered.arrive_and_wait();
        completed.fetch_or(2);
      });
      // Destroy immediately after both tasks finish: the worker may still be
      // returning to its idle wait, so stop must not lose its notification.
    }
    require(completed.load() == 3, "destruction lost a completed pair");
  }
}
}  // namespace

int main() {
  try {
    startup_failure_and_retry();
    repeated_same_worker_without_allocations();
    exceptions_and_reuse();
    resource_exceptions_are_task_failures();
    borrowed_context_survives_completion_and_unwinding();
    forwarding_without_task_copies();
    idle_and_busy_destruction();
    std::cout << "parallel scan worker tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "parallel scan worker test failed: " << error.what() << '\n';
    return 1;
  } catch (...) {
    std::cerr << "parallel scan worker test failed: unexpected task exception\n";
    return 1;
  }
}
