add_executable(test_timetravel test_timetravel.cpp)
target_link_libraries(test_timetravel PRIVATE omni-timetravel GTest::gtest GTest::gtest_main)
add_test(NAME test_timetravel COMMAND test_timetravel)
