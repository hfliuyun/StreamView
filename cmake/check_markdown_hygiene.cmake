# cmake/check_markdown_hygiene.cmake
# Markdown hygiene check script executed by CTest.
# Validates that all git-tracked markdown files contain no literal tab characters
# and end with a trailing newline.

if(NOT DEFINED PROJECT_SOURCE_DIR)
    message(FATAL_ERROR "PROJECT_SOURCE_DIR must be defined.")
endif()

execute_process(
    COMMAND git ls-files "*.md"
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    OUTPUT_VARIABLE GIT_MD_OUTPUT
    RESULT_VARIABLE GIT_RES
    ERROR_QUIET
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

if(NOT GIT_RES EQUAL 0)
    message(STATUS "git not available or repository not a git tree; skipping markdown hygiene check.")
    return()
endif()

string(REPLACE ";" "\\[semicolon\\]" GIT_MD_OUTPUT_SAFE "${GIT_MD_OUTPUT}")
string(REGEX REPLACE "\r?\n" ";" MD_FILES_LIST "${GIT_MD_OUTPUT_SAFE}")

set(ERROR_COUNT 0)
set(TAB_CHAR "\t")

foreach(MD_REL_PATH ${MD_FILES_LIST})
    string(REPLACE "\\[semicolon\\]" ";" MD_REL_PATH "${MD_REL_PATH}")
    if(MD_REL_PATH STREQUAL "" OR MD_REL_PATH MATCHES "^third_party/")
        continue()
    endif()

    set(MD_FILE "${PROJECT_SOURCE_DIR}/${MD_REL_PATH}")
    if(NOT EXISTS "${MD_FILE}")
        continue()
    endif()

    file(READ "${MD_FILE}" FILE_CONTENT)

    # 1. Check trailing newline
    if(NOT FILE_CONTENT MATCHES "\n$")
        message(SEND_ERROR "${MD_REL_PATH}: missing trailing newline at EOF")
        math(EXPR ERROR_COUNT "${ERROR_COUNT} + 1")
    endif()

    # 2. Check literal tab characters line by line without semicolon list-splitting
    string(REPLACE ";" "\\[semicolon\\]" FILE_CONTENT_SAFE "${FILE_CONTENT}")
    string(REGEX REPLACE "\r?\n" ";" LINES "${FILE_CONTENT_SAFE}")
    set(LINE_NUM 1)
    foreach(LINE IN LISTS LINES)
        string(FIND "${LINE}" "${TAB_CHAR}" TAB_POS)
        if(NOT TAB_POS EQUAL -1)
            message(SEND_ERROR "${MD_REL_PATH}:${LINE_NUM}: contains literal tab character")
            math(EXPR ERROR_COUNT "${ERROR_COUNT} + 1")
        endif()
        math(EXPR LINE_NUM "${LINE_NUM} + 1")
    endforeach()
endforeach()

if(ERROR_COUNT GREATER 0)
    message(FATAL_ERROR "Markdown hygiene check failed with ${ERROR_COUNT} error(s).")
else()
    message(STATUS "All markdown files passed hygiene checks.")
endif()
