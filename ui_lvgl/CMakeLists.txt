# hearthmap_ui_lvgl: core + LVGL only (AGENTS.md §4). No ESPHome, no SDL, no file I/O, no clock.
#
# Links a target named `lvgl` that the embedder provides: cmake/HearthmapLvgl.cmake on the host;
# on the device the ESPHome component compiles these sources against ESPHome's LVGL.
# src/fonts_builtin.cpp is the only file that names concrete fonts; a build supplying its own
# Fonts leaves it out (HEARTHMAP_UI_BUILTIN_FONTS=OFF).

option(HEARTHMAP_UI_BUILTIN_FONTS "Compile builtin_fonts() (LVGL's Montserrat)" ON)

set(HEARTHMAP_UI_SOURCES
  src/blitter.cpp
  src/present.cpp
  src/format.cpp
  src/heat_image.cpp
  src/image_util.cpp
  src/map_bake.cpp
  src/memory.cpp
  src/pulse_layer.cpp
  src/screen.cpp
  src/sprites.cpp
  src/worker.cpp
)
if(HEARTHMAP_UI_BUILTIN_FONTS)
  list(APPEND HEARTHMAP_UI_SOURCES src/fonts_builtin.cpp)
endif()

add_library(hearthmap_ui_lvgl STATIC ${HEARTHMAP_UI_SOURCES})
add_library(hearthmap::ui_lvgl ALIAS hearthmap_ui_lvgl)
target_include_directories(hearthmap_ui_lvgl PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(hearthmap_ui_lvgl PUBLIC hearthmap::core lvgl)
target_compile_features(hearthmap_ui_lvgl PUBLIC cxx_std_17)
hearthmap_strict_warnings(hearthmap_ui_lvgl)

if(HEARTHMAP_BUILD_TESTS)
  add_subdirectory(tests)
endif()
