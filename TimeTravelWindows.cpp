#include "TimeTravel.hpp"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <string>
#include <system_error>
#include <vector>

#include <detours/detours.h>

namespace {

BOOL(WINAPI* g_TrueQueryPerformanceCounter)(LARGE_INTEGER*) = QueryPerformanceCounter;
DWORD(WINAPI* g_TrueGetTickCount)() = GetTickCount;
ULONGLONG(WINAPI* g_TrueGetTickCount64)() = GetTickCount64;

std::atomic<LONGLONG> g_QpcOffset{0};
std::atomic<LONGLONG> g_MsecOffset{0};

BOOL WINAPI HookedQueryPerformanceCounter(LARGE_INTEGER* lpPerformanceCount) {
  BOOL result = g_TrueQueryPerformanceCounter(lpPerformanceCount);
  if (result != 0) {
    lpPerformanceCount->QuadPart += g_QpcOffset.load(std::memory_order_relaxed);
  }
  return result;
}

DWORD WINAPI HookedGetTickCount() {
  return g_TrueGetTickCount() + static_cast<DWORD>(g_MsecOffset.load(std::memory_order_relaxed));
}

ULONGLONG WINAPI HookedGetTickCount64() {
  return g_TrueGetTickCount64() + g_MsecOffset.load(std::memory_order_relaxed);
}

} // namespace

namespace Omni::TimeTravel {

Client::Client() {
  const char* fdStr = std::getenv("OMNI_TIMETRAVEL_SOCKET_FD");
  if (fdStr == nullptr) {
    return;
  }

  DetourTransactionBegin();
  DetourUpdateThread(GetCurrentThread());
  DetourAttach(reinterpret_cast<void**>(&g_TrueQueryPerformanceCounter), HookedQueryPerformanceCounter);
  DetourAttach(reinterpret_cast<void**>(&g_TrueGetTickCount), HookedGetTickCount);
  DetourAttach(reinterpret_cast<void**>(&g_TrueGetTickCount64), HookedGetTickCount64);
  LONG err = DetourTransactionCommit();
  if (err == NO_ERROR) {
    _IsInitialized = true;
  }
}

Client::~Client() {
  if (_IsInitialized) {
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourDetach(reinterpret_cast<void**>(&g_TrueQueryPerformanceCounter), HookedQueryPerformanceCounter);
    DetourDetach(reinterpret_cast<void**>(&g_TrueGetTickCount), HookedGetTickCount);
    DetourDetach(reinterpret_cast<void**>(&g_TrueGetTickCount64), HookedGetTickCount64);
    DetourTransactionCommit();
    _IsInitialized = false;

    g_QpcOffset = 0;
    g_MsecOffset = 0;
  }
}

Client::Client(Client&& other) noexcept : _IsInitialized(other._IsInitialized), _Listener(std::move(other._Listener)) {
  other._IsInitialized = false;
}

Client& Client::operator=(Client&& other) noexcept {
  if (this != &other) {
    if (_IsInitialized) {
      DetourTransactionBegin();
      DetourUpdateThread(GetCurrentThread());
      DetourDetach(reinterpret_cast<void**>(&g_TrueQueryPerformanceCounter), HookedQueryPerformanceCounter);
      DetourDetach(reinterpret_cast<void**>(&g_TrueGetTickCount), HookedGetTickCount);
      DetourDetach(reinterpret_cast<void**>(&g_TrueGetTickCount64), HookedGetTickCount64);
      DetourTransactionCommit();
    }
    _IsInitialized = other._IsInitialized;
    _Listener = std::move(other._Listener);
    other._IsInitialized = false;
  }
  return *this;
}

void Client::RegisterListener(IWarpListener& listener) { _Listener = listener; }

void Client::FastForward(std::chrono::nanoseconds duration) {
  if (!_IsInitialized) {
    throw std::runtime_error(
        "Client is not running under the TimeTravel Orchestrator (OMNI_TIMETRAVEL_SOCKET_FD not set or invalid)");
  }

  if (_Listener) {
    _Listener->get().OnPreWarp();
  }

  LARGE_INTEGER freq;
  if (!QueryPerformanceFrequency(&freq)) {
    throw std::runtime_error("QueryPerformanceFrequency failed");
  }

  double qpcFrequency = static_cast<double>(freq.QuadPart);
  double durationSec = std::chrono::duration<double>(duration).count();
  LONGLONG qpcTicks = static_cast<LONGLONG>(durationSec * qpcFrequency);
  LONGLONG msec = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();

  g_QpcOffset += qpcTicks;
  g_MsecOffset += msec;

  if (_Listener) {
    _Listener->get().OnPostWarpChild();
  }
}

Orchestrator::Orchestrator() {}

Orchestrator::~Orchestrator() {}

int Orchestrator::Run(char** argv) {
  std::string cmdLine;
  for (int i = 0; argv[i] != nullptr; ++i) {
    std::string arg = argv[i];
    if (arg.find(' ') != std::string::npos || arg.find('"') != std::string::npos) {
      std::string escaped = "\"";
      for (char c : arg) {
        if (c == '"') {
          escaped += "\\\"";
        } else {
          escaped += c;
        }
      }
      escaped += "\"";
      cmdLine += escaped;
    } else {
      cmdLine += arg;
    }
    if (argv[i + 1] != nullptr) {
      cmdLine += " ";
    }
  }

  if (!SetEnvironmentVariableA("OMNI_TIMETRAVEL_IS_CHILD", "1")) {
    throw std::system_error(GetLastError(), std::system_category(), "SetEnvironmentVariableA failed");
  }
  if (!SetEnvironmentVariableA("OMNI_TIMETRAVEL_SOCKET_FD", "1")) {
    throw std::system_error(GetLastError(), std::system_category(), "SetEnvironmentVariableA failed");
  }

  STARTUPINFOA si;
  PROCESS_INFORMATION pi;
  ZeroMemory(&si, sizeof(si));
  si.cb = sizeof(si);
  ZeroMemory(&pi, sizeof(pi));

  std::vector<char> cmdLineBuf(cmdLine.begin(), cmdLine.end());
  cmdLineBuf.push_back('\0');

  if (!CreateProcessA(nullptr, cmdLineBuf.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
    SetEnvironmentVariableA("OMNI_TIMETRAVEL_IS_CHILD", nullptr);
    SetEnvironmentVariableA("OMNI_TIMETRAVEL_SOCKET_FD", nullptr);
    throw std::system_error(GetLastError(), std::system_category(), "CreateProcessA failed");
  }

  WaitForSingleObject(pi.hProcess, INFINITE);

  DWORD exitCode = 1;
  GetExitCodeProcess(pi.hProcess, &exitCode);

  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);

  SetEnvironmentVariableA("OMNI_TIMETRAVEL_IS_CHILD", nullptr);
  SetEnvironmentVariableA("OMNI_TIMETRAVEL_SOCKET_FD", nullptr);

  return static_cast<int>(exitCode);
}

} // namespace Omni::TimeTravel
