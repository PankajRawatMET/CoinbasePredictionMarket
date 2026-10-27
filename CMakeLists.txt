cmake_minimum_required(VERSION 3.20)

project(cpm VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(CPM_BUILD_TESTS "Build unit and integration tests" ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(Warnings)
include(Dependencies)

# Data dictionaries are read at runtime; apps default to the in-tree copies.
set(CPM_DICT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/config")

if(CPM_BUILD_TESTS)
  enable_testing()
endif()

# shared/      session plumbing both FIX services need
# market_data/  the CPM market data service (CPMMD)
# order_management/ the CPM order entry service
# strategies/    strategies over both; nothing below depends on these
add_subdirectory(shared/common)
add_subdirectory(shared/fix_common)
add_subdirectory(market_data)
add_subdirectory(order_management)
# strategies/ sits on top of both services.
add_subdirectory(strategies/CryptoMM)
