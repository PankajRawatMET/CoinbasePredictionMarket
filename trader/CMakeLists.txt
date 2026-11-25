# The daemon: one session pair, one of every piece of state the venue counts,
# and the operator's surface over them. The GUI in gui/ sits on ControlApi and
# adds the only sockets in the project that are not FIX.

add_library(cpm_trader_lib
  src/control_api.cpp
  src/fill_recovery.cpp
  src/trader.cpp
)
target_include_directories(cpm_trader_lib PUBLIC include)
target_link_libraries(cpm_trader_lib PUBLIC cpm_binance cpm_strategy_host)
cpm_set_warnings(cpm_trader_lib)

add_executable(cpm_trader apps/cpm_trader/main.cpp)
# The daemon knows the strategy kinds; nothing below it does. cpm_gui is the
# operator's surface over ControlApi and is defined in gui/, which depends on
# this directory rather than the other way round.
target_link_libraries(cpm_trader PRIVATE cpm_trader_lib cpm_crypto_mm cpm_quoting_mm cpm_gui)
target_compile_definitions(cpm_trader PRIVATE CPM_DEFAULT_DICT_DIR="${CPM_DICT_DIR}")
cpm_set_warnings(cpm_trader)

if(CPM_BUILD_TESTS)
  add_executable(cpm_trader_tests
    tests/control_api_test.cpp
    tests/trader_test.cpp
  )
  target_link_libraries(cpm_trader_tests PRIVATE cpm_trader_lib cpm_crypto_mm cpm_md_sim cpm_oe_sim
                                                 GTest::gtest_main)
  target_compile_definitions(cpm_trader_tests PRIVATE CPM_DICT_DIR="${CPM_DICT_DIR}")
  cpm_set_warnings(cpm_trader_tests)
  gtest_discover_tests(cpm_trader_tests PROPERTIES TIMEOUT 120)
endif()
