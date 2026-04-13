cmake_minimum_required(VERSION 3.16)
project(LwIPC VERSION 1.0.0 LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Global settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Build type default
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Compiler flags
add_compile_options(-Wall -Wextra -Wpedantic)
if(CMAKE_BUILD_TYPE STREQUAL "Release")
  add_compile_options(-O3)
elseif(CMAKE_BUILD_TYPE STREQUAL "Debug")
  add_compile_options(-g -fsanitize=address,undefined)
  add_link_options(-fsanitize=address,undefined)
endif()

# ---------------------------------------------------------------------------
# Header-only interface library  (all modules are header-only)
# ---------------------------------------------------------------------------
add_library(lwipc INTERFACE)
target_include_directories(lwipc INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
# Link POSIX real-time + threading libraries
target_link_libraries(lwipc INTERFACE rt pthread)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
enable_testing()

function(add_lwipc_test name src)
  add_executable(${name} ${src})
  target_link_libraries(${name} PRIVATE lwipc)
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES TIMEOUT 30)
endfunction()

add_lwipc_test(test_ipc_message   tests/test_ipc_message.cpp)
add_lwipc_test(test_shm_ring_buffer tests/test_shm_ring_buffer.cpp)
add_lwipc_test(test_pub_sub       tests/test_pub_sub.cpp)
add_lwipc_test(test_socket_transport tests/test_socket_transport.cpp)

# ---------------------------------------------------------------------------
# Examples (not run as tests, just built to verify compilation)
# ---------------------------------------------------------------------------
function(add_lwipc_example name src)
  add_executable(${name} ${src})
  target_link_libraries(${name} PRIVATE lwipc)
endfunction()

add_lwipc_example(sensor_publisher  examples/sensor_publisher.cpp)
add_lwipc_example(ai_subscriber     examples/ai_subscriber.cpp)
add_lwipc_example(socket_bridge     examples/socket_bridge.cpp)
add_lwipc_example(benchmark         examples/benchmark.cpp)

# ---------------------------------------------------------------------------
# Install rules (optional)
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(TARGETS lwipc EXPORT LwIPCTargets)
install(EXPORT LwIPCTargets
  FILE LwIPCTargets.cmake
  NAMESPACE LwIPC::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/LwIPC
)
