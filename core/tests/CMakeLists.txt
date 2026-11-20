# Core's unit tests: one doctest executable, one ctest entry. doctest reports per test case;
# ctest gates on the exit code.

add_executable(hearthmap_core_tests
  main.cpp
  inline_helpers_test.cpp
)
target_link_libraries(hearthmap_core_tests PRIVATE hearthmap::core hearthmap_doctest)
hearthmap_strict_warnings(hearthmap_core_tests)

add_test(NAME core.unit COMMAND hearthmap_core_tests --no-version)
# doctest exits 0 when no test case ran (an empty file list, a filter that matches nothing).
# "Nothing ran" is not a pass (AGENTS.md §7), so a zero-case summary fails the ctest entry
# in addition to the exit-code check.
set_tests_properties(core.unit PROPERTIES
  FAIL_REGULAR_EXPRESSION "test cases: +0 +\\|")
