# Runs `hearthmap-render --check` on CONFIG and requires exit code EXPECT_RC and, when set, that
# its output contains EXPECT_TEXT. Used for the accepted example and a refused config.
execute_process(COMMAND "${RENDER}" --check --config "${CONFIG}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
message("${out}${err}")
if(NOT rc EQUAL EXPECT_RC)
  message(FATAL_ERROR "hearthmap-render --check exited ${rc}, expected ${EXPECT_RC}")
endif()
if(DEFINED EXPECT_TEXT AND NOT "${out}${err}" MATCHES "${EXPECT_TEXT}")
  message(FATAL_ERROR "output does not contain '${EXPECT_TEXT}'")
endif()
