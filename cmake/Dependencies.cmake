# Third-party dependencies: QuickFIX/C++ (built from a pinned release tarball,
# with SSL) and GoogleTest (system package). OpenSSL comes from the system and
# is required by QuickFIX's SSL transport and by our signing code.
include(FetchContent)

find_package(OpenSSL 3.0 REQUIRED)
find_package(Threads REQUIRED)

# --- QuickFIX ---------------------------------------------------------------
# v1.16.0 release tarball; URL download, so git is not needed.
set(HAVE_SSL ON CACHE BOOL "" FORCE)
set(QUICKFIX_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(QUICKFIX_EXAMPLES OFF CACHE BOOL "" FORCE)
set(QUICKFIX_TESTS OFF CACHE BOOL "" FORCE)
# QuickFIX defaults its output dir to ${CMAKE_SOURCE_DIR}/lib, i.e. our source tree.
set(QUICKFIX_LIB_OUTPUT_DIR "${CMAKE_BINARY_DIR}/_deps/quickfix-out" CACHE PATH "" FORCE)

FetchContent_Declare(quickfix
  URL https://github.com/quickfix/quickfix/archive/refs/tags/v1.16.0.tar.gz
  URL_HASH SHA256=6eb0e678288473e57db220d3bf53c4d5c92814629ca548af09ca7e365fd7ee70
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  # Adds TLS hostname verification, which QuickFIX lacks (see cmake/patch_quickfix.cmake).
  PATCH_COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_LIST_DIR}/patch_quickfix.cmake
)
FetchContent_MakeAvailable(quickfix)

# Headers are copied into <src>/include/quickfix by a QuickFIX build-time target.
# Expose them as SYSTEM includes so our -Wpedantic does not report QuickFIX code.
add_library(cpm_quickfix INTERFACE)
add_dependencies(cpm_quickfix QUICKFIX_HEADERS_COPY)
target_include_directories(cpm_quickfix SYSTEM INTERFACE "${quickfix_SOURCE_DIR}/include")
target_link_libraries(cpm_quickfix INTERFACE quickfix OpenSSL::SSL OpenSSL::Crypto Threads::Threads)
# QuickFIX's public headers gate the SSL classes on HAVE_SSL but never include the
# generated config.h that defines it; give consumers the value the library was built with.
target_compile_definitions(cpm_quickfix INTERFACE HAVE_SSL=1)
# The quickfix target's own INTERFACE include is non-SYSTEM; mark it SYSTEM too.
set_target_properties(quickfix PROPERTIES
  INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${quickfix_SOURCE_DIR}/include")

# --- GoogleTest ---------------------------------------------------------------
if(CPM_BUILD_TESTS)
  find_package(GTest REQUIRED)
  include(GoogleTest)
endif()
