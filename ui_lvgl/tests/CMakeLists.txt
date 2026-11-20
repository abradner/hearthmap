# ui_lvgl's tests: hand-built view models drawn on a headless LVGL display. No core logic runs
# here, so a failure is a drawing or diffing failure.

add_executable(hearthmap_ui_tests
  main.cpp
  format_test.cpp
  heat_image_test.cpp
  screen_test.cpp
)
target_link_libraries(hearthmap_ui_tests PRIVATE
  hearthmap::ui_lvgl hearthmap_lvgl_headless hearthmap_doctest)
hearthmap_strict_warnings(hearthmap_ui_tests)

add_test(NAME ui.unit COMMAND hearthmap_ui_tests --no-version)
set_tests_properties(ui.unit PROPERTIES FAIL_REGULAR_EXPRESSION "test cases: +0 +\\|")
