#pragma once
namespace webcool
{
namespace ai
{
inline const char *ctest_registration_template()
{
	return R"WEBCOOL(# Fixed registration for webcool_io_test.cmake. Requires CMake 3.16.
include_guard(GLOBAL)
set(_WEBCOOL_IO_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/webcool_io_test.cmake")
function(webcool_add_io_test)
    cmake_parse_arguments(PARSE_ARGV 0 IO "STRIP_PROMPTS"
        "NAME;TARGET;INPUT_FILE;EXPECTED_FILE;EXPECTED_EXIT;EXPECTED_STDERR_FILE" "APP_ARGS")
    if(IO_UNPARSED_ARGUMENTS OR IO_KEYWORDS_MISSING_VALUES)
        message(FATAL_ERROR "webcool_add_io_test(${IO_NAME}): unknown arguments=[${IO_UNPARSED_ARGUMENTS}]; missing values for=[${IO_KEYWORDS_MISSING_VALUES}]. STRIP_PROMPTS is a flag: write STRIP_PROMPTS without ON/OFF. Named values such as EXPECTED_EXIT require a value; APP_ARGS must contain only application arguments.")
    endif()
    foreach(key NAME TARGET INPUT_FILE EXPECTED_FILE)
        if(NOT DEFINED IO_${key})
            message(FATAL_ERROR "webcool_add_io_test requires ${key}")
        endif()
    endforeach()
    if(NOT TARGET "${IO_TARGET}")
        message(FATAL_ERROR "webcool_add_io_test: unknown target ${IO_TARGET}")
    endif()
    if(NOT DEFINED IO_EXPECTED_EXIT)
        set(IO_EXPECTED_EXIT 0)
    endif()
    set(options "-DEXPECTED_EXIT=${IO_EXPECTED_EXIT}" "-DSTRIP_PROMPTS=${IO_STRIP_PROMPTS}")
    foreach(key INPUT_FILE EXPECTED_FILE EXPECTED_STDERR_FILE)
        if(DEFINED IO_${key})
            get_filename_component(path "${IO_${key}}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
            list(APPEND options "-D${key}=${path}")
        endif()
    endforeach()
    # Keep the entire application argv list in one -D argument.
    add_test(NAME "${IO_NAME}" COMMAND "${CMAKE_COMMAND}"
        "-DAPP=$<TARGET_FILE:${IO_TARGET}>" "-DAPP_ARGS=${IO_APP_ARGS}"
        ${options} -P "${_WEBCOOL_IO_SCRIPT}")
endfunction()
)WEBCOOL";
}
inline const char *ctest_io_template()
{
	return R"WEBCOOL(# Reusable CTest stdin/stdout check; no shell or nested CMake process.
# add_test(NAME cli_io COMMAND "${CMAKE_COMMAND}"
#   "-DAPP=$<TARGET_FILE:your_target>"
#   "-DINPUT_FILE=${CMAKE_CURRENT_SOURCE_DIR}/tests/input.txt"
#   "-DEXPECTED_FILE=${CMAKE_CURRENT_SOURCE_DIR}/tests/expected.txt"
#   -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/webcool_io_test.cmake")
# Optional: APP_ARGS (CMake list), EXPECTED_EXIT (default 0),
# EXPECTED_STDERR_FILE, STRIP_PROMPTS (default OFF; strips leading > only).
# Prefer a quiet/noninteractive CLI for automated tests. Expected output is
# compared in full, including banners and final newlines. Never drop error lines.
cmake_minimum_required(VERSION 3.16)
foreach(required APP INPUT_FILE EXPECTED_FILE)
    if(NOT DEFINED ${required} OR NOT EXISTS "${${required}}")
        message(FATAL_ERROR "Missing ${required}: [${${required}}]")
    endif()
endforeach()
if(NOT DEFINED EXPECTED_EXIT)
    set(EXPECTED_EXIT 0)
endif()
# Offsets refer to compared bytes after CRLF normalization / optional prompt
# stripping. Hex context makes spaces, prompts and final newlines unambiguous.
function(webcool_output_difference label expected actual expected_file)
    string(LENGTH "${expected}" expected_bytes)
    string(LENGTH "${actual}" actual_bytes)
    set(low 0)
    if(expected_bytes LESS actual_bytes)
        set(high ${expected_bytes})
    else()
        set(high ${actual_bytes})
    endif()
    # Find the longest equal prefix without a byte-by-byte quadratic scan.
    while(low LESS high)
        math(EXPR mid "(${low} + ${high} + 1) / 2")
        string(SUBSTRING "${expected}" 0 ${mid} expected_prefix)
        string(SUBSTRING "${actual}" 0 ${mid} actual_prefix)
        if("${expected_prefix}" STREQUAL "${actual_prefix}")
            set(low ${mid})
        else()
            math(EXPR high "${mid} - 1")
        endif()
    endwhile()
    math(EXPR context_start "${low} - 24")
    if(context_start LESS 0)
        set(context_start 0)
    endif()
    foreach(side expected actual)
        math(EXPR count "${${side}_bytes} - ${context_start}")
        if(count GREATER 64)
            set(count 64)
        endif()
        string(SUBSTRING "${${side}}" ${context_start} ${count} context)
        string(HEX "${context}" ${side}_context_hex)
    endforeach()
    message(STATUS "${label} difference: first_difference_byte=${low} expected_bytes=${expected_bytes} actual_bytes=${actual_bytes} context_start_byte=${context_start}")
    message(STATUS "input_file=[${INPUT_FILE}] expected_file=[${expected_file}] STRIP_PROMPTS=[${STRIP_PROMPTS}]")
    message(STATUS "expected_context_hex=${expected_context_hex}")
    message(STATUS "actual_context_hex=${actual_context_hex}")
endfunction()
set(verification_failed FALSE)
execute_process(COMMAND "${APP}" ${APP_ARGS}
    INPUT_FILE "${INPUT_FILE}"
    OUTPUT_VARIABLE actual ERROR_VARIABLE actual_stderr
    RESULT_VARIABLE actual_exit TIMEOUT 15)
if(NOT "${actual_exit}" STREQUAL "${EXPECTED_EXIT}")
    set(verification_failed TRUE)
    message(STATUS "Exit mismatch: expected=${EXPECTED_EXIT} actual=${actual_exit}\ninput_file=[${INPUT_FILE}]\nstdout=[${actual}]\nstderr=[${actual_stderr}]")
endif()
file(READ "${EXPECTED_FILE}" expected)
string(REPLACE "\r\n" "\n" actual "${actual}")
string(REPLACE "\r\n" "\n" expected "${expected}")
if(STRIP_PROMPTS)
    string(REGEX REPLACE "(^|\n)([ \t]*>[ \t]*)+" "\\1" actual "${actual}")
endif()
if(NOT "${actual}" STREQUAL "${expected}")
    webcool_output_difference("Stdout" "${expected}" "${actual}" "${EXPECTED_FILE}")
    set(verification_failed TRUE)
    message(STATUS "Stdout mismatch\nexpected=[${expected}]\nactual=[${actual}]\nstderr=[${actual_stderr}]")
endif()
if(DEFINED EXPECTED_STDERR_FILE)
    file(READ "${EXPECTED_STDERR_FILE}" expected_stderr)
    string(REPLACE "\r\n" "\n" actual_stderr "${actual_stderr}")
    string(REPLACE "\r\n" "\n" expected_stderr "${expected_stderr}")
    if(NOT "${actual_stderr}" STREQUAL "${expected_stderr}")
        webcool_output_difference("Stderr" "${expected_stderr}" "${actual_stderr}" "${EXPECTED_STDERR_FILE}")
        set(verification_failed TRUE)
        message(STATUS "Stderr mismatch\nexpected=[${expected_stderr}]\nactual=[${actual_stderr}]")
    endif()
endif()
if(verification_failed)
    message(FATAL_ERROR "CLI validation failed: inspect all exit/stdout/stderr mismatches above, then fix the related contract together.")
endif()
)WEBCOOL";
}
}
}
