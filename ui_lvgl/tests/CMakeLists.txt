# ui_lvgl's tests: hand-built view models drawn on a headless LVGL display. No core logic runs
# here, so a failure is a drawing or diffing failure.

add_executable(hearthmap_ui_tests
  main.cpp
  blitter_test.cpp
  present_test.cpp
  format_test.cpp
  heat_image_test.cpp
  screen_test.cpp
  map_bake_test.cpp
  memory_test.cpp
  worker_test.cpp
)
# worker_test.cpp and the threaded sprite cases run a real second thread (worker_thread.h).
find_package(Threads REQUIRED)
target_link_libraries(hearthmap_ui_tests PRIVATE
  hearthmap::ui_lvgl hearthmap_lvgl_headless hearthmap_doctest Threads::Threads)
target_include_directories(hearthmap_ui_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
hearthmap_strict_warnings(hearthmap_ui_tests)

add_test(NAME ui.unit COMMAND hearthmap_ui_tests --no-version)
set_tests_properties(ui.unit PROPERTIES FAIL_REGULAR_EXPRESSION "test cases: +0 +\\|")
