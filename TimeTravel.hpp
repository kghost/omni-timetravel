#pragma once

#include <chrono>
#include <functional>
#include <optional>

namespace Omni::TimeTravel {

class IWarpListener {
public:
  explicit IWarpListener() = default;
  virtual ~IWarpListener() = default;

  IWarpListener(const IWarpListener&) = default;
  auto operator=(const IWarpListener&) -> IWarpListener& = default;
  IWarpListener(IWarpListener&&) = delete;
  auto operator=(IWarpListener&&) -> IWarpListener& = delete;

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
  auto operator=(const Client&) -> Client& = delete;
  Client(Client&&) noexcept;
  auto operator=(Client&&) noexcept -> Client&;

  // Move the monotonic clock forward by the given duration
  void FastForward(std::chrono::nanoseconds duration);

  // Register a listener to prepare and recover from fork-based warp transitions
  void RegisterListener(IWarpListener& listener);

private:
#ifdef _WIN32
  bool _IsInitialized = false;
#else
  int _SocketFd = -1; // Inherited Unix domain socket connection to the parent orchestrator
#endif
  std::optional<std::reference_wrapper<IWarpListener>> _Listener;
};

class Orchestrator {
public:
  explicit Orchestrator();
  ~Orchestrator();

  Orchestrator(Orchestrator&&) = delete;
  auto operator=(Orchestrator&&) -> Orchestrator& = delete;
  Orchestrator(const Orchestrator&) = delete;
  auto operator=(const Orchestrator&) -> Orchestrator& = delete;

  // Spawns the child (by forking and re-executing argv) and starts the orchestrator loop.
  // Returns the exit status code of the last active child process.
  auto Run(char** argv) -> int;

private:
#ifndef _WIN32
  int _ChildPid = -1;
  int _ControlSocketFd = -1;

  // Track cumulative offsets to apply to new namespaces
  long long _CumulativeMonotonicOffsetNs = 0;
  long long _CumulativeBoottimeOffsetNs = 0;
#endif
};

} // namespace Omni::TimeTravel
