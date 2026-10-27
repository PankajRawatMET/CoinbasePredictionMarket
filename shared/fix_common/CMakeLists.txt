add_library(cpm_fix_common
  src/initiator.cpp
  src/logon_auth.cpp
  src/raw_log.cpp
  src/session_settings.cpp
)
target_include_directories(cpm_fix_common PUBLIC include)
target_link_libraries(cpm_fix_common PUBLIC cpm_common cpm_quickfix)
cpm_set_warnings(cpm_fix_common)

if(CPM_BUILD_TESTS)
  add_executable(cpm_fix_common_tests
    tests/dictionary_test.cpp
    tests/logon_auth_test.cpp
    tests/raw_log_test.cpp
  )
  target_link_libraries(cpm_fix_common_tests PRIVATE cpm_fix_common GTest::gtest_main)
  target_compile_definitions(cpm_fix_common_tests PRIVATE CPM_DICT_DIR="${CPM_DICT_DIR}")
  cpm_set_warnings(cpm_fix_common_tests)
  gtest_discover_tests(cpm_fix_common_tests)
endif()
