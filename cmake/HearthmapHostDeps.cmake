# The host renderer's two third-party dependencies, pinned by content hash. Neither may be
# linked by core or ui_lvgl (AGENTS.md §4).
#
# - nlohmann/json v3.12.0 (MIT, LICENSE.MIT in the archive): header-only JSON parser, from the
#   release's own source tarball. Host code may use exceptions (D-002); this library throws.
# - lodepng at commit 7b2893e (zlib licence, LICENSE in the archive): PNG encode and decode in
#   one .cpp/.h pair, compiled here without our warnings.
#
# To update either: change URL and SHA-256 together and say why in the PR.
include(FetchContent)

# DOWNLOAD_EXTRACT_TIMESTAMP exists from CMake 3.24; on 3.21..3.23 (our floor) it would be
# parsed as part of URL_HASH and break the download.
set(_hearthmap_extract_ts)
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.24)
  set(_hearthmap_extract_ts DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
endif()

FetchContent_Declare(nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
  URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
  ${_hearthmap_extract_ts}
  SOURCE_SUBDIR do-not-add-subdirectory)
FetchContent_Declare(lodepng
  URL https://github.com/lvandeve/lodepng/archive/7b2893e35891c0c85bee5595caafaf3c2044b409.tar.gz
  URL_HASH SHA256=e0b571062acdbe187fd4e8b0ff46f6e7d1b634457ff1a4b383aaac4c78bfd59b
  ${_hearthmap_extract_ts}
  SOURCE_SUBDIR do-not-add-subdirectory)
FetchContent_MakeAvailable(nlohmann_json lodepng)

if(NOT TARGET hearthmap_json)
  add_library(hearthmap_json INTERFACE)
  target_include_directories(hearthmap_json SYSTEM INTERFACE "${nlohmann_json_SOURCE_DIR}/single_include")
endif()

if(NOT TARGET hearthmap_lodepng)
  add_library(hearthmap_lodepng STATIC "${lodepng_SOURCE_DIR}/lodepng.cpp")
  target_include_directories(hearthmap_lodepng SYSTEM PUBLIC "${lodepng_SOURCE_DIR}")
  if(NOT MSVC)
    target_compile_options(hearthmap_lodepng PRIVATE -w)
  endif()
endif()
