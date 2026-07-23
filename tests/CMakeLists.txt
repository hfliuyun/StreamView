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
streamview_add_rules_test(streamview_dsl_tests rules/dsl_test.cpp)
streamview_add_rules_test(streamview_dsl_executor_tests rules/dsl_executor_test.cpp)
streamview_add_rules_test(
    streamview_h264_start_code_scanner_tests
    rules/h264_start_code_scanner_test.cpp
)
streamview_add_rules_test(
    streamview_h264_annex_b_detector_tests
    rules/h264_annex_b_detector_test.cpp
)

if(TARGET streamview_app)
    streamview_add_app_test(streamview_analysis_session_tests app/analysis_session_test.cpp)
    streamview_add_app_test(streamview_analysis_tree_model_tests app/analysis_tree_model_test.cpp)
    streamview_add_app_test(streamview_field_inspector_tests app/field_inspector_test.cpp)
    streamview_add_app_test(streamview_raw_data_model_tests app/raw_data_model_test.cpp)
    streamview_add_app_test(streamview_raw_data_view_tests app/raw_data_view_test.cpp)
    streamview_add_app_test(streamview_main_window_tests app/main_window_test.cpp)
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
