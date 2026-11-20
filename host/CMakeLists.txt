# host/: the headless renderer. Depends on core, ui_lvgl, LVGL, a JSON parser and a PNG codec;
# the last two never reach core or ui_lvgl (AGENTS.md §4).

add_library(hearthmap_lvgl_headless STATIC src/headless.cpp)
target_include_directories(hearthmap_lvgl_headless PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(hearthmap_lvgl_headless PUBLIC lvgl)
target_compile_features(hearthmap_lvgl_headless PUBLIC cxx_std_17)
hearthmap_strict_warnings(hearthmap_lvgl_headless)
