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
# Off by default so a core-only build stays quick and offline after the first configure; CI's
# ui-host job turns it on. Fetches LVGL 9.6.0, lodepng and nlohmann/json at pinned versions.
option(HEARTHMAP_BUILD_UI "Build ui_lvgl, the host renderer and the golden-image tests" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(HearthmapWarnings)

# Golden images compare pixels exactly, across Apple clang on arm64 and GCC/Clang on x86-64.
# Fused multiply-add changes floating-point rounding, so contraction is off for every target
# (core, ui_lvgl, host, LVGL) on every compiler that would otherwise fuse.
if(NOT MSVC)
  add_compile_options(-ffp-contract=off)
endif()

# Layering (AGENTS.md §4): core depends on nothing; ui_lvgl on core + LVGL; host and the
# ESPHome component on both.
add_subdirectory(core)

if(HEARTHMAP_BUILD_TESTS)
  enable_testing()
  include(HearthmapDoctest)
  add_subdirectory(core/tests)
endif()

if(HEARTHMAP_BUILD_UI)
  enable_language(C)
  include(HearthmapLvgl)
  add_subdirectory(ui_lvgl)
  add_subdirectory(host)
endif()
