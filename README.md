# OmniTimeTravel

`omni-timetravel` is a lightweight, zero-overhead C++23 library designed to virtualize and manipulate system time (specifically `CLOCK_MONOTONIC` and `CLOCK_BOOTTIME`) using Linux Time Namespaces on Linux, and Microsoft Detours on Windows. It is primarily used for testing integration suites, simulating fast-forwarding, and accelerating tests that rely on steady timeouts without altering the wall-clock time (`CLOCK_REALTIME`).

## Prerequisites

- **Linux**: Linux kernel 5.6+ with `CONFIG_TIME_NS=y` enabled.
- **Windows**: Microsoft Detours (integrated automatically via vcpkg).
- **Privileges**: On Linux, can run as root (using direct time namespace unshare) or as an unprivileged user (automatically falling back to user namespaces UID/GID mapping). No special privileges are required on Windows.
- **Threading**: **Only works for single-threaded applications on Linux**. Because `FastForward` on Linux performs a `fork()` under the hood to transition to a new time namespace offset, any auxiliary threads running in the parent process will not survive in the child process, which can cause deadlocks and undefined behavior. On Windows, no `fork()` is performed (user-space hooking is used in-place), but keeping applications single-threaded or synchronized is still recommended for platform consistency.

## Public API Reference

The library exposes two main classes inside the `Omni::TimeTravel` namespace:

### `IWarpListener`

An interface that allows external components (such as fiber event loops or asynchronous runtimes like Boost.Asio) to coordinate state across time warp fork boundaries.

```cpp
class IWarpListener {
public:
  virtual ~IWarpListener() = default;

  // Called in the child process context before the time warp transition starts.
  virtual void OnPreWarp() {}

  // Called in the child process context (Gen N) after the fork but before it exits.
  virtual void OnPostWarpParent() {}

  // Called in the new child process context (Gen N+1) after the fork before returning.
  virtual void OnPostWarpChild() {}
};
```

### `Client`

Constructed inside the child process context to request fast-forwarding operations.

```cpp
class Client {
public:
  Client();
  ~Client();

  // Non-copyable, movable
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;
  Client(Client&&) noexcept;
  Client& operator=(Client&&) noexcept;

  // Move the monotonic clock forward by the given duration.
  // Throws std::runtime_error or std::system_error if the operation fails.
  void FastForward(std::chrono::nanoseconds duration);

  // Register a listener to prepare/recover from fork-based warp transitions.
  void RegisterListener(IWarpListener& listener);
};
```

### `Orchestrator`

Used in the parent process context to bootstrap the initial time namespace and control loop.

```cpp
class Orchestrator {
public:
  Orchestrator();
  ~Orchestrator();

  // Non-copyable, non-movable
  Orchestrator(const Orchestrator&) = delete;
  Orchestrator& operator=(const Orchestrator&) = delete;

  // Spawns the child (forks & re-executes) and runs the time travel server loop.
  // Returns the exit code of the last active child generation.
  int Run(char** argv);
};
```

## Integration with Event Loops (e.g., Boost.Asio)

When integrating `omni-timetravel` with event loops such as `boost::asio::io_context`, fast-forwarding requires specific handling depending on the operating system:

- **Linux**: `FastForward` performs a process `fork()` to transition to a new time namespace. The event loop must be notified of the fork (e.g., calling `_Io.notify_fork(...)`) to recreate internal `epoll` or `io_uring` file descriptors.
- **Windows**: Microsoft Detours hooks `QueryPerformanceCounter` in user space without forking. However, Boost.Asio's `win_iocp_io_context` utilizes a dedicated internal timer thread (`timer_thread_`) that may be sleeping in an OS kernel wait (`::Sleep` / `::WaitForSingleObject`) for an already-scheduled steady timer. To force the timer thread to wake up immediately and re-evaluate timer expiries against the updated QPC offset:
  1. Post an event to the IOCP completion queue (`boost::asio::post`).
  2. Register a dummy `steady_timer` with `expires_at(std::chrono::steady_clock::time_point::min())`. Placing a timer at `time_point::min()` places it at the head of Boost.Asio's internal timer queue, interrupting kernel sleep instantly.

### Recommended `IWarpListener` Implementation

```cpp
class AsioWarpListener : public Omni::TimeTravel::IWarpListener {
public:
  explicit AsioWarpListener(boost::asio::io_context& io) : _Io(io) {}

  void OnPreWarp() override {
#ifndef _WIN32
    _Io.notify_fork(boost::asio::io_context::fork_prepare);
#endif
  }

  void OnPostWarpParent() override {
#ifndef _WIN32
    _Io.notify_fork(boost::asio::io_context::fork_parent);
#endif
  }

  void OnPostWarpChild() override {
#ifndef _WIN32
    _Io.notify_fork(boost::asio::io_context::fork_child);
#else
    // On Windows, wake up the IOCP completion port and interrupt Boost.Asio's timer thread
    boost::asio::post(_Io, [] {});
    auto dummyTimer = std::make_shared<boost::asio::steady_timer>(_Io);
    dummyTimer->expires_at(std::chrono::steady_clock::time_point::min());
    dummyTimer->async_wait([dummyTimer](const boost::system::error_code&) {});
#endif
  }

private:
  boost::asio::io_context& _Io;
};
```

## Bootstrap Setup

To use the library, your executable must split its execution flow in `main()` based on the `OMNI_TIMETRAVEL_IS_CHILD` environment variable:

```cpp
#include <omni-timetravel/TimeTravel.hpp>
#include <iostream>
#include <chrono>

int main(int argc, char* argv[]) {
  if (std::getenv("OMNI_TIMETRAVEL_IS_CHILD")) {
    // Client child role
    Omni::TimeTravel::Client timeClient;
    
    // Perform standard application logic...
    auto t1 = std::chrono::steady_clock::now();
    timeClient.FastForward(std::chrono::seconds(10));
    auto t2 = std::chrono::steady_clock::now(); // Perceives +10 seconds instantly!
    
    return 0;
  }

  // Parent orchestrator role
  Omni::TimeTravel::Orchestrator orchestrator;
  return orchestrator.Run(argv);
}
```

## License

This library is licensed under the MIT License. See the [LICENSE](LICENSE) file for details.

