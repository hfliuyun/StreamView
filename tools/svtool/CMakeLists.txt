qt_add_executable(svtool main.cpp)

target_link_libraries(svtool PRIVATE StreamView::Core StreamView::Rules Qt6::Core)
streamview_set_warnings(svtool)

include(GNUInstallDirs)
install(TARGETS svtool RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
