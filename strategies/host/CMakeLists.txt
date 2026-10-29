# The strategy host: the IStrategy seam, the parameter schemas and the runner
# that drives N strategies over one pair of sessions. It depends on both
# services for the decoded types; nothing in them depends on it, and no
# strategy kind is named here.

add_library(cpm_strategy_host
  src/host.cpp
  src/params.cpp
  src/registry.cpp
)
target_include_directories(cpm_strategy_host PUBLIC include)
target_link_libraries(cpm_strategy_host PUBLIC cpm_marketdata cpm_oms cpm_binance)
cpm_set_warnings(cpm_strategy_host)

if(CPM_BUILD_TESTS)
  add_executable(cpm_strategy_host_tests
    tests/params_test.cpp
    tests/host_test.cpp
  )
  target_link_libraries(cpm_strategy_host_tests PRIVATE cpm_strategy_host GTest::gtest_main)
  cpm_set_warnings(cpm_strategy_host_tests)
  gtest_discover_tests(cpm_strategy_host_tests)
endif()
