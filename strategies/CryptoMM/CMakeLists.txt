# CryptoMM: a two-sided quoting strategy over both containers. It depends on
# market_data and order_management; neither depends on it.

add_library(cpm_crypto_mm src/crypto_mm.cpp)
target_include_directories(cpm_crypto_mm PUBLIC include)
target_link_libraries(cpm_crypto_mm PUBLIC cpm_strategy_host)
cpm_set_warnings(cpm_crypto_mm)

add_executable(crypto_mm apps/crypto_mm/main.cpp)
target_link_libraries(crypto_mm PRIVATE cpm_crypto_mm)
target_compile_definitions(crypto_mm PRIVATE CPM_DEFAULT_DICT_DIR="${CPM_DICT_DIR}")
cpm_set_warnings(crypto_mm)

if(CPM_BUILD_TESTS)
  add_executable(cpm_crypto_mm_tests tests/crypto_mm_test.cpp)
  target_link_libraries(cpm_crypto_mm_tests PRIVATE cpm_crypto_mm cpm_oe_sim GTest::gtest_main)
  target_compile_definitions(cpm_crypto_mm_tests PRIVATE CPM_DICT_DIR="${CPM_DICT_DIR}")
  cpm_set_warnings(cpm_crypto_mm_tests)
  gtest_discover_tests(cpm_crypto_mm_tests PROPERTIES TIMEOUT 60)
endif()
