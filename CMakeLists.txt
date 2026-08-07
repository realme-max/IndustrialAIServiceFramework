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
find_package(Threads REQUIRED)

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
  src/config/app_config.cpp
  src/core/error.cpp
  src/core/json_value_limits.cpp
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
target_link_libraries(iaisf_core PUBLIC nlohmann_json::nlohmann_json)
iaisf_enable_project_warnings(iaisf_core)

add_library(
  iaisf_application_core
  STATIC
  src/application/application_identity.cpp
  src/application/application_job.cpp
  src/application/application_job_id.cpp
  src/application/application_job_state.cpp
  src/application/application_submission.cpp
  src/application/artifact_ref.cpp
)
add_library(iaisf::application_core ALIAS iaisf_application_core)
target_compile_features(iaisf_application_core PUBLIC cxx_std_17)
target_include_directories(
  iaisf_application_core
  PUBLIC
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${IAISF_GENERATED_INCLUDE_DIR}>"
)
target_link_libraries(iaisf_application_core PUBLIC iaisf::core)
iaisf_enable_project_warnings(iaisf_application_core)

add_library(
  iaisf_application_api_primitives
  STATIC
  src/application/application_job_id_generator.cpp
  src/application/application_job_clock.cpp
)
if(WIN32)
  target_sources(
    iaisf_application_api_primitives
    PRIVATE
      src/application/application_job_id_entropy_windows.cpp
  )
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_sources(
    iaisf_application_api_primitives
    PRIVATE
      src/application/application_job_id_entropy_linux.cpp
  )
else()
  target_sources(
    iaisf_application_api_primitives
    PRIVATE
      src/application/application_job_id_entropy_unsupported.cpp
  )
endif()
add_library(iaisf::application_api_primitives ALIAS iaisf_application_api_primitives)
target_compile_features(iaisf_application_api_primitives PUBLIC cxx_std_17)
target_include_directories(
  iaisf_application_api_primitives
  PUBLIC
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${IAISF_GENERATED_INCLUDE_DIR}>"
  PRIVATE
    "${PROJECT_SOURCE_DIR}/src/application"
)
target_link_libraries(
  iaisf_application_api_primitives
  PUBLIC
    iaisf::application_core
)
if(WIN32)
  target_link_libraries(iaisf_application_api_primitives PRIVATE bcrypt)
endif()
iaisf_enable_project_warnings(iaisf_application_api_primitives)

add_library(
  iaisf_api_common
  STATIC
  src/api/strict_json.cpp
)
add_library(iaisf::api_common ALIAS iaisf_api_common)
target_compile_features(iaisf_api_common PUBLIC cxx_std_17)
target_include_directories(
  iaisf_api_common
  PUBLIC
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${IAISF_GENERATED_INCLUDE_DIR}>"
)
target_link_libraries(iaisf_api_common PUBLIC iaisf::core nlohmann_json::nlohmann_json)
iaisf_enable_project_warnings(iaisf_api_common)

add_library(
  iaisf_application_contract
  STATIC
  src/application/application_contract.cpp
  src/application/application_status_json.cpp
)
add_library(iaisf::application_contract ALIAS iaisf_application_contract)
target_compile_features(iaisf_application_contract PUBLIC cxx_std_17)
target_include_directories(
  iaisf_application_contract
  PUBLIC
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${IAISF_GENERATED_INCLUDE_DIR}>"
)
target_link_libraries(
  iaisf_application_contract
  PUBLIC
    iaisf::application_core
    iaisf::api_common
)
iaisf_enable_project_warnings(iaisf_application_contract)

add_library(
  iaisf_application_repository
  STATIC
  src/application/application_job_repository.cpp
  src/application/in_memory_application_job_repository.cpp
)
add_library(iaisf::application_repository ALIAS iaisf_application_repository)
target_compile_features(iaisf_application_repository PUBLIC cxx_std_17)
target_include_directories(
  iaisf_application_repository
  PUBLIC
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${IAISF_GENERATED_INCLUDE_DIR}>"
)
target_link_libraries(
  iaisf_application_repository
  PUBLIC
    iaisf::application_core
  PRIVATE
    Threads::Threads
)
iaisf_enable_project_warnings(iaisf_application_repository)

