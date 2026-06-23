#include "TimeTravel.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <sched.h>
#include <string>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <system_error>
#include <unistd.h>

namespace {

// RAII helper for managing file descriptors.
class UniqueFd {
public:
  UniqueFd() noexcept = default;
  explicit UniqueFd(int fd) noexcept : _Fd(fd) {}
  ~UniqueFd() { Reset(); }

  UniqueFd(const UniqueFd&) = delete;
  UniqueFd& operator=(const UniqueFd&) = delete;

  UniqueFd(UniqueFd&& other) noexcept : _Fd(other._Fd) { other._Fd = -1; }

  UniqueFd& operator=(UniqueFd&& other) noexcept {
    if (this != &other) {
      Reset();
      _Fd = other._Fd;
      other._Fd = -1;
    }
    return *this;
  }

  int Get() const noexcept { return _Fd; }
  bool IsValid() const noexcept { return _Fd >= 0; }

  int Release() noexcept {
    int ret = _Fd;
    _Fd = -1;
    return ret;
  }

  void Reset(int newFd = -1) noexcept {
    if (_Fd >= 0) {
      close(_Fd);
    }
    _Fd = newFd;
  }

private:
  int _Fd = -1;
};

// Protocol definitions
enum class MsgType : uint8_t { kWarpRequest = 1, kWarpResponse = 2, kTransitionResult = 3 };

struct WarpRequestMsg {
  MsgType Type = MsgType::kWarpRequest;
  long long DurationNs = 0;
};

struct WarpResponseMsg {
  MsgType Type = MsgType::kWarpResponse;
  bool Success = false;
};

struct TransitionResultMsg {
  MsgType Type = MsgType::kTransitionResult;
  bool Success = false;
  pid_t ChildPid = -1;
  int ErrorCode = 0;
};

// Sends exactly 'length' bytes over the socket.
bool SendAll(int socket, const void* buffer, size_t length) {
  size_t totalSent = 0;
  const char* ptr = static_cast<const char*>(buffer);
  while (totalSent < length) {
    ssize_t sent = send(socket, ptr + totalSent, length - totalSent, 0);
    if (sent < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    totalSent += sent;
  }
  return true;
}

// Receives exactly 'length' bytes from the socket.
bool RecvAll(int socket, void* buffer, size_t length) {
  size_t totalRecv = 0;
  char* ptr = static_cast<char*>(buffer);
  while (totalRecv < length) {
    ssize_t received = recv(socket, ptr + totalRecv, length - totalRecv, 0);
    if (received < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    if (received == 0) {
      return false; // EOF
    }
    totalRecv += received;
  }
  return true;
}

// Sends a file descriptor over a Unix domain socket.
bool SendFd(int socket, int fdToSend) {
  struct msghdr msg;
  std::memset(&msg, 0, sizeof(msg));
  char buf[1] = {0};
  struct iovec io = {.iov_base = buf, .iov_len = sizeof(buf)};
  msg.msg_iov = &io;
  msg.msg_iovlen = 1;

  union {
    struct cmsghdr align;
    char control[CMSG_SPACE(sizeof(int))];
  } controlUn;
  msg.msg_control = controlUn.control;
  msg.msg_controllen = sizeof(controlUn.control);

  struct cmsghdr* cmptr = CMSG_FIRSTHDR(&msg);
  cmptr->cmsg_len = CMSG_LEN(sizeof(int));
  cmptr->cmsg_level = SOL_SOCKET;
  cmptr->cmsg_type = SCM_RIGHTS;
  *reinterpret_cast<int*>(CMSG_DATA(cmptr)) = fdToSend;

  if (sendmsg(socket, &msg, 0) < 0) {
    return false;
  }
  return true;
}

// Receives a file descriptor over a Unix domain socket.
int RecvFd(int socket) {
  struct msghdr msg;
  std::memset(&msg, 0, sizeof(msg));
  char buf[1];
  struct iovec io = {.iov_base = buf, .iov_len = sizeof(buf)};
  msg.msg_iov = &io;
  msg.msg_iovlen = 1;

  union {
    struct cmsghdr align;
    char control[CMSG_SPACE(sizeof(int))];
  } controlUn;
  msg.msg_control = controlUn.control;
  msg.msg_controllen = sizeof(controlUn.control);

  if (recvmsg(socket, &msg, 0) < 0) {
    return -1;
  }

  struct cmsghdr* cmptr = CMSG_FIRSTHDR(&msg);
  if (cmptr != nullptr && cmptr->cmsg_len == CMSG_LEN(sizeof(int))) {
    if (cmptr->cmsg_level != SOL_SOCKET || cmptr->cmsg_type != SCM_RIGHTS) {
      return -1;
    }
    return *reinterpret_cast<int*>(CMSG_DATA(cmptr));
  }
  return -1;
}

// Sets send/recv timeout on a socket.
bool SetSocketTimeout(int socket, int seconds) {
  struct timeval tv;
  tv.tv_sec = seconds;
  tv.tv_usec = 0;
  if (setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
    return false;
  }
  if (setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) < 0) {
    return false;
  }
  return true;
}

// Exception-safe helper to write content to a file.
void WriteFile(const std::string& path, const std::string& content) {
  std::ofstream file(path);
  if (!file) {
    throw std::system_error(errno, std::generic_category(), "Failed to open " + path);
  }
  file << content;
  if (!file) {
    throw std::system_error(errno, std::generic_category(), "Failed to write to " + path);
  }
  file.close();
}

} // namespace

namespace Omni::TimeTravel {

Client::Client() {
  const char* fdStr = std::getenv("OMNI_TIMETRAVEL_SOCKET_FD");
  if (fdStr != nullptr) {
    try {
      _SocketFd = std::stoi(fdStr);
    } catch (...) {
      _SocketFd = -1;
    }
  }
}

Client::~Client() {}

Client::Client(Client&& other) noexcept : _SocketFd(other._SocketFd), _Listener(std::move(other._Listener)) {
  other._SocketFd = -1;
}

Client& Client::operator=(Client&& other) noexcept {
  if (this != &other) {
    _SocketFd = other._SocketFd;
    _Listener = std::move(other._Listener);
    other._SocketFd = -1;
  }
  return *this;
}

void Client::RegisterListener(IWarpListener& listener) { _Listener = listener; }

void Client::FastForward(std::chrono::nanoseconds duration) {
  if (_SocketFd < 0) {
    throw std::runtime_error(
        "Client is not running under the TimeTravel Orchestrator (OMNI_TIMETRAVEL_SOCKET_FD not set or invalid)");
  }

  if (_Listener) {
    _Listener->get().OnPreWarp();
  }

  WarpRequestMsg req;
  req.DurationNs = duration.count();

  if (!SendAll(_SocketFd, &req, sizeof(req))) {
    throw std::system_error(errno, std::generic_category(), "Failed to send warp request to Orchestrator");
  }

  // Receive warp response
  WarpResponseMsg resp;
  if (!RecvAll(_SocketFd, &resp, sizeof(resp))) {
    throw std::system_error(errno, std::generic_category(), "Failed to receive warp response from Orchestrator");
  }

  if (!resp.Success) {
    throw std::runtime_error("Orchestrator failed to perform time warp unshare");
  }

  // Receive the new namespace FD
  int rawNsFd = RecvFd(_SocketFd);
  if (rawNsFd < 0) {
    throw std::runtime_error("Failed to receive time namespace file descriptor from Orchestrator");
  }
  UniqueFd nsFd(rawNsFd);

  // Join the new time namespace
  if (setns(nsFd.Get(), CLONE_NEWTIME) != 0) {
    int savedErrno = errno;
    TransitionResultMsg failResult;
    failResult.Success = false;
    failResult.ErrorCode = savedErrno;
    SendAll(_SocketFd, &failResult, sizeof(failResult));
    throw std::system_error(savedErrno, std::generic_category(), "setns(CLONE_NEWTIME) failed");
  }
  nsFd.Reset(); // Close immediately as we have joined

  // Create synchronization pipe
  int rawPipe[2];
  if (pipe(rawPipe) < 0) {
    int savedErrno = errno;
    TransitionResultMsg failResult;
    failResult.Success = false;
    failResult.ErrorCode = savedErrno;
    SendAll(_SocketFd, &failResult, sizeof(failResult));
    throw std::system_error(savedErrno, std::generic_category(), "pipe creation failed");
  }
  UniqueFd pipeRead(rawPipe[0]);
  UniqueFd pipeWrite(rawPipe[1]);

  pid_t pid = fork();
  if (pid < 0) {
    int savedErrno = errno;
    TransitionResultMsg failResult;
    failResult.Success = false;
    failResult.ErrorCode = savedErrno;
    SendAll(_SocketFd, &failResult, sizeof(failResult));
    throw std::system_error(savedErrno, std::generic_category(), "fork failed");
  }

  if (pid > 0) {
    // Child Gen N (parent of fork)
    pipeRead.Reset(); // Close read end

    TransitionResultMsg successResult;
    successResult.Success = true;
    successResult.ChildPid = pid;
    successResult.ErrorCode = 0;

    SendAll(_SocketFd, &successResult, sizeof(successResult));

    if (_Listener) {
      _Listener->get().OnPostWarpParent();
    }

    // Closing pipeWrite will trigger EOF in the new child
    pipeWrite.Reset();

    // Close the socket to parent
    if (_SocketFd >= 0) {
      close(_SocketFd);
      _SocketFd = -1;
    }

    // Exit immediately without calling destructors/cleanup twice
    std::_Exit(0);
  } else {
    // Child Gen N+1 (child of fork)
    pipeWrite.Reset(); // Close write end

    // Block on read end until Child Gen N exits (EOF)
    char buf;
    ssize_t bytes = read(pipeRead.Get(), &buf, sizeof(buf));
    (void)bytes;
    pipeRead.Reset();

    if (_Listener) {
      _Listener->get().OnPostWarpChild();
    }
  }
}

Orchestrator::Orchestrator() {}

Orchestrator::~Orchestrator() {
  if (_ControlSocketFd >= 0) {
    close(_ControlSocketFd);
    _ControlSocketFd = -1;
  }
}

int Orchestrator::Run(char** argv) {
  uid_t hostUid = getuid();
  gid_t hostGid = getgid();

  // Create socket pair
  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
    throw std::system_error(errno, std::generic_category(), "Failed to create socketpair");
  }
  UniqueFd parentSocket(sv[0]);
  UniqueFd childSocket(sv[1]);

