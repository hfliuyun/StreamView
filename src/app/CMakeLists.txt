add_library(streamview_app STATIC
    analysis_session.cpp
    analysis_session.h
    analysis_tree_model.cpp
    analysis_tree_model.h
    field_inspector.cpp
    field_inspector.h
    main_window.cpp
    main_window.h
    raw_data_model.cpp
    raw_data_model.h
    raw_data_view.cpp
    raw_data_view.h
    source_selection.h
)

add_library(StreamView::App ALIAS streamview_app)

target_link_libraries(
    streamview_app
    PUBLIC
        StreamView::Core
        StreamView::Rules
        Qt6::Gui
        Qt6::Widgets
)

target_include_directories(streamview_app PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
streamview_set_warnings(streamview_app)

qt_add_executable(streamview main.cpp)
target_link_libraries(streamview PRIVATE StreamView::App)

set_target_properties(
    streamview
    PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "StreamView"
        MACOSX_BUNDLE_GUI_IDENTIFIER "org.streamview.app"
        WIN32_EXECUTABLE TRUE
)

streamview_set_warnings(streamview)

include(GNUInstallDirs)
install(
    TARGETS streamview
    BUNDLE DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

qt_generate_deploy_app_script(
    TARGET streamview
    OUTPUT_SCRIPT streamview_deploy_script
    NO_UNSUPPORTED_PLATFORM_ERROR
)
install(SCRIPT ${streamview_deploy_script})
