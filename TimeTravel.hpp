#pragma once

#include <chrono>
#include <functional>
#include <optional>

namespace Omni::TimeTravel {

class IWarpListener {
public:
  virtual ~IWarpListener() = default;

  // Invoked in Child Gen N before transitioning.
  virtual void OnPreWarp() {}

  // Invoked in Child Gen N after fork, before exit.
  virtual void OnPostWarpParent() {}

  // Invoked in Child Gen N+1 after fork, before returning from FastForward.
  virtual void OnPostWarpChild() {}
};

class Client {
public:
  // Constructed inside the child process context
  Client();

  ~Client();

  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;
  Client(Client&&) noexcept;
  Client& operator=(Client&&) noexcept;

  // Move the monotonic clock forward by the given duration
  void FastForward(std::chrono::nanoseconds duration);

  // Register a listener to prepare and recover from fork-based warp transitions
  void RegisterListener(IWarpListener& listener);

private:
  int _SocketFd = -1; // Inherited Unix domain socket connection to the parent orchestrator
  std::optional<std::reference_wrapper<IWarpListener>> _Listener;
};

class Orchestrator {
public:
  Orchestrator();

  ~Orchestrator();

  Orchestrator(const Orchestrator&) = delete;
  Orchestrator& operator=(const Orchestrator&) = delete;

  // Spawns the child (by forking and re-executing argv) and starts the orchestrator loop.
  // Returns the exit status code of the last active child process.
  int Run(char** argv);

private:
  int _ChildPid = -1;
  int _ControlSocketFd = -1;

  // Track cumulative offsets to apply to new namespaces
  long long _CumulativeMonotonicOffsetNs = 0;
  long long _CumulativeBoottimeOffsetNs = 0;
};

} // namespace Omni::TimeTravel

