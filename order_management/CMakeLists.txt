# Order entry service (CPMOE).

add_library(cpm_oms
  src/oe_client.cpp
  src/bookmark.cpp
  src/order_rules.cpp
  src/reconciler.cpp
  src/order_state.cpp
  src/orders.cpp
  src/fix/oe_application.cpp
  src/fix/oe_decoder.cpp
  src/fix/oe_messages.cpp
)
target_include_directories(cpm_oms
  PUBLIC include
  PRIVATE src
)
# Public API depends on cpm_common only; QuickFIX stays private (src/fix/).
target_link_libraries(cpm_oms
  PUBLIC cpm_common
  PRIVATE cpm_fix_common
)
cpm_set_warnings(cpm_oms)

add_executable(oe_client apps/oe_client/main.cpp)
target_link_libraries(oe_client PRIVATE cpm_oms)
target_compile_definitions(oe_client PRIVATE CPM_DEFAULT_DICT_DIR="${CPM_DICT_DIR}")
cpm_set_warnings(oe_client)

# --- oe_sim: local stand-in for the CPM order entry gateway ------------------
add_library(cpm_oe_sim
  sim/src/engine.cpp
  sim/src/simulator.cpp
  sim/src/fix/sim_application.cpp
)
target_include_directories(cpm_oe_sim
  PUBLIC sim/include
  PRIVATE sim/src
)
target_link_libraries(cpm_oe_sim
  PUBLIC cpm_oms
  PRIVATE cpm_fix_common
)
cpm_set_warnings(cpm_oe_sim)

add_executable(oe_sim apps/oe_sim/main.cpp)
target_link_libraries(oe_sim PRIVATE cpm_oe_sim)
target_compile_definitions(oe_sim PRIVATE CPM_DEFAULT_DICT_DIR="${CPM_DICT_DIR}")
cpm_set_warnings(oe_sim)

if(CPM_BUILD_TESTS)
  add_executable(cpm_oms_tests
    tests/capture_replay_test.cpp
    tests/dictionary_test.cpp
    tests/messages_test.cpp
    tests/order_state_test.cpp
    tests/recovery_test.cpp
    tests/orders_test.cpp
    tests/sim_engine_test.cpp
    tests/sim_session_test.cpp
    tests/session_loopback_test.cpp
  )
  target_link_libraries(cpm_oms_tests PRIVATE cpm_oe_sim cpm_oms cpm_fix_common GTest::gtest_main)
  # messages_test drives src/fix/ directly.
  target_include_directories(cpm_oms_tests PRIVATE src)
  # Reuses the shared wire builder from the fix_common tests.
  target_include_directories(cpm_oms_tests PRIVATE ${CMAKE_SOURCE_DIR}/shared/fix_common/tests)
  target_compile_definitions(cpm_oms_tests
    PRIVATE CPM_DICT_DIR="${CPM_DICT_DIR}" CPM_CAPTURE_DIR="${CMAKE_SOURCE_DIR}/docs/spec")
  cpm_set_warnings(cpm_oms_tests)
  gtest_discover_tests(cpm_oms_tests PROPERTIES TIMEOUT 60)
endif()
