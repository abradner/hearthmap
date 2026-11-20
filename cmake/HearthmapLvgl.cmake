# LVGL 9.6.0 for the host build (the version ESPHome 2026.11.0 pins), fetched as a source archive
# at an exact commit and compiled here from its sources with host/lvgl/lv_conf.h.
#
# LVGL's own CMakeLists is not used: it resolves optional dependencies (SDL, ThorVG, ...) through
# find_package / pkg-config / FetchContent, and we want none of them. We glob src/*.c exactly as
# its env_support/cmake/main.cmake does, minus the C++ ThorVG sources (LV_USE_THORVG is off).
#
# The device build never uses this file: ESPHome supplies LVGL there, and ui_lvgl links whatever
# target named `lvgl` the embedder provides.
#
# Licence: MIT (LICENCE.txt in the archive). The built-in Montserrat fonts are SIL OFL 1.1 and
# carry Font Awesome Free glyphs (ui_lvgl/README.md, "Fonts").
#
# To update: change the commit, the tag comment and the SHA-256 together, and say why in the PR.
include(FetchContent)

# DOWNLOAD_EXTRACT_TIMESTAMP exists from CMake 3.24; on 3.21..3.23 (our floor) it would be
# parsed as part of URL_HASH and break the download.
set(_hearthmap_extract_ts)
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.24)
  set(_hearthmap_extract_ts DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
endif()

FetchContent_Declare(lvgl
  URL https://github.com/lvgl/lvgl/archive/80ca777e37a2b176770726a02e07a6fb79ef0b39.tar.gz  # v9.6.0
  URL_HASH SHA256=1c10f77b8e5e7776366d56de5645958f3312b3926b71a2a811d819fc0ad0cbaa
  ${_hearthmap_extract_ts}
  SOURCE_SUBDIR do-not-add-subdirectory)
FetchContent_MakeAvailable(lvgl)

if(NOT TARGET lvgl)
  file(GLOB_RECURSE _lvgl_sources CONFIGURE_DEPENDS "${lvgl_SOURCE_DIR}/src/*.c")
  add_library(lvgl STATIC ${_lvgl_sources})
  # SYSTEM: LVGL's own warnings must not trip our -Werror in targets that include its headers.
  # The same include set LVGL's main.cmake gives its own target (9.6 moved public headers under
  # include/lvgl/; the root lvgl.h forwards there).
  target_include_directories(lvgl SYSTEM PUBLIC
    "${lvgl_SOURCE_DIR}"
    "${lvgl_SOURCE_DIR}/include"
    "${lvgl_SOURCE_DIR}/include/lvgl"
    "${CMAKE_SOURCE_DIR}/host/lvgl")
  target_compile_definitions(lvgl PUBLIC
    LV_CONF_INCLUDE_SIMPLE LV_LVGL_H_INCLUDE_SIMPLE LV_KCONFIG_IGNORE)
  set_target_properties(lvgl PROPERTIES C_STANDARD 99 C_EXTENSIONS ON)
  if(NOT MSVC)
    target_compile_options(lvgl PRIVATE -w)
  endif()
endif()
