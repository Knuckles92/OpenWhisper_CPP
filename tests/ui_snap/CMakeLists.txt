fv_test(NO_MAIN QT Core Gui Widgets
        DEPS fv_testkit fv_testkit_qt fv_golden fv_screenshot fv_base fv_settings fv_uimath
             fv_ui_theme fv_ui_widgets)

# The QPA native interface header (qpa/qplatformnativeinterface.h): the multi-screen tests give
# the offscreen platform more screens through its "setConfiguration" resource (two_screens.h).
if(TARGET fv_ui_snap_tests)
    find_package(Qt6 QUIET COMPONENTS GuiPrivate)
    if(TARGET Qt6::GuiPrivate)
        target_link_libraries(fv_ui_snap_tests PRIVATE Qt6::GuiPrivate)
    endif()
endif()

# The other process of test_interactive.cpp's [interactive] test: the real capture exclusion,
# screen grab, window list and foreground on Qt's windows platform, which the offscreen test
# executable cannot load. In a subfolder so fv_test does not compile it into fv_ui_snap_tests.
if(TARGET fv_ui_snap_tests AND WIN32)
    add_executable(fv_ui_snap_platform_helper platform_helper/main.cpp)
    target_link_libraries(fv_ui_snap_platform_helper PRIVATE fv_ui_snap)
    target_compile_features(fv_ui_snap_platform_helper PRIVATE cxx_std_23)
    fiorivoce_target_defaults(fv_ui_snap_platform_helper)
    fv_add_qt_runtime()
    add_dependencies(fv_ui_snap_platform_helper fv_qt_runtime)
    add_dependencies(fv_ui_snap_tests fv_ui_snap_platform_helper)
    target_compile_definitions(fv_ui_snap_tests PRIVATE
        "FV_UI_SNAP_PLATFORM_HELPER=\"$<TARGET_FILE:fv_ui_snap_platform_helper>\"")

    # The other process of test_session_interactive.cpp's [interactive] test: real snaps over
    # the desktop on Qt's windows platform.
    add_executable(fv_ui_snap_session_helper session_helper/main.cpp)
    target_link_libraries(fv_ui_snap_session_helper PRIVATE fv_ui_snap)
    target_compile_features(fv_ui_snap_session_helper PRIVATE cxx_std_23)
    fiorivoce_target_defaults(fv_ui_snap_session_helper)
    add_dependencies(fv_ui_snap_session_helper fv_qt_runtime)
    add_dependencies(fv_ui_snap_tests fv_ui_snap_session_helper)
    target_compile_definitions(fv_ui_snap_tests PRIVATE
        "FV_UI_SNAP_SESSION_HELPER=\"$<TARGET_FILE:fv_ui_snap_session_helper>\"")
endif()
