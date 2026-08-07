enable_language(C)

add_executable(
  iaisf_tests
  test_app_config.cpp
  test_application.cpp
  test_console_logger.cpp
  test_error_result.cpp
)

target_compile_features(iaisf_tests PRIVATE cxx_std_17)
target_link_libraries(
  iaisf_tests
  PRIVATE
    iaisf::app
    GTest::gtest
    GTest::gtest_main
)
target_compile_definitions(
  iaisf_tests
  PRIVATE
    IAISF_EXAMPLE_CONFIG_PATH="${PROJECT_SOURCE_DIR}/config/iaisf.example.json"
)
iaisf_enable_project_warnings(iaisf_tests)

if(WIN32)
  target_sources(iaisf_tests PRIVATE app/test_application_logging.cpp)
endif()

include(GoogleTest)
gtest_add_tests(TARGET iaisf_tests TEST_LIST IAISF_UNIT_TESTS)
set_tests_properties(${IAISF_UNIT_TESTS} PROPERTIES LABELS unit)

add_executable(
  iaisf_application_core_tests
  application/test_application_identity.cpp
  application/test_application_job_state.cpp
  application/test_application_submission.cpp
  application/test_artifact_ref.cpp
)
target_compile_features(iaisf_application_core_tests PRIVATE cxx_std_17)
target_link_libraries(
  iaisf_application_core_tests
  PRIVATE
    iaisf::application_core
    GTest::gtest
    GTest::gtest_main
)
iaisf_enable_project_warnings(iaisf_application_core_tests)
gtest_add_tests(
  TARGET iaisf_application_core_tests
  TEST_LIST IAISF_APPLICATION_CORE_TESTS
)
set_tests_properties(
  ${IAISF_APPLICATION_CORE_TESTS}
  PROPERTIES LABELS "unit;application" TIMEOUT 10
)

add_executable(
  iaisf_application_api_primitives_tests
  application/test_application_job_id_generator.cpp
  application/test_application_job_clock.cpp
)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_sources(
    iaisf_application_api_primitives_tests
    PRIVATE
      application/test_application_job_id_entropy_linux.cpp
  )
endif()
target_compile_features(iaisf_application_api_primitives_tests PRIVATE cxx_std_17)
target_include_directories(
  iaisf_application_api_primitives_tests
  PRIVATE
    "${PROJECT_SOURCE_DIR}/src/application"
)
target_link_libraries(
  iaisf_application_api_primitives_tests
  PRIVATE
    iaisf::application_api_primitives
    GTest::gtest
    GTest::gtest_main
    Threads::Threads
)
iaisf_enable_project_warnings(iaisf_application_api_primitives_tests)
gtest_add_tests(
  TARGET iaisf_application_api_primitives_tests
  TEST_LIST IAISF_APPLICATION_API_PRIMITIVES_TESTS
)
set_tests_properties(
  ${IAISF_APPLICATION_API_PRIMITIVES_TESTS}
  PROPERTIES LABELS "unit;application;api_primitives" TIMEOUT 20
)

add_executable(
  iaisf_api_common_tests
  api/test_strict_json.cpp
)
target_compile_features(iaisf_api_common_tests PRIVATE cxx_std_17)
target_link_libraries(
  iaisf_api_common_tests
  PRIVATE
    iaisf::api_common
    GTest::gtest
    GTest::gtest_main
)
iaisf_enable_project_warnings(iaisf_api_common_tests)
gtest_add_tests(
  TARGET iaisf_api_common_tests
  TEST_LIST IAISF_API_COMMON_TESTS
)
set_tests_properties(
  ${IAISF_API_COMMON_TESTS}
  PROPERTIES LABELS "unit;api_common" TIMEOUT 20
)

add_executable(
  iaisf_application_contract_tests
  application/test_application_contract.cpp
  application/test_application_status_json.cpp
)
target_compile_features(iaisf_application_contract_tests PRIVATE cxx_std_17)
target_link_libraries(
  iaisf_application_contract_tests
  PRIVATE
    iaisf::application_contract
    GTest::gtest
    GTest::gtest_main
)
iaisf_enable_project_warnings(iaisf_application_contract_tests)
gtest_add_tests(
  TARGET iaisf_application_contract_tests
  TEST_LIST IAISF_APPLICATION_CONTRACT_TESTS
)
set_tests_properties(
  ${IAISF_APPLICATION_CONTRACT_TESTS}
  PROPERTIES LABELS "unit;application;contract" TIMEOUT 20
)

