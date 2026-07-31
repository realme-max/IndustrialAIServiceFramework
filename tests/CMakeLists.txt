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
    iaisf::core
    GTest::gtest
    GTest::gtest_main
)
target_compile_definitions(
  iaisf_tests
  PRIVATE
    IAISF_EXAMPLE_CONFIG_PATH="${PROJECT_SOURCE_DIR}/config/iaisf.example.json"
)
iaisf_enable_project_warnings(iaisf_tests)

include(GoogleTest)
gtest_add_tests(TARGET iaisf_tests TEST_LIST IAISF_UNIT_TESTS)
set_tests_properties(${IAISF_UNIT_TESTS} PROPERTIES LABELS unit)

add_executable(
  iaisf_http_core_tests
  http/test_http_limits.cpp
  http/test_http_parser.cpp
  http/test_http_request_response.cpp
  http/test_http_router.cpp
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
  plugin/test_plugin_task_adapter.cpp
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
    net/test_socket.cpp
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
