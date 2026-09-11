/** GPL-3.0. Serialized retry timer. Caller holds the shared mutex in setup_timer. */
#pragma once
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>
namespace display_device::detail {
  class state_retry_timer {
  public:
    using callback = std::function<bool()>;
    using exception_callback = std::function<void(std::exception_ptr)>;
    explicit state_retry_timer(std::mutex &mutex, std::chrono::milliseconds interval = std::chrono::seconds(5),
                               exception_callback on_exception = {}):
      mutex_(mutex), interval_(interval), on_exception_(std::move(on_exception)) {
      if (interval_.count() <= 0) throw std::invalid_argument("retry interval must be positive");
      worker_ = std::thread([this] { run(); });
    }
    ~state_retry_timer() {
      {
        std::lock_guard lock(mutex_);
        running_ = false; ++revision_; deadline_.reset(); retry_ = {}; changed_.notify_all();
      }
      if (worker_.joinable()) worker_.join();
    }
    state_retry_timer(const state_retry_timer &) = delete;
    state_retry_timer &operator=(const state_retry_timer &) = delete;
    void setup_timer(callback retry) {
      ++revision_; retry_ = std::move(retry);
      if (retry_) deadline_ = std::chrono::steady_clock::now() + interval_;
      else deadline_.reset();
      changed_.notify_all();
    }
  private:
    void run() {
      std::unique_lock lock(mutex_);
      while (running_) {
        if (!deadline_) {
          changed_.wait(lock, [this] { return !running_ || deadline_.has_value(); }); continue;
        }
        const auto revision = revision_; const auto deadline = *deadline_;
        if (changed_.wait_until(lock, deadline, [this, revision] { return !running_ || revision_ != revision; })) continue;
        deadline_.reset(); bool finished = true;
        try { auto retry = retry_; finished = !retry || retry(); }
        catch (...) { if (on_exception_) { try { on_exception_(std::current_exception()); } catch (...) {} } }
        if (revision_ == revision) {
          if (!finished && running_) deadline_ = std::chrono::steady_clock::now() + interval_;
          else retry_ = {};
        }
      }
    }
    std::mutex &mutex_;
    std::chrono::milliseconds interval_;
    exception_callback on_exception_;
    std::condition_variable changed_;
    callback retry_;
    std::optional<std::chrono::steady_clock::time_point> deadline_;
    std::uint64_t revision_ {};
    bool running_ {true};
    std::thread worker_;
  };
}
