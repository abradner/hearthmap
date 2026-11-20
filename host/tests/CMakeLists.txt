# Host tests: unit tests for the JSON source, the fixture parser and the top-bar text, and the
# golden images (host/README.md, "Golden images").

add_executable(hearthmap_host_tests main.cpp json_config_test.cpp summary_test.cpp app_test.cpp)
target_link_libraries(hearthmap_host_tests PRIVATE hearthmap_host hearthmap_doctest)
target_compile_definitions(hearthmap_host_tests PRIVATE
  HEARTHMAP_EXAMPLES_DIR="${PROJECT_SOURCE_DIR}/examples")
hearthmap_strict_warnings(hearthmap_host_tests)
add_test(NAME host.unit COMMAND hearthmap_host_tests --no-version)
set_tests_properties(host.unit PROPERTIES FAIL_REGULAR_EXPRESSION "test cases: +0 +\\|")

# ---- golden images -----------------------------------------------------------------------------
# Each case: name, then the render arguments. Every case renders at --time-ms 400 so pulses are
# caught mid-cycle. Taps are canvas pixels on the example (map area 864 x 720 at 40, 76):
#   basil (plant)          (0.625,  0.06) -> 580, 119
#   living-room climate    (0.2083, 0.30) -> 220, 292
set(_flat "${PROJECT_SOURCE_DIR}/examples/flat")
set(_golden_dir "${CMAKE_CURRENT_SOURCE_DIR}/golden")
set(_out_dir "${PROJECT_BINARY_DIR}/golden-out")
set(HEARTHMAP_GOLDEN_CASES
  "all-ok|all-ok|"
  "thirsty-many|thirsty-many|--layer soil"
  "stale-sensors|stale-sensors|"
  "humid-bathroom|humid-bathroom|--layer humidity"
  "overlay-plant|thirsty-many|--layer soil --tap 580,119"
  "overlay-climate|all-ok|--tap 220,292"
  "layer-temperature|thirsty-many|--layer temperature"
  "layer-humidity|thirsty-many|--layer humidity"
  "layer-soil|all-ok|--layer soil"
  "layer-air|thirsty-many|--layer air"
)

set(_update_commands)
foreach(case IN LISTS HEARTHMAP_GOLDEN_CASES)
  string(REPLACE "|" ";" _parts "${case}")
  list(GET _parts 0 _name)
  list(GET _parts 1 _fixture)
  list(LENGTH _parts _n)
  set(_args --config "${_flat}/config.json" --readings "${_flat}/readings/${_fixture}.json"
            --time-ms 400)
  if(_n GREATER 2)
    list(GET _parts 2 _extra)
    separate_arguments(_extra_list UNIX_COMMAND "${_extra}")
    list(APPEND _args ${_extra_list})
  endif()
  # Pass the argument list through -D as a |-separated string; the script splits it back.
  string(REPLACE ";" "|" _args_bar "${_args}")
  add_test(NAME "golden.${_name}"
    COMMAND "${CMAKE_COMMAND}"
      "-DRENDER=$<TARGET_FILE:hearthmap-render>" "-DDIFF=$<TARGET_FILE:hearthmap-imgdiff>"
      "-DGOLDEN=${_golden_dir}/${_name}.png" "-DOUTDIR=${_out_dir}" "-DNAME=${_name}"
      "-DARGS_BAR=${_args_bar}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/run_golden.cmake")
  list(APPEND _update_commands
    COMMAND "${CMAKE_COMMAND}"
      "-DRENDER=$<TARGET_FILE:hearthmap-render>" "-DDIFF=$<TARGET_FILE:hearthmap-imgdiff>"
      "-DGOLDEN=${_golden_dir}/${_name}.png" "-DOUTDIR=${_out_dir}" "-DNAME=${_name}"
      "-DARGS_BAR=${_args_bar}" -DUPDATE=ON
      -P "${CMAKE_CURRENT_SOURCE_DIR}/run_golden.cmake")
endforeach()

# Regenerate every golden: `cmake --build build --target update-goldens`. Read host/README.md
# first: a golden change needs a human-readable reason in its commit message.
add_custom_target(update-goldens ${_update_commands}
  DEPENDS hearthmap-render hearthmap-imgdiff
  VERBATIM
  COMMENT "Rewriting host/tests/golden/*.png from the current renderer")

# The comparison itself must be able to fail: render a known case, plant a one-pixel change,
# and require hearthmap-imgdiff to report exactly one differing pixel.
add_executable(hearthmap_plant_pixel plant_pixel.cpp)
target_link_libraries(hearthmap_plant_pixel PRIVATE hearthmap_png_io)
hearthmap_strict_warnings(hearthmap_plant_pixel)
add_test(NAME golden.comparison_can_fail
  COMMAND "${CMAKE_COMMAND}"
    "-DPLANT=$<TARGET_FILE:hearthmap_plant_pixel>" "-DDIFF=$<TARGET_FILE:hearthmap-imgdiff>"
    "-DGOLDEN=${_golden_dir}/all-ok.png" "-DOUTDIR=${_out_dir}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/comparison_can_fail.cmake")

# `hearthmap-render --check`, the pre-flash check (components/hearthmap/README.md): the example
# passes, and a config the schema accepts but validate() refuses fails with the issue's path.
add_test(NAME host.check_accepts_example
  COMMAND "${CMAKE_COMMAND}" "-DRENDER=$<TARGET_FILE:hearthmap-render>"
    "-DCONFIG=${_flat}/config.json" -DEXPECT_RC=0 "-DEXPECT_TEXT=ok: no errors"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/run_check.cmake")
add_test(NAME host.check_refuses_collinear_room
  COMMAND "${CMAKE_COMMAND}" "-DRENDER=$<TARGET_FILE:hearthmap-render>"
    "-DCONFIG=${CMAKE_CURRENT_SOURCE_DIR}/check/collinear-room.json" -DEXPECT_RC=1
    "-DEXPECT_TEXT=error at /rooms/0/polygon"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/run_check.cmake")