add_library(
  iaisf_logging
  STATIC
  src/logging/async_logger.cpp
  src/logging/console_sink.cpp
  src/logging/rolling_file_sink.cpp
)
add_library(iaisf::logging ALIAS iaisf_logging)
target_compile_features(iaisf_logging PUBLIC cxx_std_17)
target_include_directories(
  iaisf_logging
  PUBLIC
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${IAISF_GENERATED_INCLUDE_DIR}>"
)
target_link_libraries(iaisf_logging PUBLIC iaisf::core Threads::Threads)
iaisf_enable_project_warnings(iaisf_logging)

add_library(
  iaisf_metrics
  STATIC
  src/metrics/metrics.cpp
  src/metrics/prometheus_formatter.cpp
)
add_library(iaisf::metrics ALIAS iaisf_metrics)
target_compile_features(iaisf_metrics PUBLIC cxx_std_17)
target_include_directories(
  iaisf_metrics
  PUBLIC
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${IAISF_GENERATED_INCLUDE_DIR}>"
)
target_link_libraries(iaisf_metrics PUBLIC iaisf::core Threads::Threads)
iaisf_enable_project_warnings(iaisf_metrics)
target_link_libraries(iaisf_logging PUBLIC iaisf::metrics)

add_library(
  iaisf_health
  STATIC
  src/health/health_checker.cpp
)
add_library(iaisf::health ALIAS iaisf_health)
target_compile_features(iaisf_health PUBLIC cxx_std_17)
target_include_directories(
  iaisf_health
  PUBLIC
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${IAISF_GENERATED_INCLUDE_DIR}>"
)
iaisf_enable_project_warnings(iaisf_health)

add_library(
  iaisf_task
  STATIC
  src/task/task_executor.cpp
  src/task/task_limits.cpp
  src/task/task_manager.cpp
  src/task/task_repository.cpp
  src/task/task_types.cpp
  src/task/thread_pool.cpp
)
add_library(iaisf::task ALIAS iaisf_task)
target_compile_features(iaisf_task PUBLIC cxx_std_17)
target_include_directories(
  iaisf_task
  PUBLIC
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${IAISF_GENERATED_INCLUDE_DIR}>"
)
target_link_libraries(
  iaisf_task
  PUBLIC
    iaisf::core
    iaisf::metrics
    nlohmann_json::nlohmann_json
    Threads::Threads
)
iaisf_enable_project_warnings(iaisf_task)

add_library(iaisf_diagnostics STATIC src/diagnostics/runtime_diagnostics.cpp)
add_library(iaisf::diagnostics ALIAS iaisf_diagnostics)
target_compile_features(iaisf_diagnostics PUBLIC cxx_std_17)
target_include_directories(
  iaisf_diagnostics
  PUBLIC
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${IAISF_GENERATED_INCLUDE_DIR}>"
)
target_link_libraries(
  iaisf_diagnostics
  PUBLIC iaisf::core iaisf::health iaisf::task iaisf::logging iaisf::metrics
         iaisf::plugin
)
iaisf_enable_project_warnings(iaisf_diagnostics)

add_library(
  iaisf_plugin
  STATIC
  src/plugin/echo_plugin.cpp
  src/plugin/mock_vision_plugin.cpp
  src/plugin/plugin_limits.cpp
  src/plugin/plugin_manager.cpp
  src/plugin/plugin_metadata.cpp
  src/plugin/plugin_abi_adapter.cpp
  src/plugin/plugin_abi_validation.cpp
  src/plugin/dynamic_plugin_adapter.cpp
  src/plugin/detail/dynamic_module.cpp
  src/plugin/detail/dynamic_plugin_loader.cpp
  src/plugin/detail/plugin_safe_path.cpp
  src/plugin/plugin_task_adapter.cpp
  src/plugin/plugin_runtime.cpp
)
add_library(iaisf::plugin ALIAS iaisf_plugin)
target_compile_features(iaisf_plugin PUBLIC cxx_std_17)
target_include_directories(
  iaisf_plugin
  PUBLIC
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${IAISF_GENERATED_INCLUDE_DIR}>"
)
target_link_libraries(
  iaisf_plugin
  PUBLIC
    iaisf::task
    iaisf::core
    nlohmann_json::nlohmann_json
    ${CMAKE_DL_LIBS}
)
iaisf_enable_project_warnings(iaisf_plugin)

