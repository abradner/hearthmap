# Core's unit tests: one doctest executable, one ctest entry. doctest reports per test case;
# ctest gates on the exit code.

add_executable(hearthmap_core_tests
  main.cpp
  inline_helpers_test.cpp
  calibration_test.cpp
  config_validate_test.cpp
  severity_test.cpp
  support/flat_fixture.cpp
)
target_include_directories(hearthmap_core_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/support")

# TEMPORARY: point_in_polygon() is implemented by PR 3 in core/src/geometry.cpp. Until that
# file exists, link a test-only stand-in that follows geometry.h's contract. Remove this block
# and support/point_in_polygon_standin.cpp once PR 3 has landed.
if(NOT EXISTS "${PROJECT_SOURCE_DIR}/core/src/geometry.cpp")
  message(STATUS "hearthmap: core/src/geometry.cpp absent; tests use the point_in_polygon stand-in")
  target_sources(hearthmap_core_tests PRIVATE support/point_in_polygon_standin.cpp)
endif()

target_link_libraries(hearthmap_core_tests PRIVATE hearthmap::core hearthmap_doctest)
hearthmap_strict_warnings(hearthmap_core_tests)

add_test(NAME core.unit COMMAND hearthmap_core_tests --no-version)
# doctest exits 0 when no test case ran (an empty file list, a filter that matches nothing).
# "Nothing ran" is not a pass (AGENTS.md §7), so a zero-case summary fails the ctest entry
# in addition to the exit-code check.
set_tests_properties(core.unit PROPERTIES
  FAIL_REGULAR_EXPRESSION "test cases: +0 +\\|")
