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