add_executable(
  iaisf_application_repository_tests
  application/test_application_job_id.cpp
  application/test_application_job.cpp
  application/test_application_job_repository.cpp
)
target_compile_features(iaisf_application_repository_tests PRIVATE cxx_std_17)
target_link_libraries(
  iaisf_application_repository_tests
  PRIVATE
    iaisf::application_repository
    GTest::gtest
    GTest::gtest_main
    Threads::Threads
)
iaisf_enable_project_warnings(iaisf_application_repository_tests)
gtest_add_tests(
  TARGET iaisf_application_repository_tests
  TEST_LIST IAISF_APPLICATION_REPOSITORY_TESTS
)
set_tests_properties(
  ${IAISF_APPLICATION_REPOSITORY_TESTS}
  PROPERTIES LABELS "unit;application;repository" TIMEOUT 20
)

add_executable(
  iaisf_logging_tests
  log/test_async_logger.cpp
  log/test_logger_metrics.cpp
  log/test_rolling_file_sink.cpp
  log/test_logging_config.cpp
)
target_compile_features(iaisf_logging_tests PRIVATE cxx_std_17)
target_link_libraries(
  iaisf_logging_tests
  PRIVATE
    iaisf::logging
    GTest::gtest
    GTest::gtest_main
    Threads::Threads
)
iaisf_enable_project_warnings(iaisf_logging_tests)

gtest_add_tests(TARGET iaisf_logging_tests TEST_LIST IAISF_LOGGING_TESTS)
set_tests_properties(
  ${IAISF_LOGGING_TESTS}
  PROPERTIES
    LABELS "unit;logging"
    TIMEOUT 20
)

add_executable(
  iaisf_metrics_tests
  metrics/test_metrics.cpp
  metrics/test_metrics_formatter.cpp
)
target_compile_features(iaisf_metrics_tests PRIVATE cxx_std_17)
target_link_libraries(
  iaisf_metrics_tests
  PRIVATE
    iaisf::metrics
    GTest::gtest
    GTest::gtest_main
    Threads::Threads
)
iaisf_enable_project_warnings(iaisf_metrics_tests)

gtest_add_tests(
  TARGET iaisf_metrics_tests
  TEST_LIST IAISF_METRICS_TESTS
)
set_tests_properties(
  ${IAISF_METRICS_TESTS}
  PROPERTIES
    LABELS "unit;metrics"
    TIMEOUT 20
)

add_executable(
  iaisf_health_tests
  health/test_health_checker.cpp
)
target_compile_features(iaisf_health_tests PRIVATE cxx_std_17)
target_link_libraries(
  iaisf_health_tests
  PRIVATE
    iaisf::health
    GTest::gtest
    GTest::gtest_main
    Threads::Threads
)
iaisf_enable_project_warnings(iaisf_health_tests)

gtest_add_tests(
  TARGET iaisf_health_tests
  TEST_LIST IAISF_HEALTH_TESTS
)
set_tests_properties(
  ${IAISF_HEALTH_TESTS}
  PROPERTIES
    LABELS "unit;health"
    TIMEOUT 10
)

add_executable(
  iaisf_diagnostics_tests
  diagnostics/test_runtime_diagnostics.cpp
)
target_compile_features(iaisf_diagnostics_tests PRIVATE cxx_std_17)
target_link_libraries(
  iaisf_diagnostics_tests
  PRIVATE
    iaisf::diagnostics
    GTest::gtest
    GTest::gtest_main
    Threads::Threads
)
iaisf_enable_project_warnings(iaisf_diagnostics_tests)
gtest_add_tests(TARGET iaisf_diagnostics_tests TEST_LIST IAISF_DIAGNOSTICS_TESTS)
set_tests_properties(
  ${IAISF_DIAGNOSTICS_TESTS}
  PROPERTIES LABELS "unit;diagnostics" TIMEOUT 10
)

add_executable(
  iaisf_diagnostics_http_tests
  http/test_diagnostics_endpoint.cpp
)
target_compile_features(iaisf_diagnostics_http_tests PRIVATE cxx_std_17)
target_link_libraries(
  iaisf_diagnostics_http_tests
  PRIVATE
    iaisf::diagnostics_http
    GTest::gtest
    GTest::gtest_main
)
iaisf_enable_project_warnings(iaisf_diagnostics_http_tests)
gtest_add_tests(
  TARGET iaisf_diagnostics_http_tests TEST_LIST IAISF_DIAGNOSTICS_HTTP_TESTS
)
set_tests_properties(
  ${IAISF_DIAGNOSTICS_HTTP_TESTS}
  PROPERTIES LABELS "unit;http;diagnostics" TIMEOUT 10
)

