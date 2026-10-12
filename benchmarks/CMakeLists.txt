qt_add_executable(streamview_benchmarks main.cpp)
target_link_libraries(streamview_benchmarks PRIVATE StreamView::App)
target_compile_definitions(streamview_benchmarks PRIVATE
    STREAMVIEW_BENCHMARK_BUILD_TYPE="${CMAKE_BUILD_TYPE}")
set_target_properties(streamview_benchmarks PROPERTIES WIN32_EXECUTABLE FALSE)
streamview_set_warnings(streamview_benchmarks)
if(WIN32)
    target_link_libraries(streamview_benchmarks PRIVATE psapi)
endif()
