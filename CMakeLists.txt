cmake_minimum_required(VERSION 3.22)

project(
  IndustrialAIServiceFramework
  VERSION 0.1.0
  DESCRIPTION "C++17 foundation for an industrial AI task service framework"
  LANGUAGES CXX
)

option(IAISF_BUILD_TESTS "Build IndustrialAIServiceFramework tests" ON)
option(IAISF_USE_SYSTEM_DEPS "Use system-provided dependencies" OFF)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(IAISF_BUILD_LINUX_NETWORK_DEFAULT ON)
else()
  set(IAISF_BUILD_LINUX_NETWORK_DEFAULT OFF)
endif()
option(
  IAISF_BUILD_LINUX_NETWORK
  "Build the Linux-only epoll/eventfd Reactor core"
  "${IAISF_BUILD_LINUX_NETWORK_DEFAULT}"
)
unset(IAISF_BUILD_LINUX_NETWORK_DEFAULT)

if(IAISF_BUILD_LINUX_NETWORK AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(
    FATAL_ERROR
    "IAISF_BUILD_LINUX_NETWORK=ON requires Linux with epoll and eventfd"
  )
endif()

set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(CompilerOptions)
include(Dependencies)

set(IAISF_GENERATED_INCLUDE_DIR "${PROJECT_BINARY_DIR}/generated/include")
file(MAKE_DIRECTORY "${IAISF_GENERATED_INCLUDE_DIR}/iaisf")
configure_file(
  "${PROJECT_SOURCE_DIR}/include/iaisf/version.hpp.in"
  "${IAISF_GENERATED_INCLUDE_DIR}/iaisf/version.hpp"
  @ONLY
)

add_library(
  iaisf_core
  STATIC
  src/app/application.cpp
  src/config/app_config.cpp
  src/core/error.cpp
  src/logging/console_logger.cpp
  src/logging/log_level.cpp
)
add_library(iaisf::core ALIAS iaisf_core)

target_compile_features(iaisf_core PUBLIC cxx_std_17)
target_include_directories(
  iaisf_core
  PUBLIC
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${IAISF_GENERATED_INCLUDE_DIR}>"
)
target_link_libraries(iaisf_core PRIVATE nlohmann_json::nlohmann_json)
iaisf_enable_project_warnings(iaisf_core)

if(IAISF_BUILD_LINUX_NETWORK)
  find_package(Threads REQUIRED)

  add_library(
    iaisf_net
    STATIC
    src/net/channel.cpp
    src/net/epoll_poller.cpp
    src/net/event_loop.cpp
    src/net/socket.cpp
    src/net/tcp/ipv4_endpoint.cpp
  )
  add_library(iaisf::net ALIAS iaisf_net)

  target_compile_features(iaisf_net PUBLIC cxx_std_17)
  target_include_directories(
    iaisf_net
    PUBLIC
      "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
      "$<BUILD_INTERFACE:${IAISF_GENERATED_INCLUDE_DIR}>"
    PRIVATE
      "${PROJECT_SOURCE_DIR}/src/net"
  )
  target_link_libraries(
    iaisf_net
    PUBLIC
      iaisf::core
    PRIVATE
      Threads::Threads
  )
  iaisf_enable_project_warnings(iaisf_net)

  add_library(
    iaisf_tcp
    STATIC
    src/net/tcp/acceptor.cpp
    src/net/tcp/buffer.cpp
    src/net/tcp/tcp_connection.cpp
    src/net/tcp/tcp_server.cpp
  )
  add_library(iaisf::tcp ALIAS iaisf_tcp)
  target_compile_features(iaisf_tcp PUBLIC cxx_std_17)
  target_include_directories(
    iaisf_tcp
    PUBLIC
      "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
      "$<BUILD_INTERFACE:${IAISF_GENERATED_INCLUDE_DIR}>"
  )
  target_link_libraries(iaisf_tcp PUBLIC iaisf::net)
  iaisf_enable_project_warnings(iaisf_tcp)
endif()

add_executable(iaisf_server src/main.cpp)
target_compile_features(iaisf_server PRIVATE cxx_std_17)
target_link_libraries(iaisf_server PRIVATE iaisf::core)
iaisf_enable_project_warnings(iaisf_server)

if(IAISF_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
