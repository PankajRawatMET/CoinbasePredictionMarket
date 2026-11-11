add_library(cpm_quoting_mm
  src/quoting_model.cpp
  src/quoting_mm.cpp
)
target_include_directories(cpm_quoting_mm PUBLIC include)
target_link_libraries(cpm_quoting_mm PUBLIC cpm_strategy_host)
cpm_set_warnings(cpm_quoting_mm)

if(CPM_BUILD_TESTS)
  add_executable(cpm_quoting_mm_tests
    tests/quoting_model_test.cpp
    tests/quoting_mm_test.cpp
  )
  target_link_libraries(cpm_quoting_mm_tests PRIVATE cpm_quoting_mm GTest::gtest_main)
  cpm_set_warnings(cpm_quoting_mm_tests)
  gtest_discover_tests(cpm_quoting_mm_tests PROPERTIES TIMEOUT 60)
endif()
