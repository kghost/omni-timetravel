include(GoogleTest)

add_executable(test_timetravel test_timetravel.cpp)
target_link_libraries(test_timetravel PRIVATE omni-timetravel GTest::gtest)
gtest_discover_tests(test_timetravel DISCOVERY_MODE PRE_TEST)