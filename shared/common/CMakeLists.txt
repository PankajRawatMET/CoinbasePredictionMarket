add_library(cpm_common
  src/config.cpp
  src/fixed.cpp
  src/logging.cpp
  src/signing.cpp
)
target_include_directories(cpm_common PUBLIC include)
target_link_libraries(cpm_common PRIVATE OpenSSL::Crypto)
cpm_set_warnings(cpm_common)

if(CPM_BUILD_TESTS)
  add_executable(cpm_common_tests
    tests/config_test.cpp
    tests/fixed_test.cpp
    tests/signing_test.cpp
  )
  target_link_libraries(cpm_common_tests PRIVATE cpm_common GTest::gtest_main)
  cpm_set_warnings(cpm_common_tests)
  gtest_discover_tests(cpm_common_tests)
endif()