add_executable(
  iaisf_http_core_tests
  http/test_http_limits.cpp
  http/test_http_parser.cpp
  http/test_http_request_response.cpp
  http/test_http_router.cpp
  http/test_health_endpoint.cpp
  http/test_metrics_endpoint.cpp
)
target_compile_features(iaisf_http_core_tests PRIVATE cxx_std_17)
target_link_libraries(
  iaisf_http_core_tests
  PRIVATE
    iaisf::http_core
    GTest::gtest
    GTest::gtest_main
)
iaisf_enable_project_warnings(iaisf_http_core_tests)

gtest_add_tests(
  TARGET iaisf_http_core_tests
  TEST_LIST IAISF_HTTP_CORE_TESTS
)
set_tests_properties(
  ${IAISF_HTTP_CORE_TESTS}
  PROPERTIES
    LABELS "unit;http"
    TIMEOUT 10
)

add_executable(
  iaisf_task_tests
  task/test_task_executor.cpp
  task/test_task_limits.cpp
  task/test_task_manager.cpp
  task/test_task_metrics.cpp
  task/test_task_repository.cpp
  task/test_task_types.cpp
  task/test_thread_pool.cpp
)
target_compile_features(iaisf_task_tests PRIVATE cxx_std_17)
target_link_libraries(
  iaisf_task_tests
  PRIVATE
    iaisf::task
    GTest::gtest
    GTest::gtest_main
    Threads::Threads
)
iaisf_enable_project_warnings(iaisf_task_tests)

gtest_add_tests(TARGET iaisf_task_tests TEST_LIST IAISF_TASK_TESTS)
set_tests_properties(
  ${IAISF_TASK_TESTS}
  PROPERTIES
    LABELS "unit;task"
    TIMEOUT 10
)

add_executable(
  iaisf_plugin_tests
  plugin/test_echo_plugin.cpp
  plugin/test_mock_vision_plugin.cpp
  plugin/test_plugin_limits.cpp
  plugin/test_plugin_manager.cpp
  plugin/test_plugin_metadata.cpp
  plugin/test_plugin_abi.cpp
  plugin/test_plugin_task_adapter.cpp
  plugin/test_plugin_runtime.cpp
)
target_compile_features(iaisf_plugin_tests PRIVATE cxx_std_17)
target_link_libraries(
  iaisf_plugin_tests
  PRIVATE
    iaisf::plugin
    GTest::gtest
    GTest::gtest_main
    Threads::Threads
)
iaisf_enable_project_warnings(iaisf_plugin_tests)

gtest_add_tests(TARGET iaisf_plugin_tests TEST_LIST IAISF_PLUGIN_TESTS)
set_tests_properties(
  ${IAISF_PLUGIN_TESTS}
  PROPERTIES
    LABELS "unit;plugin"
  TIMEOUT 10
)

add_executable(iaisf_plugin_abi_c_include plugin/test_plugin_abi_c.c)
set_target_properties(
  iaisf_plugin_abi_c_include
  PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS OFF
)
target_include_directories(
  iaisf_plugin_abi_c_include
  PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
)
add_test(
  NAME iaisf_plugin_abi_c_include
  COMMAND iaisf_plugin_abi_c_include
)
set_tests_properties(
  iaisf_plugin_abi_c_include
  PROPERTIES LABELS "unit;plugin" TIMEOUT 10
)

add_library(
  iaisf_test_fixture_plugin
  MODULE
  plugin/fixtures/valid_fixture_plugin.cpp
)
target_compile_features(iaisf_test_fixture_plugin PRIVATE cxx_std_17)
target_include_directories(
  iaisf_test_fixture_plugin
  PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
)
set_target_properties(
  iaisf_test_fixture_plugin
  PROPERTIES
    PREFIX ""
)

add_library(
  iaisf_dynamic_fixture_plugin
  MODULE
  plugin/fixtures/fixture_dynamic_plugin.cpp
)
target_compile_features(iaisf_dynamic_fixture_plugin PRIVATE cxx_std_17)
target_include_directories(
  iaisf_dynamic_fixture_plugin
  PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
)
set_target_properties(
  iaisf_dynamic_fixture_plugin
  PROPERTIES
    PREFIX ""
)

