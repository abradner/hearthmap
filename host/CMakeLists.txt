# host/: the headless renderer. Depends on core, ui_lvgl, LVGL, a JSON parser and a PNG codec;
# the last two never reach core or ui_lvgl (AGENTS.md §4).

include(HearthmapHostDeps)

# LVGL with no window (also used by ui_lvgl's widget tests).
add_library(hearthmap_lvgl_headless STATIC src/headless.cpp)
target_include_directories(hearthmap_lvgl_headless PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(hearthmap_lvgl_headless PUBLIC lvgl)
target_compile_features(hearthmap_lvgl_headless PUBLIC cxx_std_17)
hearthmap_strict_warnings(hearthmap_lvgl_headless)

# PNG in and out.
add_library(hearthmap_png_io STATIC src/png_io.cpp)
target_include_directories(hearthmap_png_io PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(hearthmap_png_io PRIVATE hearthmap_lodepng)
target_compile_features(hearthmap_png_io PUBLIC cxx_std_17)
hearthmap_strict_warnings(hearthmap_png_io)

# Everything the CLI does except argument parsing, so tests can link it.
add_library(hearthmap_host STATIC
  src/app.cpp
  src/json_config.cpp
  src/photos.cpp
  src/summary.cpp
)
target_include_directories(hearthmap_host PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(hearthmap_host
  PUBLIC hearthmap::core hearthmap::ui_lvgl hearthmap_lvgl_headless
  PRIVATE hearthmap_json hearthmap_png_io)
hearthmap_strict_warnings(hearthmap_host)

add_executable(hearthmap-render src/render_main.cpp)
target_link_libraries(hearthmap-render PRIVATE hearthmap_host hearthmap_png_io)
hearthmap_strict_warnings(hearthmap-render)

add_executable(hearthmap-imgdiff src/imgdiff_main.cpp)
target_link_libraries(hearthmap-imgdiff PRIVATE hearthmap_png_io)
hearthmap_strict_warnings(hearthmap-imgdiff)

if(HEARTHMAP_BUILD_TESTS)
  add_subdirectory(tests)
endif()
