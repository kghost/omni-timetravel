cmake_minimum_required(VERSION 3.10)
project(omni-timetravel)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(WIN32)
  set(SOURCES TimeTravelWindows.cpp)
else()
  set(SOURCES TimeTravelLinux.cpp)
endif()

set(HEADERS
  TimeTravel.hpp
)

if(WIN32)
  find_path(DETOURS_INCLUDE_DIRS "detours/detours.h" REQUIRED)
  find_library(DETOURS_LIBRARY NAMES detours REQUIRED)
endif()

foreach(suffix "" "-asan")
  set(target "omni-timetravel${suffix}")
  add_library(${target} STATIC ${SOURCES} ${HEADERS})

  target_include_directories(${target}
    PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:include>
  )

  if(WIN32)
    target_compile_options(${target} PRIVATE /W3 /Od)
    target_include_directories(${target} PUBLIC ${DETOURS_INCLUDE_DIRS})
    target_link_libraries(${target} PUBLIC ${DETOURS_LIBRARY})
  else()
    target_compile_options(${target} PRIVATE -Wall)
  endif()

  if(suffix STREQUAL "-asan")
    target_compile_options(${target} PRIVATE -fsanitize=address)
    target_link_options(${target} PRIVATE -fsanitize=address)
  endif()
endforeach()

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
