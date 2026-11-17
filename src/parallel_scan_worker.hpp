#ifndef MVD_PARALLEL_SCAN_WORKER_HPP
#define MVD_PARALLEL_SCAN_WORKER_HPP

#include <atomic>
#include <chrono>
#include <exception>
#include <functional>
#include <memory>
#include <new>
#include <system_error>
#include <thread>
#include <utility>

namespace mvd {

// One serial client, one persistent worker. No concurrent/reentrant run_pair,
// and destruction must follow the client's last completed call. Task objects
// are borrowed until both invocations finish; normal submissions allocate no
// storage. Allocation inside tasks or exception machinery is outside this API.
class ParallelScanWorker {
 public:
  ParallelScanWorker() = default;
  ParallelScanWorker(const ParallelScanWorker &) = delete;
  ParallelScanWorker &operator=(const ParallelScanWorker &) = delete;
  ParallelScanWorker(ParallelScanWorker &&) = delete;
  ParallelScanWorker &operator=(ParallelScanWorker &&) = delete;

  ~ParallelScanWorker() {
    if (!worker_.joinable()) return;
    state_.store(stopping, std::memory_order_release);
    state_.notify_one();
    worker_.join();
  }

  // false leaves both callables untouched: startup failed or a small scan
  // should run serially while the worker is parked/warming. An accepted pair
  // invokes both exactly once. permit_parked=false uses this adaptive policy.
  // Await remote even if local throws. If both throw, local takes precedence.
  template <class First, class Second>
  bool run_pair(First &&local, Second &&remote, bool permit_parked=true) {
    bool recent=false;
    if (!permit_parked) {
      const auto now=std::chrono::steady_clock::now();
      recent=now-last_small_submission_<std::chrono::microseconds(256);
      last_small_submission_=now;
      if (unprofitable_submissions_!=0) {
        --unprofitable_submissions_;
        return false;
      }
    }
    if (!worker_.joinable()) {
      try {
        worker_ = std::thread(&ParallelScanWorker::workerLoop, this);
      } catch (const std::system_error &) {
        return false;
      } catch (const std::bad_alloc &) {
        return false;
      }
      // A small cold scan falls back while its new worker becomes ready.
      if (!permit_parked) return false;
    }
    const unsigned observed=state_.load(std::memory_order_acquire);
    if (observed==warming) return false;
    if (!permit_parked && observed==parked) {
      // A single request after a pause keeps the serial path. Rearm only
      // after another nearby request demonstrates an actual query burst.
      if (!recent) return false;
      unsigned expected=parked;
      if (state_.compare_exchange_strong(expected,warming,std::memory_order_acq_rel))
        state_.notify_one();
      return false;
    }

    const auto pair_start=permit_parked?std::chrono::steady_clock::time_point{}:
        std::chrono::steady_clock::now();
    struct Task {
      Second &&callable;
      std::exception_ptr error;
    } task{std::forward<Second>(remote), {}};
    {
      context_ = std::addressof(task);
      invoke_ = [](void *context) noexcept {
        auto &job = *static_cast<Task *>(context);
        try {
          std::invoke(std::forward<Second>(job.callable));
        } catch (...) {
          job.error = std::current_exception();
        }
      };
      if (!permit_parked) {
        // Race task admission against idle-to-parked on the same atomic.
        // Publishing pending only after context initialization makes every
        // accepted task visible in full. A lost race invokes neither callback.
        unsigned expected=idle;
        if (!state_.compare_exchange_strong(expected,pending,std::memory_order_acq_rel)) {
          context_=nullptr;
          invoke_=nullptr;
          if (recent && expected==parked) {
            expected=parked;
            if (state_.compare_exchange_strong(expected,warming,std::memory_order_acq_rel))
              state_.notify_one();
          }
          return false;
        }
      } else {
        state_.store(pending,std::memory_order_release);
      }
    }
    state_.notify_one();

    const auto local_start=permit_parked?std::chrono::steady_clock::time_point{}:
        std::chrono::steady_clock::now();
    std::exception_ptr local_error;
    try {
      std::invoke(std::forward<First>(local));
    } catch (...) {
      local_error = std::current_exception();
    }
    const auto local_done=permit_parked?std::chrono::steady_clock::time_point{}:
        std::chrono::steady_clock::now();
    for (unsigned i=0;i<256 && state_.load(std::memory_order_acquire)==pending;++i)
      pause();
    while (state_.load(std::memory_order_acquire)==pending)
      state_.wait(pending,std::memory_order_acquire);
    // The release/acquire handoff publishes task.error and remote writes before
    // task/callable/context can be destroyed by return or exception unwinding.
    if (!permit_parked) {
      const auto pair_done=std::chrono::steady_clock::now();
      // A short range must amortize scheduling and handoff on this machine.
      // Both scans have already completed; this only changes later admission.
      if (pair_done-pair_start > (local_done-local_start)*2+std::chrono::microseconds(1))
        unprofitable_submissions_=64;
    }
    if (local_error) std::rethrow_exception(local_error);
    if (task.error) std::rethrow_exception(task.error);
    return true;
  }

 private:
  static void pause() noexcept {
#if (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(__i386__))
    __builtin_ia32_pause();
#else
    std::this_thread::yield();
#endif
  }
  void workerLoop() noexcept {
    for (;;) {
      unsigned state=state_.load(std::memory_order_acquire);
      if (state==idle) {
        // Bound idle CPU use even if no next request arrives; then park.
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::microseconds(512);
        do {
          for (unsigned i=0;i<64 && state==idle;++i) {
            pause();state=state_.load(std::memory_order_acquire);
          }
        } while (state==idle && std::chrono::steady_clock::now()<deadline);
        if (state==idle) {
          unsigned expected=idle;
          if (state_.compare_exchange_strong(expected,parked,std::memory_order_acq_rel))
            state_.wait(parked,std::memory_order_acquire);
          continue;
        }
      }
      if (state==stopping) return;
      if (state==warming) {
        unsigned expected=warming;
        state_.compare_exchange_strong(expected,idle,std::memory_order_acq_rel);
        continue;
      }
      if (state==parked) { state_.wait(parked,std::memory_order_acquire);continue; }
      invoke_(context_);
      context_=nullptr;
      invoke_=nullptr;
      state_.store(idle,std::memory_order_release);
      state_.notify_one();
    }
  }

  static constexpr unsigned idle=0,pending=1,stopping=2,parked=3,warming=4;
  unsigned unprofitable_submissions_=0;
  std::chrono::steady_clock::time_point last_small_submission_{};
  alignas(64) std::atomic<unsigned> state_{idle};
  void *context_ = nullptr;
  void (*invoke_)(void *) noexcept = nullptr;
  std::thread worker_;
};

}  // namespace mvd

#endif
