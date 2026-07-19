add_library(streamview_rules STATIC
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
