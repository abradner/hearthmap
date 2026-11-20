# Proves hearthmap-imgdiff is a gate: identical files pass, and a copy of a golden with one
# planted pixel fails with exactly one differing pixel reported.
file(MAKE_DIRECTORY "${OUTDIR}")
set(planted "${OUTDIR}/planted.png")

execute_process(COMMAND "${DIFF}" "${GOLDEN}" "${GOLDEN}" RESULT_VARIABLE same_rc)
if(NOT same_rc EQUAL 0)
  message(FATAL_ERROR "imgdiff reported a difference between a file and itself (${same_rc})")
endif()

execute_process(COMMAND "${PLANT}" "${GOLDEN}" "${planted}" 640 400 RESULT_VARIABLE plant_rc)
if(NOT plant_rc EQUAL 0)
  message(FATAL_ERROR "could not plant a pixel (${plant_rc})")
endif()
execute_process(COMMAND "${DIFF}" "${GOLDEN}" "${planted}" "${OUTDIR}/planted.diff.png"
  RESULT_VARIABLE diff_rc ERROR_VARIABLE diff_err)
if(NOT diff_rc EQUAL 1)
  message(FATAL_ERROR "imgdiff did not fail on a planted pixel (exit ${diff_rc})")
endif()
if(NOT diff_err MATCHES "1 pixel\\(s\\) differ, within x 640..640, y 400..400")
  message(FATAL_ERROR "imgdiff failed, but not for the planted pixel: ${diff_err}")
endif()
message(STATUS "planted pixel caught: ${diff_err}")