add_executable(
  iaisf_plugin_loader_tests
  plugin/test_dynamic_module.cpp
  plugin/test_plugin_safe_path.cpp
)
target_compile_features(iaisf_plugin_loader_tests PRIVATE cxx_std_17)
target_compile_definitions(
  iaisf_plugin_loader_tests
  PRIVATE
    IAISF_TEST_FIXTURE_PATH="$<TARGET_FILE:iaisf_test_fixture_plugin>"
    IAISF_DYNAMIC_FIXTURE_PATH="$<TARGET_FILE:iaisf_dynamic_fixture_plugin>"
)
target_link_libraries(
  iaisf_plugin_loader_tests
  PRIVATE
    iaisf::plugin
    GTest::gtest
    GTest::gtest_main
    Threads::Threads
)
add_dependencies(
  iaisf_plugin_loader_tests
  iaisf_test_fixture_plugin
  iaisf_dynamic_fixture_plugin
)
iaisf_enable_project_warnings(iaisf_plugin_loader_tests)

gtest_add_tests(
  TARGET iaisf_plugin_loader_tests
  TEST_LIST IAISF_PLUGIN_LOADER_TESTS
)
set_tests_properties(
  ${IAISF_PLUGIN_LOADER_TESTS}
  PROPERTIES
    LABELS "unit;plugin;dynamic_loader"
    TIMEOUT 10
)

add_executable(
  iaisf_dynamic_plugin_adapter_tests
  plugin/test_dynamic_plugin_adapter.cpp
)
target_compile_features(iaisf_dynamic_plugin_adapter_tests PRIVATE cxx_std_17)
target_compile_definitions(
  iaisf_dynamic_plugin_adapter_tests
  PRIVATE
    IAISF_DYNAMIC_FIXTURE_PATH="$<TARGET_FILE:iaisf_dynamic_fixture_plugin>"
)
target_link_libraries(
  iaisf_dynamic_plugin_adapter_tests
  PRIVATE
    iaisf::plugin
    iaisf::diagnostics
    GTest::gtest
    GTest::gtest_main
    Threads::Threads
)
add_dependencies(
  iaisf_dynamic_plugin_adapter_tests
  iaisf_dynamic_fixture_plugin
)
iaisf_enable_project_warnings(iaisf_dynamic_plugin_adapter_tests)

gtest_add_tests(
  TARGET iaisf_dynamic_plugin_adapter_tests
  TEST_LIST IAISF_DYNAMIC_PLUGIN_ADAPTER_TESTS
)
set_tests_properties(
  ${IAISF_DYNAMIC_PLUGIN_ADAPTER_TESTS}
  PROPERTIES
    LABELS "unit;plugin;dynamic_loader"
    TIMEOUT 10
)

add_executable(
  iaisf_task_api_tests
  api/test_task_http_api.cpp
)
target_compile_features(iaisf_task_api_tests PRIVATE cxx_std_17)
target_link_libraries(
  iaisf_task_api_tests
  PRIVATE
    iaisf::task_api
    GTest::gtest
    GTest::gtest_main
    Threads::Threads
)
iaisf_enable_project_warnings(iaisf_task_api_tests)

gtest_add_tests(
  TARGET iaisf_task_api_tests
  TEST_LIST IAISF_TASK_API_TESTS
)
set_tests_properties(
  ${IAISF_TASK_API_TESTS}
  PROPERTIES
    LABELS "unit;task_api"
    TIMEOUT 10
)

if(TARGET iaisf_net)
  add_executable(
    iaisf_net_tests
    net/test_channel.cpp
    net/test_epoll_poller.cpp
    net/test_event_loop.cpp
    net/test_signal_queue.cpp
    net/test_socket.cpp
    net/test_timer_queue.cpp
    net/test_unique_fd.cpp
  )
  target_compile_features(iaisf_net_tests PRIVATE cxx_std_17)
  target_link_libraries(
    iaisf_net_tests
    PRIVATE
      iaisf::net
      GTest::gtest
      GTest::gtest_main
      Threads::Threads
  )
  target_include_directories(
    iaisf_net_tests
    PRIVATE
      "${PROJECT_SOURCE_DIR}/src/net"
  )
  iaisf_enable_project_warnings(iaisf_net_tests)

  gtest_add_tests(TARGET iaisf_net_tests TEST_LIST IAISF_NET_TESTS)
  set_tests_properties(
    ${IAISF_NET_TESTS}
    PROPERTIES
      LABELS "unit;linux"
      TIMEOUT 10
  )
endif()