  // Set timeout of 5 seconds on the socket to prevent indefinite hangs
  SetSocketTimeout(parentSocket.Get(), 5);
  SetSocketTimeout(childSocket.Get(), 5);

  // Register as subreaper
  if (prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) < 0) {
    throw std::system_error(errno, std::generic_category(), "prctl(PR_SET_CHILD_SUBREAPER) failed");
  }

  // Dual Fallback namespace unshare strategy:
  // First attempt time namespace unshare directly (requires root/CAP_SYS_ADMIN).
  // If that fails with EPERM, fallback to CLONE_NEWUSER | CLONE_NEWTIME.
  bool hasUserNs = false;
  if (unshare(CLONE_NEWTIME) != 0) {
    if (errno != EPERM) {
      throw std::system_error(errno, std::generic_category(), "unshare(CLONE_NEWTIME) failed with non-EPERM error");
    }
    // Attempt with CLONE_NEWUSER fallback
    if (unshare(CLONE_NEWUSER | CLONE_NEWTIME) != 0) {
      throw std::system_error(errno, std::generic_category(), "unshare(CLONE_NEWUSER | CLONE_NEWTIME) failed");
    }
    hasUserNs = true;
  }

  if (hasUserNs) {
    // Write mapping for our new user namespace
    try {
      WriteFile("/proc/self/setgroups", "deny");
      WriteFile("/proc/self/gid_map", "0 " + std::to_string(hostGid) + " 1\n");
      WriteFile("/proc/self/uid_map", "0 " + std::to_string(hostUid) + " 1\n");
    } catch (const std::exception& e) {
      // Print warning but try to proceed as mapping failure might not block child execution
      std::cerr << "[Orchestrator] Warning: Failed to write user namespace mappings: " << e.what() << std::endl;
    }
  }

