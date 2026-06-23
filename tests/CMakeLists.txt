include(GoogleTest)

add_executable(test_timetravel test_timetravel.cpp)
target_link_libraries(test_timetravel PRIVATE omni-timetravel-asan GTest::gtest)
target_compile_options(test_timetravel PRIVATE -fsanitize=address)
target_link_options(test_timetravel PRIVATE -fsanitize=address)
gtest_discover_tests(test_timetravel DISCOVERY_MODE PRE_TEST)