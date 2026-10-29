# The GUI: the only non-FIX sockets in the project. It sits on ControlApi and
# adds nothing to it - every refusal, every limit and every halt is decided
# below this directory (design-gui.md section 5).

# The page is a real editable file that becomes a string in the binary, so
# there is no runtime asset path and no file-serving code path (GUI-Q8).
set(CPM_GUI_PAGE_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/web/index.html")
set(CPM_GUI_PAGE_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/page_data.cpp")
add_custom_command(
  OUTPUT "${CPM_GUI_PAGE_GENERATED}"
  COMMAND ${CMAKE_COMMAND}
          -DINPUT=${CPM_GUI_PAGE_SOURCE}
          -DOUTPUT=${CPM_GUI_PAGE_GENERATED}
          -DFUNCTION=page
          -DHEADER=cpm/gui/page.hpp
          -P "${CMAKE_SOURCE_DIR}/cmake/EmbedFile.cmake"
  DEPENDS "${CPM_GUI_PAGE_SOURCE}" "${CMAKE_SOURCE_DIR}/cmake/EmbedFile.cmake"
  COMMENT "Embedding gui/web/index.html"
  VERBATIM
)

add_library(cpm_gui
  src/http.cpp
  src/server.cpp
  src/views.cpp
  "${CPM_GUI_PAGE_GENERATED}"
)
target_include_directories(cpm_gui PUBLIC include)
# OpenSSL for RAND_bytes only: the run token. The GUI has no TLS of its own,
# because the SSH tunnel it is reached through already has it (GUI-Q3).
target_link_libraries(cpm_gui PUBLIC cpm_trader_lib PRIVATE OpenSSL::Crypto)
cpm_set_warnings(cpm_gui)

if(CPM_BUILD_TESTS)
  add_executable(cpm_gui_tests
    tests/http_test.cpp
    tests/server_test.cpp
    tests/page_test.cpp
    tests/live_test.cpp
  )
  # The simulators, so the GUI can be driven against real FIX sessions rather
  # than only against fakes.
  target_link_libraries(cpm_gui_tests PRIVATE cpm_gui cpm_crypto_mm cpm_md_sim cpm_oe_sim
                                              GTest::gtest_main)
  target_compile_definitions(cpm_gui_tests PRIVATE CPM_DICT_DIR="${CPM_DICT_DIR}")
  cpm_set_warnings(cpm_gui_tests)
  gtest_discover_tests(cpm_gui_tests PROPERTIES TIMEOUT 120)
endif()
