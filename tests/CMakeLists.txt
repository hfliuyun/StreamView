qt_add_executable(streamview_core_tests core/version_test.cpp)
target_link_libraries(streamview_core_tests PRIVATE StreamView::Core StreamView::Rules Qt6::Test)
streamview_set_warnings(streamview_core_tests)

add_test(NAME streamview_core_tests COMMAND streamview_core_tests)

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
