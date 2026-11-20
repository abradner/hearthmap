# hearthmap_core: pure C++17, no dependencies (AGENTS.md §4). Nothing here may include LVGL,
# ESPHome, a clock, a file or a socket.

set(HEARTHMAP_CORE_HEADERS
  calibration.h
  colour_scale.h
  config.h
  geometry.h
  heat_field.h
  readings.h
  severity.h
  view_model.h
)

# Implementations land in PR 2 (config validation, calibration, severity), PR 3 (geometry,
# heat field, colour scale) and PR 4 (view model). Add each new .cpp here.
set(HEARTHMAP_CORE_SOURCES
  src/colour_scale.cpp
  src/geometry.cpp
  src/heat_field.cpp
)

# Header self-containment: one generated translation unit per public header, each including
# only that header, compiled with core's strict warnings. A header that forgets an #include,
# or trips a warning, fails the build.
set(_selfcontained_sources)
foreach(header IN LISTS HEARTHMAP_CORE_HEADERS)
  string(REPLACE ".h" "" _stem "${header}")
  set(_tu "${CMAKE_CURRENT_BINARY_DIR}/selfcontained/${_stem}.cpp")
  file(CONFIGURE OUTPUT "${_tu}"
    CONTENT "// Generated: proves hearthmap/${header} compiles on its own.\n#include <hearthmap/${header}>\n")
  list(APPEND _selfcontained_sources "${_tu}")
endforeach()

add_library(hearthmap_core STATIC ${HEARTHMAP_CORE_SOURCES} ${_selfcontained_sources})
add_library(hearthmap::core ALIAS hearthmap_core)
target_include_directories(hearthmap_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(hearthmap_core PUBLIC cxx_std_17)
hearthmap_strict_warnings(hearthmap_core)

# Core builds without exceptions or RTTI, as ESP-IDF firmware commonly does (docs/decisions.md
# D-002). PRIVATE: it binds core's own translation units, including the header self-containment
# units above, so a header that needs either fails here. The test executable keeps both, because
# doctest uses exceptions; tests only include the headers.
if(MSVC)
  target_compile_options(hearthmap_core PRIVATE /GR- /EHs-c-)
  target_compile_definitions(hearthmap_core PRIVATE _HAS_EXCEPTIONS=0)
else()
  target_compile_options(hearthmap_core PRIVATE -fno-exceptions -fno-rtti)
endif()

# Determinism (docs/decisions.md D-024): heat fields feed golden images, so core's arithmetic must
# not depend on whether the compiler chose to fuse a*b+c. Clang contracts by default; GCC does in
# GNU modes. Fast-math is never enabled for core.
if(NOT MSVC)
  target_compile_options(hearthmap_core PRIVATE -ffp-contract=off)
endif()
