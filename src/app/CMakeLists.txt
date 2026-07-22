qt_add_executable(streamview
    analysis_tree_model.cpp
    analysis_tree_model.h
    main.cpp
    main_window.cpp
    main_window.h
)

target_link_libraries(
    streamview
    PRIVATE
        StreamView::Core
        StreamView::Rules
        Qt6::Gui
        Qt6::Widgets
)

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
