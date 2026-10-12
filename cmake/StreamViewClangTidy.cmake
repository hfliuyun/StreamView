function(streamview_configure_clang_tidy)
    if(NOT STREAMVIEW_ENABLE_CLANG_TIDY)
        return()
    endif()

    if(STREAMVIEW_CLANG_TIDY_EXECUTABLE)
        set(_clang_tidy "${STREAMVIEW_CLANG_TIDY_EXECUTABLE}")
    else()
        find_program(
            _clang_tidy
            NAMES clang-tidy-18 clang-tidy
            DOC "Clang-Tidy executable used by the static-analysis preset"
        )
    endif()

    if(NOT _clang_tidy)
        message(FATAL_ERROR
            "STREAMVIEW_ENABLE_CLANG_TIDY=ON requires LLVM Clang-Tidy 18.x; "
            "set STREAMVIEW_CLANG_TIDY_EXECUTABLE to the tool path"
        )
    endif()

    execute_process(
        COMMAND "${_clang_tidy}" --version
        RESULT_VARIABLE _version_result
        OUTPUT_VARIABLE _version_output
        ERROR_VARIABLE _version_error
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_STRIP_TRAILING_WHITESPACE
    )
    if(NOT _version_result EQUAL 0)
        message(FATAL_ERROR
            "Unable to query Clang-Tidy version from '${_clang_tidy}': "
            "${_version_error}"
        )
    endif()

    string(REGEX MATCH "version ([0-9]+)\\.([0-9]+)\\.([0-9]+)" _version_match
           "${_version_output}")
    if(NOT _version_match OR NOT CMAKE_MATCH_1 STREQUAL "18")
        message(FATAL_ERROR
            "StreamView requires LLVM Clang-Tidy 18.x, but '${_clang_tidy}' "
            "reported: ${_version_output}"
        )
    endif()

    set(
        STREAMVIEW_CLANG_TIDY_COMMAND
        "${_clang_tidy};--warnings-as-errors=*"
        CACHE STRING
        "Clang-Tidy command and arguments for project targets"
        FORCE
    )
endfunction()
