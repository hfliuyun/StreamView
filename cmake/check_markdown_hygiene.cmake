# cmake/check_markdown_hygiene.cmake
# Markdown hygiene check script executed by CTest.
# Validates that all repository markdown files contain no literal tab characters
# and end with a trailing newline.

if(NOT DEFINED PROJECT_SOURCE_DIR)
    message(FATAL_ERROR "PROJECT_SOURCE_DIR must be defined.")
endif()

file(GLOB_RECURSE MD_DOCS_FILES
    LIST_DIRECTORIES false
    "${PROJECT_SOURCE_DIR}/docs/*.md"
    "${PROJECT_SOURCE_DIR}/*.md"
)

set(ERROR_COUNT 0)
set(TAB_CHAR "\t")

foreach(MD_FILE ${MD_DOCS_FILES})
    # Skip build directories or external subtrees if matched
    if(MD_FILE MATCHES "/build/" OR MD_FILE MATCHES "/\.git/")
        continue()
    endif()

    file(READ "${MD_FILE}" FILE_CONTENT)

    # 1. Check trailing newline
    if(NOT FILE_CONTENT MATCHES "\n$")
        file(RELATIVE_PATH REL_PATH "${PROJECT_SOURCE_DIR}" "${MD_FILE}")
        message(SEND_ERROR "${REL_PATH}: missing trailing newline at EOF")
        math(EXPR ERROR_COUNT "${ERROR_COUNT} + 1")
    endif()

    # 2. Check literal tab characters line by line
    string(REGEX REPLACE "\r?\n" ";" LINES "${FILE_CONTENT}")
    set(LINE_NUM 1)
    foreach(LINE IN LISTS LINES)
        string(FIND "${LINE}" "${TAB_CHAR}" TAB_POS)
        if(NOT TAB_POS EQUAL -1)
            file(RELATIVE_PATH REL_PATH "${PROJECT_SOURCE_DIR}" "${MD_FILE}")
            message(SEND_ERROR "${REL_PATH}:${LINE_NUM}: contains literal tab character")
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