  // Set initial zero offsets for the first child time namespace
  try {
    WriteFile("/proc/self/timens_offsets", "monotonic 0 0\nboottime 0 0\n");
  } catch (const std::exception& e) {
    throw std::runtime_error(std::string("Failed to write initial timens_offsets: ") + e.what());
  }

  // Fork child gen 1
  _ChildPid = fork();
  if (_ChildPid < 0) {
    throw std::system_error(errno, std::generic_category(), "Failed to fork Child Gen 1");
  }

  if (_ChildPid == 0) {
    // Child Gen 1 context
    parentSocket.Reset(); // Close parent socket end

    // Clear FD_CLOEXEC on childSocket so it survives execv
    int flags = fcntl(childSocket.Get(), F_GETFD);
    if (flags >= 0) {
      fcntl(childSocket.Get(), F_SETFD, flags & ~FD_CLOEXEC);
    }

    setenv("OMNI_TIMETRAVEL_IS_CHILD", "1", 1);
    std::string socketFdStr = std::to_string(childSocket.Release());
    setenv("OMNI_TIMETRAVEL_SOCKET_FD", socketFdStr.c_str(), 1);

    execv("/proc/self/exe", argv);
    std::cerr << "[Child Gen 1] execv failed: " << std::strerror(errno) << std::endl;
    std::_Exit(1);
  }

