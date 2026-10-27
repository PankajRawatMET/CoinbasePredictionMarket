add_library(cpm_marketdata
  src/book.cpp
  src/events.cpp
  src/feed_monitor.cpp
  src/market_state.cpp
  src/md_client.cpp
  src/fix/md_application.cpp
  src/fix/md_decoder.cpp
)
target_include_directories(cpm_marketdata
  PUBLIC include
  PRIVATE src
)
# Public API depends on cpm_common only; QuickFIX stays private (src/fix/).
target_link_libraries(cpm_marketdata
  PUBLIC cpm_common
  PRIVATE cpm_fix_common
)
cpm_set_warnings(cpm_marketdata)

add_executable(md_client apps/md_client/main.cpp)
target_link_libraries(md_client PRIVATE cpm_marketdata)
target_compile_definitions(md_client PRIVATE CPM_DEFAULT_DICT_DIR="${CPM_DICT_DIR}")
cpm_set_warnings(md_client)

if(CPM_BUILD_TESTS)
  add_executable(cpm_marketdata_tests
    tests/book_test.cpp
    tests/capture_replay_test.cpp
    tests/decoder_test.cpp
    tests/feed_monitor_test.cpp
    tests/market_state_test.cpp
    tests/session_loopback_test.cpp
  )
  target_link_libraries(cpm_marketdata_tests PRIVATE cpm_marketdata cpm_fix_common GTest::gtest_main)
  # decoder_test drives src/fix/md_decoder.hpp and reuses the fix_common wire builder.
  target_include_directories(cpm_marketdata_tests PRIVATE src ${CMAKE_SOURCE_DIR}/shared/fix_common/tests)
  target_compile_definitions(cpm_marketdata_tests
    PRIVATE CPM_DICT_DIR="${CPM_DICT_DIR}" CPM_CAPTURE_DIR="${CMAKE_SOURCE_DIR}/docs/spec")
  cpm_set_warnings(cpm_marketdata_tests)
  gtest_discover_tests(cpm_marketdata_tests PROPERTIES TIMEOUT 60)
endif()

# --- md_sim: local stand-in for the CPM MD gateway (Stage 2) -----------------
add_library(cpm_md_sim
  sim/src/market.cpp
  sim/src/request.cpp
  sim/src/simulator.cpp
  sim/src/fix/sim_application.cpp
)
target_include_directories(cpm_md_sim
  PUBLIC sim/include
  PRIVATE sim/src
)
target_link_libraries(cpm_md_sim
  PUBLIC cpm_common
  PRIVATE cpm_fix_common
)
cpm_set_warnings(cpm_md_sim)

add_executable(md_sim apps/md_sim/main.cpp)
target_link_libraries(md_sim PRIVATE cpm_md_sim)
target_compile_definitions(md_sim PRIVATE CPM_DEFAULT_DICT_DIR="${CPM_DICT_DIR}")
cpm_set_warnings(md_sim)

if(CPM_BUILD_TESTS)
  add_executable(cpm_md_sim_tests
    tests/halt_test.cpp
    tests/md_events_test.cpp
    tests/sim_market_test.cpp
    tests/sim_request_test.cpp
    tests/sim_session_test.cpp
  )
  target_link_libraries(cpm_md_sim_tests PRIVATE cpm_md_sim cpm_marketdata GTest::gtest_main)
  target_compile_definitions(cpm_md_sim_tests PRIVATE CPM_DICT_DIR="${CPM_DICT_DIR}")
  cpm_set_warnings(cpm_md_sim_tests)
  gtest_discover_tests(cpm_md_sim_tests PROPERTIES TIMEOUT 60)
endif()