add_library(
  iaisf_http_core
  STATIC
  src/http/builtin_routes.cpp
  src/http/health_routes.cpp
  src/http/http_limits.cpp
  src/http/http_parser.cpp
  src/http/http_request.cpp
  src/http/http_response.cpp
  src/http/http_router.cpp
  src/http/http_status.cpp
)
add_library(iaisf::http_core ALIAS iaisf_http_core)
target_compile_features(iaisf_http_core PUBLIC cxx_std_17)
target_include_directories(
  iaisf_http_core
  PUBLIC
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${IAISF_GENERATED_INCLUDE_DIR}>"
)
target_link_libraries(iaisf_http_core PUBLIC iaisf::core iaisf::metrics iaisf::health)
iaisf_enable_project_warnings(iaisf_http_core)

add_library(
  iaisf_task_api
  STATIC
  src/api/task_api_limits.cpp
  src/api/task_http_api.cpp
  src/api/task_snapshot_json.cpp
)
add_library(iaisf::task_api ALIAS iaisf_task_api)
target_compile_features(iaisf_task_api PUBLIC cxx_std_17)
target_include_directories(
  iaisf_task_api
  PUBLIC
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${IAISF_GENERATED_INCLUDE_DIR}>"
)
target_link_libraries(
  iaisf_task_api
  PUBLIC
    iaisf::http_core
    iaisf::task
    iaisf::plugin
)
iaisf_enable_project_warnings(iaisf_task_api)

add_library(iaisf_diagnostics_http STATIC src/http/diagnostics_routes.cpp)
add_library(iaisf::diagnostics_http ALIAS iaisf_diagnostics_http)
target_compile_features(iaisf_diagnostics_http PUBLIC cxx_std_17)
target_include_directories(
  iaisf_diagnostics_http
  PUBLIC
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${IAISF_GENERATED_INCLUDE_DIR}>"
)
target_link_libraries(iaisf_diagnostics_http PUBLIC iaisf::http_core iaisf::diagnostics)
iaisf_enable_project_warnings(iaisf_diagnostics_http)

if(IAISF_BUILD_LINUX_NETWORK)
  add_library(
    iaisf_net
    STATIC
    src/net/channel.cpp
    src/net/epoll_poller.cpp
    src/net/event_loop.cpp
    src/net/signal_queue.cpp
    src/net/socket.cpp
    src/net/timer_queue.cpp
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
      iaisf::metrics
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

  add_library(
    iaisf_http
    STATIC
    src/http/http_server.cpp
    src/http/http_session.cpp
  )
  add_library(iaisf::http ALIAS iaisf_http)
  target_compile_features(iaisf_http PUBLIC cxx_std_17)
  target_include_directories(
    iaisf_http
    PUBLIC
      "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
      "$<BUILD_INTERFACE:${IAISF_GENERATED_INCLUDE_DIR}>"
  )
  target_link_libraries(
    iaisf_http
    PUBLIC
      iaisf::http_core
      iaisf::tcp
  )
  iaisf_enable_project_warnings(iaisf_http)

  add_library(
    iaisf_service
    STATIC
    src/service/industrial_ai_service.cpp
    src/service/runtime_options.cpp
    src/service/service_options.cpp
  )
  add_library(iaisf::service ALIAS iaisf_service)
  target_compile_features(iaisf_service PUBLIC cxx_std_17)
  target_include_directories(
    iaisf_service
    PUBLIC
      "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
      "$<BUILD_INTERFACE:${IAISF_GENERATED_INCLUDE_DIR}>"
  )
  target_link_libraries(
    iaisf_service
    PUBLIC
      iaisf::http
      iaisf::task_api
      iaisf::task
      iaisf::plugin
      iaisf::health
      iaisf::diagnostics_http
      iaisf::diagnostics
  )
  iaisf_enable_project_warnings(iaisf_service)
endif()

add_library(iaisf_app STATIC src/app/application.cpp)
add_library(iaisf::app ALIAS iaisf_app)
target_compile_features(iaisf_app PUBLIC cxx_std_17)
target_include_directories(
  iaisf_app
  PUBLIC
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${IAISF_GENERATED_INCLUDE_DIR}>"
)
target_link_libraries(iaisf_app PUBLIC iaisf::core iaisf::logging iaisf::metrics)
if(TARGET iaisf_service)
  target_link_libraries(iaisf_app PRIVATE iaisf::service)
  target_compile_definitions(iaisf_app PRIVATE IAISF_HAS_SERVICE_RUNTIME=1)
endif()
iaisf_enable_project_warnings(iaisf_app)

add_executable(iaisf_server src/main.cpp)
target_compile_features(iaisf_server PRIVATE cxx_std_17)
target_link_libraries(iaisf_server PRIVATE iaisf::app)
iaisf_enable_project_warnings(iaisf_server)

if(IAISF_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
