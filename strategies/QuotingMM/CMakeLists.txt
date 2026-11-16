add_library(cpm_quoting_mm
  src/quoting_model.cpp
  src/quoting_mm.cpp
  # The REST kline reader, and the once-per-window warmup the strategy is
  # built from: its opening price and its sigma.
  src/quoting_klines.cpp
  src/quoting_warmup.cpp
)
target_include_directories(cpm_quoting_mm PUBLIC include)
target_link_libraries(cpm_quoting_mm PUBLIC cpm_strategy_host cpm_common)
cpm_set_warnings(cpm_quoting_mm)

# A reading tool, not part of the daemon: the last n minutes of futures OHLC,
# printed, so REST history can be compared with what the live stream builds.
add_executable(binance_klines apps/klines/main.cpp)
target_link_libraries(binance_klines PRIVATE cpm_quoting_mm)
cpm_set_warnings(binance_klines)

if(CPM_BUILD_TESTS)
  add_executable(cpm_quoting_mm_tests
    tests/quoting_model_test.cpp
    tests/quoting_mm_test.cpp
    tests/quoting_klines_test.cpp
    tests/quoting_warmup_test.cpp
  )
  target_link_libraries(cpm_quoting_mm_tests PRIVATE cpm_quoting_mm GTest::gtest_main)
  cpm_set_warnings(cpm_quoting_mm_tests)
  gtest_discover_tests(cpm_quoting_mm_tests PROPERTIES TIMEOUT 60)
endif()
