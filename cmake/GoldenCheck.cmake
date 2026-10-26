# ENCODING NONE: compare irdump's raw UTF-8 bytes. Windows CMake otherwise decodes child
# output with the console code page (CP949 on Korean Windows) and every file mismatches.
execute_process(COMMAND ${EMULATOR} "${PROGRAM}" "${INPUT}" WORKING_DIRECTORY "${WORK_DIR}"
    RESULT_VARIABLE result OUTPUT_VARIABLE actual ERROR_VARIABLE error ENCODING NONE)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "irdump failed (${result}): ${error}")
endif()
file(READ "${WORK_DIR}/${EXPECTED}" expected)
string(REPLACE "\r\n" "\n" actual "${actual}")
string(REPLACE "\r\n" "\n" expected "${expected}")
if(NOT actual STREQUAL expected)
    message(FATAL_ERROR "Golden IR mismatch: ${INPUT}")
endif()
