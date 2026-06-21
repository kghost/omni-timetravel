cmake_minimum_required(VERSION 3.10)
project(omni-timetravel)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(SOURCES
  TimeTravel.cpp
)

set(HEADERS
  TimeTravel.hpp
)

add_library(omni-timetravel STATIC ${SOURCES} ${HEADERS})

target_include_directories(omni-timetravel
  PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
  $<INSTALL_INTERFACE:include>
)

target_compile_options(omni-timetravel PRIVATE -Wall -Wextra)

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
