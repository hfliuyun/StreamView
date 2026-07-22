add_library(streamview_core STATIC
    analysis_model.cpp
    bit_reader.cpp
    coordinates.cpp
    source.cpp
    source_pager.cpp
    include/streamview/core/analysis_model.h
    include/streamview/core/bit_reader.h
    include/streamview/core/coordinates.h
    include/streamview/core/source.h
    include/streamview/core/source_pager.h
    include/streamview/core/version.h
    version.cpp
)

add_library(StreamView::Core ALIAS streamview_core)

target_include_directories(
    streamview_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(streamview_core PUBLIC Qt6::Core)
target_compile_definitions(streamview_core PRIVATE STREAMVIEW_VERSION="${PROJECT_VERSION}")
streamview_set_warnings(streamview_core)
