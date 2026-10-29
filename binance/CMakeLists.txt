add_library(cpm_binance
  src/config.cpp
  src/reference.cpp
)
target_include_directories(cpm_binance PUBLIC include)
target_link_libraries(cpm_binance PUBLIC cpm_common)
cpm_set_warnings(cpm_binance)

# The simulator is the other end of the same protocol, and the reason every
# test of the speed bump can run with no network at all.
add_executable(binance_sim apps/binance_sim/main.cpp)
target_link_libraries(binance_sim PRIVATE cpm_binance)
cpm_set_warnings(binance_sim)

if(CPM_BUILD_TESTS)
  add_executable(cpm_binance_tests
    tests/config_test.cpp
    tests/reference_test.cpp
  )
  target_link_libraries(cpm_binance_tests PRIVATE cpm_binance GTest::gtest_main)
  # The config file shipped in config/ is read by a test: a comment in it
  # that was not a comment would be found there before a run found it.
  target_compile_definitions(cpm_binance_tests PRIVATE CPM_DICT_DIR="${CPM_DICT_DIR}")
  cpm_set_warnings(cpm_binance_tests)
  gtest_discover_tests(cpm_binance_tests PROPERTIES TIMEOUT 120)
endif()
