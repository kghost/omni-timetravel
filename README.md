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

Since `FastForward` performs a `fork()` under the hood, any event loop or system descriptors must handle fork notifications.

```cpp
class AsioWarpListener : public Omni::TimeTravel::IWarpListener {
public:
  explicit AsioWarpListener(boost::asio::io_context& io) : _Io(io) {}

  void OnPreWarp() override {
    // Suspend or pause event loops if needed.
  }

  void OnPostWarpChild() override {
    // Re-initialize epoll/io_uring state after fork.
    _Io.notify_fork(boost::asio::fork_child);
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

