# doctest, pinned to an exact commit (v2.4.12) and used header-only.
#
# Only the single header is consumed: doctest's own CMakeLists is never added to the build
# (SOURCE_SUBDIR points at a directory that does not exist), so its cmake_minimum_required and
# options cannot leak into ours. Licence: MIT, shipped in the fetched tree as LICENSE.txt.
#
# To update: change both the tag comment and the hash, and say why in the PR.
include(FetchContent)

FetchContent_Declare(doctest
  GIT_REPOSITORY https://github.com/doctest/doctest.git
  GIT_TAG 1da23a3e8119ec5cce4f9388e91b065e20bf06f5  # v2.4.12
  GIT_SHALLOW FALSE
  SOURCE_SUBDIR do-not-add-subdirectory)
FetchContent_MakeAvailable(doctest)

if(NOT TARGET hearthmap_doctest)
  add_library(hearthmap_doctest INTERFACE)
  # SYSTEM: doctest's own warnings must not trip our -Werror.
  target_include_directories(hearthmap_doctest SYSTEM INTERFACE "${doctest_SOURCE_DIR}")
  target_compile_features(hearthmap_doctest INTERFACE cxx_std_17)
endif()
