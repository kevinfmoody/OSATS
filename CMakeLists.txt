cmake_minimum_required(VERSION 3.16)

# Prefer g++ when the default c++ driver is clang without a findable libstdc++.
if(NOT DEFINED CMAKE_CXX_COMPILER AND EXISTS "/usr/bin/g++")
  set(CMAKE_CXX_COMPILER "/usr/bin/g++")
endif()

project(osats VERSION 2.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)

add_library(osats
  src/net/socket.cpp
  src/net/framed_connection.cpp
  src/http_api.cpp
  src/server.cpp
  src/client.cpp
)
target_include_directories(osats PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(osats PUBLIC Threads::Threads)
target_compile_options(osats PRIVATE -Wall -Wextra -Wpedantic)

add_executable(osats-server src/server_main.cpp)
target_link_libraries(osats-server PRIVATE osats)

add_executable(osats-client src/client_main.cpp)
target_link_libraries(osats-client PRIVATE osats)

add_executable(osats-cli src/cli_main.cpp)
target_link_libraries(osats-cli PRIVATE osats)

add_executable(osats-loadtest src/loadtest_main.cpp)
target_link_libraries(osats-loadtest PRIVATE osats)

enable_testing()
foreach(t test_geo test_spatial_index test_protocol test_world)
  add_executable(${t} tests/${t}.cpp)
  target_link_libraries(${t} PRIVATE osats)
  add_test(NAME ${t} COMMAND ${t})
endforeach()
