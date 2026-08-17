add_library(streamview_rules STATIC
    aac_adts_analyzer.cpp
    aac_adts_detector.cpp
    aac_adts_scanner.cpp
    analysis_cache.cpp
    analysis_cache_owner.cpp
    analysis_cache_payload.cpp
    dsl.cpp
    dsl_ir.cpp
    dsl_executor.cpp
    dsl_vm.cpp
    h264_annex_b_analyzer.cpp
    h264_annex_b_detector.cpp
    h264_ebsp_rbsp_mapper.cpp
    h264_start_code_prefix.h
    h264_start_code_scanner.cpp
    mp4_box_detector.cpp
    mp4_box_scanner.cpp
    mp4_isobmff_analyzer.cpp
    window_decoder.cpp
    rule_catalog.cpp
    rule_package_archive.cpp
    rule_package_directory.cpp
    rule_package_store.cpp
    rule_package.cpp
    rule_execution_session.cpp
    include/streamview/rules/aac_adts_analyzer.h
    include/streamview/rules/aac_adts_detector.h
    include/streamview/rules/aac_adts_scanner.h
    include/streamview/rules/analysis_cache.h
    include/streamview/rules/analysis_cache_owner.h
    include/streamview/rules/analysis_cache_payload.h
    include/streamview/rules/dsl.h
    include/streamview/rules/dsl_ir.h
    include/streamview/rules/dsl_executor.h
    include/streamview/rules/dsl_vm.h
    include/streamview/rules/h264_annex_b_analyzer.h
    include/streamview/rules/h264_annex_b_detector.h
    include/streamview/rules/h264_ebsp_rbsp_mapper.h
    include/streamview/rules/h264_start_code_scanner.h
    include/streamview/rules/language_version.h
    include/streamview/rules/mp4_box_detector.h
    include/streamview/rules/mp4_box_scanner.h
    include/streamview/rules/mp4_isobmff_analyzer.h
    include/streamview/rules/window_decoder.h
    include/streamview/rules/rule_catalog.h
    include/streamview/rules/rule_package.h
    include/streamview/rules/rule_package_store.h
    include/streamview/rules/rule_execution_session.h
    language_version.cpp
)

add_library(StreamView::Rules ALIAS streamview_rules)

target_include_directories(
    streamview_rules
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_definitions(
    streamview_rules
    PRIVATE
        TOML_HEADER_ONLY=1
        TOML_ENABLE_PARSER=1
        TOML_ENABLE_FORMATTERS=0
        TOML_EXCEPTIONS=1
)

target_include_directories(
    streamview_rules
    SYSTEM PRIVATE
        "${PROJECT_SOURCE_DIR}/third_party/tomlplusplus/include"
)

target_link_libraries(streamview_rules PUBLIC StreamView::Core Qt6::Core)
streamview_set_warnings(streamview_rules)

qt_add_resources(
    streamview_rules
    streamview_official_rules
    PREFIX "/streamview/rules/org.streamview.h264"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/official/org.streamview.h264"
    FILES
        official/org.streamview.h264/rule.toml
        official/org.streamview.h264/src/h264_annex_b.svfmt
)

qt_add_resources(
    streamview_rules
    streamview_official_rules_aac
    PREFIX "/streamview/rules/org.streamview.aac"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/official/org.streamview.aac"
    FILES
        official/org.streamview.aac/rule.toml
        official/org.streamview.aac/src/aac_adts.svfmt
        official/org.streamview.aac/src/aac_asc.svfmt
)


