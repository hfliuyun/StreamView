add_library(streamview_rules STATIC
    dsl.cpp
    dsl_ir.cpp
    dsl_executor.cpp
    dsl_vm.cpp
    h264_annex_b_analyzer.cpp
    h264_annex_b_detector.cpp
    h264_start_code_prefix.h
    h264_start_code_scanner.cpp
    include/streamview/rules/dsl.h
    include/streamview/rules/dsl_ir.h
    include/streamview/rules/dsl_executor.h
    include/streamview/rules/dsl_vm.h
    include/streamview/rules/h264_annex_b_analyzer.h
    include/streamview/rules/h264_annex_b_detector.h
    include/streamview/rules/h264_start_code_scanner.h
    include/streamview/rules/language_version.h
    language_version.cpp
)

add_library(StreamView::Rules ALIAS streamview_rules)

target_include_directories(
    streamview_rules
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(streamview_rules PUBLIC StreamView::Core Qt6::Core)
streamview_set_warnings(streamview_rules)

qt_add_resources(
    streamview_rules
    streamview_official_rules
    PREFIX "/streamview/rules"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/official"
    FILES
        official/h264_annex_b.svfmt
)
