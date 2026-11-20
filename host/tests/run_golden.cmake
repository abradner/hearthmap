# One golden-image check, run by ctest as `cmake -P run_golden.cmake` with:
#   RENDER  path to hearthmap-render       DIFF    path to hearthmap-imgdiff
#   GOLDEN  the committed PNG              OUTDIR  where actual and diff images go
#   NAME    the case name                  ARGS_BAR the render arguments, |-separated
#   UPDATE  if true, write the render over GOLDEN instead of comparing
#
# On a mismatch the actual image and a diff image stay in OUTDIR (CI uploads that directory).
string(REPLACE "|" ";" ARGS "${ARGS_BAR}")
file(MAKE_DIRECTORY "${OUTDIR}")
set(actual "${OUTDIR}/${NAME}.actual.png")
set(diff "${OUTDIR}/${NAME}.diff.png")
file(REMOVE "${actual}" "${diff}")

if(UPDATE)
  set(target "${GOLDEN}")
else()
  set(target "${actual}")
endif()

execute_process(COMMAND "${RENDER}" ${ARGS} --out "${target}"
  RESULT_VARIABLE render_rc)
if(NOT render_rc EQUAL 0)
  message(FATAL_ERROR "${NAME}: hearthmap-render failed (${render_rc})")
endif()
if(UPDATE)
  message(STATUS "${NAME}: wrote ${GOLDEN}")
  return()
endif()

if(NOT EXISTS "${GOLDEN}")
  message(FATAL_ERROR "${NAME}: no golden at ${GOLDEN}; actual render kept at ${actual}")
endif()
execute_process(COMMAND "${DIFF}" "${GOLDEN}" "${actual}" "${diff}" RESULT_VARIABLE diff_rc)
if(diff_rc EQUAL 0)
  file(REMOVE "${actual}")
  message(STATUS "${NAME}: identical to ${GOLDEN}")
else()
  message(FATAL_ERROR "${NAME}: render differs from ${GOLDEN} (actual: ${actual}, diff: ${diff})")
endif()