  // Orchestrator parent context
  childSocket.Reset(); // Close child socket end
  _ControlSocketFd = parentSocket.Release();

  // Loop to handle warp requests and child updates
  while (true) {
    WarpRequestMsg req;
    if (!RecvAll(_ControlSocketFd, &req, sizeof(req))) {
      // Child closed socket or recv timed out/error
      break;
    }

    // Accumulate offsets
    _CumulativeMonotonicOffsetNs += req.DurationNs;
    _CumulativeBoottimeOffsetNs += req.DurationNs;

    // Create a new time namespace for future forks
    if (unshare(CLONE_NEWTIME) != 0) {
      std::cerr << "[Orchestrator] unshare(CLONE_NEWTIME) failed during warp: " << std::strerror(errno) << std::endl;
      WarpResponseMsg failResp;
      failResp.Success = false;
      SendAll(_ControlSocketFd, &failResp, sizeof(failResp));
      break;
    }

    // Write new cumulative offsets
    try {
      long long secM = _CumulativeMonotonicOffsetNs / 1000000000LL;
      long long nsecM = _CumulativeMonotonicOffsetNs % 1000000000LL;
      long long secB = _CumulativeBoottimeOffsetNs / 1000000000LL;
      long long nsecB = _CumulativeBoottimeOffsetNs % 1000000000LL;

      std::string offsetStr = "monotonic " + std::to_string(secM) + " " + std::to_string(nsecM) + "\n" + "boottime " +
                              std::to_string(secB) + " " + std::to_string(nsecB) + "\n";
      WriteFile("/proc/self/timens_offsets", offsetStr);
    } catch (const std::exception& e) {
      std::cerr << "[Orchestrator] Failed to write timens_offsets during warp: " << e.what() << std::endl;
      WarpResponseMsg failResp;
      failResp.Success = false;
      SendAll(_ControlSocketFd, &failResp, sizeof(failResp));
      break;
    }

    // Open the new namespace FD
    int rawNsFd = open("/proc/self/ns/time_for_children", O_RDONLY);
    if (rawNsFd < 0) {
      std::cerr << "[Orchestrator] Failed to open time_for_children: " << std::strerror(errno) << std::endl;
      WarpResponseMsg failResp;
      failResp.Success = false;
      SendAll(_ControlSocketFd, &failResp, sizeof(failResp));
      break;
    }
    UniqueFd nsFd(rawNsFd);

    // Send success response
    WarpResponseMsg successResp;
    successResp.Success = true;
    if (!SendAll(_ControlSocketFd, &successResp, sizeof(successResp))) {
      std::cerr << "[Orchestrator] Failed to send warp response" << std::endl;
      break;
    }

    // Pass namespace FD to the child
    if (!SendFd(_ControlSocketFd, nsFd.Get())) {
      std::cerr << "[Orchestrator] SendFd failed" << std::endl;
      break;
    }
    nsFd.Reset();

    // Read the transition result from the child
    TransitionResultMsg result;
    if (!RecvAll(_ControlSocketFd, &result, sizeof(result))) {
      std::cerr << "[Orchestrator] Failed to receive child transition result" << std::endl;
      break;
    }

    if (!result.Success) {
      std::cerr << "[Orchestrator] Child reported warp transition failure, error code: " << result.ErrorCode
                << std::endl;
      // The child failed to transition and is still running in its old generation.
      // We keep loop running and do not reap or change _ChildPid.
      continue;
    }

    pid_t oldChildPid = _ChildPid;
    _ChildPid = result.ChildPid;

    // Reap the old child process
    int status;
    waitpid(oldChildPid, &status, 0);
  }

  // Socket closed or error occurred, wait for the last active child to finish
  int finalStatus = 0;
  if (_ChildPid > 0) {
    waitpid(_ChildPid, &finalStatus, 0);
  }

  if (WIFEXITED(finalStatus)) {
    return WEXITSTATUS(finalStatus);
  } else if (WIFSIGNALED(finalStatus)) {
    return 128 + WTERMSIG(finalStatus);
  }
  return 1;
}

} // namespace Omni::TimeTravel
