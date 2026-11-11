#ifndef MVD_PARALLEL_SCAN_WORKER_HPP
#define MVD_PARALLEL_SCAN_WORKER_HPP

#include <condition_variable>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
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
    {
      std::lock_guard lock(mutex_);
      stopping_ = true;
    }
    ready_.notify_one();
    worker_.join();
  }

  // false means startup failed and neither callable was invoked; the caller
  // may then execute a serial fallback. Once started, both run exactly once.
  // Await remote even if local throws. If both throw, local takes precedence.
  template <class First, class Second>
  bool run_pair(First &&local, Second &&remote) {
    if (!worker_.joinable()) {
      try {
        worker_ = std::thread(&ParallelScanWorker::workerLoop, this);
      } catch (const std::system_error &) {
        return false;
      } catch (const std::bad_alloc &) {
        return false;
      }
    }

    struct Task {
      Second &&callable;
      std::exception_ptr error;
    } task{std::forward<Second>(remote), {}};
    {
      std::lock_guard lock(mutex_);
      context_ = std::addressof(task);
      invoke_ = [](void *context) noexcept {
        auto &job = *static_cast<Task *>(context);
        try {
          std::invoke(std::forward<Second>(job.callable));
        } catch (...) {
          job.error = std::current_exception();
        }
      };
      pending_ = true;
    }
    ready_.notify_one();

    std::exception_ptr local_error;
    try {
      std::invoke(std::forward<First>(local));
    } catch (...) {
      local_error = std::current_exception();
    }
    {
      std::unique_lock lock(mutex_);
      finished_.wait(lock, [this] { return !pending_; });
    }
    // The mutex handoff publishes both task.error and all remote writes before
    // task/callable/context can be destroyed by return or exception unwinding.
    if (local_error) std::rethrow_exception(local_error);
    if (task.error) std::rethrow_exception(task.error);
    return true;
  }

 private:
  void workerLoop() {
    for (;;) {
      void *context;
      void (*invoke)(void *) noexcept;
      {
        std::unique_lock lock(mutex_);
        ready_.wait(lock, [this] { return stopping_ || pending_; });
        if (!pending_) return;
        context = context_;
        invoke = invoke_;
      }
      invoke(context);
      {
        std::lock_guard lock(mutex_);
        context_ = nullptr;
        invoke_ = nullptr;
        pending_ = false;
      }
      finished_.notify_one();
    }
  }

  std::mutex mutex_;
  std::condition_variable ready_;
  std::condition_variable finished_;
  void *context_ = nullptr;
  void (*invoke_)(void *) noexcept = nullptr;
  bool pending_ = false;
  bool stopping_ = false;
  std::thread worker_;
};

}  // namespace mvd

#endif
