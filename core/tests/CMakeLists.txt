# Core's unit tests: one doctest executable, one ctest entry. doctest reports per test case;
# ctest gates on the exit code.

add_executable(hearthmap_core_tests
  main.cpp
  inline_helpers_test.cpp
  calibration_test.cpp
  colour_scale_test.cpp
  config_validate_test.cpp
  config_validator_test.cpp
  geometry_test.cpp
  heat_field_test.cpp
  pipeline_test.cpp
  room_scanner_test.cpp
  severity_test.cpp
  view_model_test.cpp
  support/flat_fixture.cpp
)
target_include_directories(hearthmap_core_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/support")

target_link_libraries(hearthmap_core_tests PRIVATE hearthmap::core hearthmap_doctest)
hearthmap_strict_warnings(hearthmap_core_tests)

add_test(NAME core.unit COMMAND hearthmap_core_tests --no-version)
# doctest exits 0 when no test case ran (an empty file list, a filter that matches nothing).
# "Nothing ran" is not a pass (AGENTS.md §7), so a zero-case summary fails the ctest entry
# in addition to the exit-code check.
set_tests_properties(core.unit PROPERTIES
  FAIL_REGULAR_EXPRESSION "test cases: +0 +\\|")

# No allocation once warmed up (D-022). Its own executable: it replaces global operator new.
add_executable(hearthmap_core_alloc_tests main.cpp alloc_test.cpp support/flat_fixture.cpp)
target_include_directories(hearthmap_core_alloc_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/support")
target_link_libraries(hearthmap_core_alloc_tests PRIVATE hearthmap::core hearthmap_doctest)
hearthmap_strict_warnings(hearthmap_core_alloc_tests)
add_test(NAME core.no_alloc COMMAND hearthmap_core_alloc_tests --no-version)
set_tests_properties(core.no_alloc PROPERTIES FAIL_REGULAR_EXPRESSION "test cases: +0 +\\|")

# Defaults agreement (docs/decisions.md D-007): default_band() and Config's member defaults,
# as compiled, must equal the numbers schema/README.md documents and the "default" keywords in
# schema/hearthmap.schema.json. dump_defaults prints what core holds; check_defaults.py
# compares. The self-test plants a mismatch in each source and requires the check to fail.
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
add_executable(hearthmap_dump_defaults dump_defaults.cpp)
target_link_libraries(hearthmap_dump_defaults PRIVATE hearthmap::core)
hearthmap_strict_warnings(hearthmap_dump_defaults)

add_test(NAME core.defaults_match_docs
  COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/check_defaults.py"
          --dump "$<TARGET_FILE:hearthmap_dump_defaults>"
          --readme "${PROJECT_SOURCE_DIR}/schema/README.md"
          --schema "${PROJECT_SOURCE_DIR}/schema/hearthmap.schema.json")
add_test(NAME core.defaults_check_self_test
  COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/check_defaults.py" --self-test
          --dump "$<TARGET_FILE:hearthmap_dump_defaults>"
          --readme "${PROJECT_SOURCE_DIR}/schema/README.md"
          --schema "${PROJECT_SOURCE_DIR}/schema/hearthmap.schema.json")

# Heat-field benchmark: prints timings, asserts nothing about them (CI runners are noisy and run
# Debug builds). It fails only if a field comes out the wrong shape. Run a Release build locally
# for numbers worth quoting.
add_executable(hearthmap_heat_bench heat_field_bench.cpp)
target_link_libraries(hearthmap_heat_bench PRIVATE hearthmap::core)
hearthmap_strict_warnings(hearthmap_heat_bench)
add_test(NAME core.heat_bench COMMAND hearthmap_heat_bench)

# Host-only eyeballing helper: renders the example flat's heat field to PNG or PPM. The smoke
# test proves it runs and writes a file; looking at the picture is a human's job.
add_executable(hearthmap_heat_dump heat_dump.cpp)
target_link_libraries(hearthmap_heat_dump PRIVATE hearthmap::core)
hearthmap_strict_warnings(hearthmap_heat_dump)
add_test(NAME core.heat_dump_smoke
  COMMAND hearthmap_heat_dump "${CMAKE_CURRENT_BINARY_DIR}/heat_dump_smoke.ppm")
