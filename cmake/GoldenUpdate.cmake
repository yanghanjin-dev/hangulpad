# Regenerate one golden IR file: cmake -DPROGRAM=irdump -DINPUT=... -DOUTPUT=... -P GoldenUpdate.cmake
# irdump writes LF (binary stdout on Windows), so files match the Linux ones byte for byte.
execute_process(COMMAND ${EMULATOR} "${PROGRAM}" "${INPUT}" WORKING_DIRECTORY "${WORK_DIR}"
    RESULT_VARIABLE result OUTPUT_FILE "${OUTPUT}" ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "irdump failed (${result}) for ${INPUT}: ${error}")
endif()
