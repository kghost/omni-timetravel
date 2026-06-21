#include <gtest/gtest.h>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <unistd.h>
#include "../TimeTravel.hpp"

namespace {

bool g_PreWarpCalled = false;
bool g_PostWarpParentCalled = false;
bool g_PostWarpChildCalled = false;

class TestWarpListener : public Omni::TimeTravel::IWarpListener {
public:
  void OnPreWarp() override {
    g_PreWarpCalled = true;
  }

  void OnPostWarpParent() override {
    g_PostWarpParentCalled = true;
  }

  void OnPostWarpChild() override {
    g_PostWarpChildCalled = true;
  }
};

} // namespace

TEST(TimeTravelTest, EndToEndWarp) {
  Omni::TimeTravel::Client timeClient;
  TestWarpListener listener;
  timeClient.RegisterListener(listener);

  // Read time before warp
  auto t1 = std::chrono::steady_clock::now();

  // Perform time travel warp of 30 seconds
  std::cout << "[Test] Requesting time warp of 30 seconds..." << std::endl;
  timeClient.FastForward(std::chrono::seconds(30));

  // Read time after warp
  auto t2 = std::chrono::steady_clock::now();
  auto elapsed_s = std::chrono::duration_cast<std::chrono::seconds>(t2 - t1).count();
  std::cout << "[Test] Elapsed monotonic time perceived: " << elapsed_s << "s" << std::endl;

  // Assertions (should perceive at least 30 seconds having passed)
  EXPECT_GE(elapsed_s, 30);

  // Check hook invocations in the current child generation (Child Gen 2)
  EXPECT_TRUE(g_PreWarpCalled);
  EXPECT_TRUE(g_PostWarpChildCalled);
  EXPECT_FALSE(g_PostWarpParentCalled);
}

TEST(TimeTravelTest, MultiWarpTest) {
  Omni::TimeTravel::Client timeClient;

  // Warp once
  auto t1 = std::chrono::steady_clock::now();
  std::cout << "[Test] Requesting first warp of 15 seconds..." << std::endl;
  timeClient.FastForward(std::chrono::seconds(15));

  auto t2 = std::chrono::steady_clock::now();
  auto elapsed1 = std::chrono::duration_cast<std::chrono::seconds>(t2 - t1).count();
  EXPECT_GE(elapsed1, 15);

  // Warp twice
  std::cout << "[Test] Requesting second warp of 20 seconds..." << std::endl;
  timeClient.FastForward(std::chrono::seconds(20));

  auto t3 = std::chrono::steady_clock::now();
  auto elapsed2 = std::chrono::duration_cast<std::chrono::seconds>(t3 - t2).count();
  EXPECT_GE(elapsed2, 20);

  auto totalElapsed = std::chrono::duration_cast<std::chrono::seconds>(t3 - t1).count();
  EXPECT_GE(totalElapsed, 35);
  std::cout << "[Test] Multi-warp succeeded. Total perceived elapsed: " << totalElapsed << "s" << std::endl;
}

TEST(TimeTravelTest, ErrorHandlingTest) {
  // Save socket env, delete it, construct a client, then restore env.
  const char* oldEnvVal = std::getenv("OMNI_TIMETRAVEL_SOCKET_FD");
  std::string savedEnv;
  if (oldEnvVal != nullptr) {
    savedEnv = oldEnvVal;
    unsetenv("OMNI_TIMETRAVEL_SOCKET_FD");
  }

  // Expect throw because not running under orchestrator
  EXPECT_THROW({
    Omni::TimeTravel::Client clientWithoutOrchestrator;
    clientWithoutOrchestrator.FastForward(std::chrono::seconds(10));
  }, std::runtime_error);

  // Restore env if it was set
  if (!savedEnv.empty()) {
    setenv("OMNI_TIMETRAVEL_SOCKET_FD", savedEnv.c_str(), 1);
  }
}

int main(int argc, char* argv[]) {
  if (std::getenv("OMNI_TIMETRAVEL_IS_CHILD")) {
    std::cout << "[Child] Running Google Test suite..." << std::endl;
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
  }

  std::cout << "[Parent] Starting orchestrator..." << std::endl;
  Omni::TimeTravel::Orchestrator orchestrator;

  int status = 1;
  try {
    status = orchestrator.Run(argv);
  } catch (const std::exception& e) {
    std::cerr << "[Parent] Orchestrator exception caught: " << e.what() << std::endl;
  }

  std::cout << "[Parent] Orchestrator completed. Child status: " << status << std::endl;
  return status;
}
