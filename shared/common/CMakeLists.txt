add_library(cpm_common
  src/config.cpp
  src/csv_log.cpp
  src/daily_file.cpp
  src/fixed.cpp
  src/https.cpp
  src/json.cpp
  src/lock_file.cpp
  src/log_files.cpp
  src/log_ring.cpp
  src/logging.cpp
  src/signing.cpp
  src/tls_stream.cpp
  src/websocket.cpp
)
target_include_directories(cpm_common PUBLIC include)
target_link_libraries(cpm_common PRIVATE OpenSSL::SSL OpenSSL::Crypto)
cpm_set_warnings(cpm_common)

if(CPM_BUILD_TESTS)
  add_executable(cpm_common_tests
    tests/config_test.cpp
    tests/csv_log_test.cpp
    tests/daily_file_test.cpp
    tests/fixed_test.cpp
    tests/json_test.cpp
    tests/lock_file_test.cpp
    tests/log_files_test.cpp
    tests/log_ring_test.cpp
    tests/signing_test.cpp
    tests/websocket_test.cpp
  )
  target_include_directories(cpm_common_tests PRIVATE tests)
  target_link_libraries(cpm_common_tests PRIVATE cpm_common GTest::gtest_main)
  cpm_set_warnings(cpm_common_tests)
  gtest_discover_tests(cpm_common_tests)
endif()
