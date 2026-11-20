cmake_minimum_required(VERSION 3.21)

project(hearthmap
  VERSION 0.0.0
  DESCRIPTION "House heatmap wall panel: core library, LVGL UI, host renderer"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(HEARTHMAP_BUILD_TESTS "Build the doctest suites and register them with ctest" ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(HearthmapWarnings)

# Layering (AGENTS.md §4): core depends on nothing; ui_lvgl on core + LVGL; host and the
# ESPHome component on both. Only core exists in PR 1; ui_lvgl/ and host/ join the build in PR 5.
add_subdirectory(core)

if(HEARTHMAP_BUILD_TESTS)
  enable_testing()
  include(HearthmapDoctest)
  add_subdirectory(core/tests)
endif()
