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

streamview_add_core_test(streamview_coordinates_tests core/coordinates_test.cpp)
streamview_add_core_test(streamview_source_tests core/source_test.cpp)
streamview_add_core_test(streamview_bit_reader_tests core/bit_reader_test.cpp)
streamview_add_core_test(streamview_analysis_model_tests core/analysis_model_test.cpp)
streamview_add_core_test(streamview_cancellation_tests core/cancellation_test.cpp)

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