if(TARGET iaisf_tcp)
  add_executable(
    iaisf_tcp_tests
    net/tcp/test_acceptor.cpp
    net/tcp/test_buffer.cpp
    net/tcp/test_ipv4_endpoint.cpp
    net/tcp/test_tcp_metrics.cpp
    net/tcp/test_tcp_connection.cpp
    net/tcp/test_tcp_server.cpp
  )
  target_compile_features(iaisf_tcp_tests PRIVATE cxx_std_17)
  target_link_libraries(
    iaisf_tcp_tests
    PRIVATE
      iaisf::tcp
      GTest::gtest
      GTest::gtest_main
      Threads::Threads
  )
  iaisf_enable_project_warnings(iaisf_tcp_tests)

  gtest_add_tests(TARGET iaisf_tcp_tests TEST_LIST IAISF_TCP_TESTS)
  set_tests_properties(
    ${IAISF_TCP_TESTS}
    PROPERTIES
      LABELS "unit;integration;linux;tcp"
      TIMEOUT 20
  )
endif()

if(TARGET iaisf_http)
  add_executable(
    iaisf_http_tests
    http/test_http_server.cpp
    http/test_http_metrics.cpp
  )
  target_compile_features(iaisf_http_tests PRIVATE cxx_std_17)
  target_link_libraries(
    iaisf_http_tests
    PRIVATE
      iaisf::http
      GTest::gtest
      GTest::gtest_main
      Threads::Threads
  )
  iaisf_enable_project_warnings(iaisf_http_tests)

  gtest_add_tests(TARGET iaisf_http_tests TEST_LIST IAISF_HTTP_TESTS)
  set_tests_properties(
    ${IAISF_HTTP_TESTS}
    PROPERTIES
      LABELS "integration;linux;http"
      TIMEOUT 20
  )
endif()

if(TARGET iaisf_service)
  add_executable(
    iaisf_service_tests
    service/test_industrial_ai_service.cpp
    service/test_health_lifecycle.cpp
    service/test_runtime_options.cpp
  )
  target_compile_features(iaisf_service_tests PRIVATE cxx_std_17)
  target_link_libraries(
    iaisf_service_tests
    PRIVATE
      iaisf::service
      GTest::gtest
      GTest::gtest_main
      Threads::Threads
  )
  target_compile_definitions(
    iaisf_service_tests
    PRIVATE
      IAISF_DYNAMIC_FIXTURE_PATH="$<TARGET_FILE:iaisf_dynamic_fixture_plugin>"
  )
  add_dependencies(iaisf_service_tests iaisf_dynamic_fixture_plugin)
  iaisf_enable_project_warnings(iaisf_service_tests)

  gtest_add_tests(
    TARGET iaisf_service_tests
    TEST_LIST IAISF_SERVICE_TESTS
  )
  set_tests_properties(
    ${IAISF_SERVICE_TESTS}
    PROPERTIES
      LABELS "integration;linux;service"
      TIMEOUT 20
  )

  add_executable(
    iaisf_app_service_tests
    app/test_application_service.cpp
    app/test_application_logging.cpp
  )
  target_compile_features(iaisf_app_service_tests PRIVATE cxx_std_17)
  target_link_libraries(
    iaisf_app_service_tests
    PRIVATE
      iaisf::app
      GTest::gtest
      GTest::gtest_main
  )
  target_compile_definitions(
    iaisf_app_service_tests
    PRIVATE
      IAISF_SERVER_PATH="$<TARGET_FILE:iaisf_server>"
  )
  add_dependencies(iaisf_app_service_tests iaisf_server)
  iaisf_enable_project_warnings(iaisf_app_service_tests)

  gtest_add_tests(
    TARGET iaisf_app_service_tests
    TEST_LIST IAISF_APP_SERVICE_TESTS
  )
  set_tests_properties(
    ${IAISF_APP_SERVICE_TESTS}
    PROPERTIES
      LABELS "integration;linux;service;config"
      TIMEOUT 20
  )
endif()

add_test(NAME iaisf_server_version COMMAND $<TARGET_FILE:iaisf_server> --version)
set_tests_properties(
  iaisf_server_version
  PROPERTIES
    LABELS smoke
    PASS_REGULAR_EXPRESSION "^IndustrialAIServiceFramework 0\\.1\\.0"
)

add_test(
  NAME iaisf_server_example_config
  COMMAND
    $<TARGET_FILE:iaisf_server>
    --config
    "${PROJECT_SOURCE_DIR}/config/iaisf.example.json"
)
set_tests_properties(
  iaisf_server_example_config
  PROPERTIES
    LABELS smoke
    PASS_REGULAR_EXPRESSION "configuration validated"
)
