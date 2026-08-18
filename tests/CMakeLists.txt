qt_add_executable(streamview_core_tests core/version_test.cpp)
target_link_libraries(streamview_core_tests PRIVATE StreamView::Core StreamView::Rules Qt6::Test)
streamview_set_warnings(streamview_core_tests)

add_test(NAME streamview_core_tests COMMAND streamview_core_tests)

function(streamview_add_core_test target source)
    qt_add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE StreamView::Core Qt6::Test)
    streamview_set_warnings(${target})
    add_test(NAME ${target} COMMAND ${target})
endfunction()

function(streamview_add_rules_test target source)
    qt_add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE StreamView::Rules Qt6::Test)
    streamview_set_warnings(${target})
    add_test(NAME ${target} COMMAND ${target})
endfunction()

function(streamview_add_app_test target source)
    qt_add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE StreamView::App Qt6::Test)
    streamview_set_warnings(${target})
    add_test(NAME ${target} COMMAND ${target})
    set_tests_properties(${target} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endfunction()

streamview_add_core_test(streamview_coordinates_tests core/coordinates_test.cpp)
streamview_add_core_test(streamview_source_tests core/source_test.cpp)
streamview_add_core_test(streamview_source_pager_tests core/source_pager_test.cpp)
streamview_add_core_test(streamview_bit_reader_tests core/bit_reader_test.cpp)
streamview_add_core_test(streamview_analysis_model_tests core/analysis_model_test.cpp)
streamview_add_core_test(streamview_cancellation_tests core/cancellation_test.cpp)
streamview_add_core_test(streamview_context_directory_tests core/context_directory_test.cpp)
streamview_add_core_test(streamview_paged_cache_tests core/paged_cache_test.cpp)
target_link_libraries(streamview_paged_cache_tests PRIVATE Qt6::Sql)
streamview_add_rules_test(streamview_dsl_tests rules/dsl_test.cpp)
streamview_add_rules_test(streamview_dsl_ir_tests rules/dsl_ir_test.cpp)
streamview_add_rules_test(streamview_dsl_executor_tests rules/dsl_executor_test.cpp)
streamview_add_rules_test(
    streamview_rule_execution_session_tests
    rules/rule_execution_session_test.cpp
)
streamview_add_rules_test(streamview_rule_package_tests rules/rule_package_test.cpp)
streamview_add_rules_test(streamview_analysis_cache_tests rules/analysis_cache_test.cpp)
streamview_add_rules_test(
    streamview_analysis_cache_owner_tests
    rules/analysis_cache_owner_test.cpp
)
target_link_libraries(streamview_analysis_cache_owner_tests PRIVATE Qt6::Sql)
streamview_add_rules_test(
    streamview_analysis_cache_payload_tests
    rules/analysis_cache_payload_test.cpp
)
streamview_add_rules_test(
    streamview_rule_package_store_tests
    rules/rule_package_store_test.cpp
)
if(WIN32)
    set_tests_properties(streamview_rule_package_store_tests PROPERTIES DISABLED TRUE)
    foreach(
        test_case
        IN ITEMS
            importsDirectoryAndRoundTripsCanonicalArchive
            rejectsNoncanonicalAndMalformedArchives
            rejectsUnicodeDirectoryPaths
            rejectsOversizedEmptyDirectoryPaths
            rejectsSymbolicLinksAndExecutableFiles
            installsIdempotentlyAndRequiresReadOnlyContent
            reportsCorruptExistingContentWithoutRepairingIt
    )
        add_test(
            NAME streamview_rule_package_store_tests.${test_case}
            COMMAND streamview_rule_package_store_tests ${test_case} -o -,txt
        )
    endforeach()
endif()
streamview_add_rules_test(
    streamview_h264_start_code_scanner_tests
    rules/h264_start_code_scanner_test.cpp
)
streamview_add_rules_test(
    streamview_aac_adts_scanner_tests
    rules/aac_adts_scanner_test.cpp
)
streamview_add_rules_test(
    streamview_aac_adts_detector_tests
    rules/aac_adts_detector_test.cpp
)
streamview_add_rules_test(
    streamview_aac_adts_analyzer_tests
    rules/aac_adts_analyzer_test.cpp
)
streamview_add_rules_test(
    streamview_h264_annex_b_detector_tests
    rules/h264_annex_b_detector_test.cpp
)
streamview_add_rules_test(
    streamview_h264_ebsp_rbsp_mapper_tests
    rules/h264_ebsp_rbsp_mapper_test.cpp
)
streamview_add_rules_test(
    streamview_mp4_box_scanner_tests
    rules/mp4_box_scanner_test.cpp
)
streamview_add_rules_test(
    streamview_mp4_box_detector_tests
    rules/mp4_box_detector_test.cpp
)
streamview_add_rules_test(
    streamview_window_decoder_tests
    rules/window_decoder_test.cpp
)
streamview_add_rules_test(
    streamview_mp4_isobmff_analyzer_tests
    rules/mp4_isobmff_analyzer_test.cpp
)
target_compile_definitions(
    streamview_mp4_isobmff_analyzer_tests
    PRIVATE
        STREAMVIEW_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)
streamview_add_rules_test(
    streamview_structural_entry_runner_tests
    rules/structural_entry_runner_test.cpp
)
target_compile_definitions(
    streamview_structural_entry_runner_tests
    PRIVATE
        STREAMVIEW_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)

if(TARGET streamview_app)
    streamview_add_app_test(streamview_analysis_session_tests app/analysis_session_test.cpp)
    target_link_libraries(streamview_analysis_session_tests PRIVATE Qt6::Sql)
    streamview_add_app_test(streamview_session_document_tests app/session_document_test.cpp)
    streamview_add_app_test(streamview_analysis_tree_model_tests app/analysis_tree_model_test.cpp)
    streamview_add_app_test(streamview_field_inspector_tests app/field_inspector_test.cpp)
    streamview_add_app_test(streamview_raw_data_model_tests app/raw_data_model_test.cpp)
    streamview_add_app_test(streamview_raw_data_view_tests app/raw_data_view_test.cpp)
    streamview_add_app_test(streamview_main_window_tests app/main_window_test.cpp)
    target_link_libraries(streamview_main_window_tests PRIVATE Qt6::Sql)
endif()
streamview_add_rules_test(
    streamview_h264_annex_b_analyzer_tests
    rules/h264_annex_b_analyzer_test.cpp
)

qt_add_executable(streamview_svtool_tests tools/svtool_test.cpp)
target_link_libraries(streamview_svtool_tests PRIVATE Qt6::Test)
target_compile_definitions(
    streamview_svtool_tests
    PRIVATE
        SVTOOL_PATH="$<TARGET_FILE:svtool>"
)
streamview_set_warnings(streamview_svtool_tests)
add_dependencies(streamview_svtool_tests svtool)
add_test(NAME streamview_svtool_tests COMMAND streamview_svtool_tests)

add_test(NAME svtool_version COMMAND svtool --version)
set_tests_properties(svtool_version PROPERTIES PASS_REGULAR_EXPRESSION "svtool 0[.]1[.]0 [(]DSL 0[.]1[)]")

if(TARGET streamview)
    add_test(NAME streamview_version COMMAND streamview --version)
    set_tests_properties(
        streamview_version
        PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
            PASS_REGULAR_EXPRESSION "StreamView 0[.]1[.]0"
    )
endif()

add_test(
    NAME markdown_hygiene
    COMMAND ${CMAKE_COMMAND} -DPROJECT_SOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/cmake/check_markdown_hygiene.cmake
)

