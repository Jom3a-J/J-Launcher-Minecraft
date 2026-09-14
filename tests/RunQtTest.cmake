# SPDX-License-Identifier: GPL-3.0-only

if(NOT DEFINED TEST_EXECUTABLE OR NOT DEFINED TEST_LOG)
    message(FATAL_ERROR "TEST_EXECUTABLE and TEST_LOG are required.")
endif()

file(REMOVE "${TEST_LOG}")
execute_process(
    COMMAND "${TEST_EXECUTABLE}" -o "${TEST_LOG},txt"
    RESULT_VARIABLE test_result
    OUTPUT_VARIABLE test_stdout
    ERROR_VARIABLE test_stderr
    TIMEOUT 290
)
if(EXISTS "${TEST_LOG}")
    file(READ "${TEST_LOG}" test_output)
    message("${test_output}")
endif()
if(NOT test_stdout STREQUAL "")
    message("${test_stdout}")
endif()
if(NOT test_stderr STREQUAL "")
    message("${test_stderr}")
endif()
if(NOT "${test_result}" STREQUAL "0")
    message(FATAL_ERROR "QtTest failed: ${test_result}")
endif()
