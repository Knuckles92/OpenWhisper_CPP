fv_test(NO_MAIN QT Core Gui Widgets
        DEPS fv_testkit fv_testkit_qt fv_screenshot fv_golden fv_base fv_settings fv_appkit fv_uimath
             fv_ui_theme fv_ui_widgets fv_speech fv_recorder fv_audio)

if(TARGET fv_ui_overlay_tests AND WIN32)
    add_executable(fv_ui_overlay_noactivate_helper noactivate_helper/main.cpp)
    target_link_libraries(fv_ui_overlay_noactivate_helper PRIVATE fv_ui_overlay user32)
    target_compile_features(fv_ui_overlay_noactivate_helper PRIVATE cxx_std_23)
    openwhisper_cpp_target_defaults(fv_ui_overlay_noactivate_helper)
    fv_add_qt_runtime()
    add_dependencies(fv_ui_overlay_noactivate_helper fv_qt_runtime)
    add_dependencies(fv_ui_overlay_tests fv_ui_overlay_noactivate_helper)
    target_compile_definitions(fv_ui_overlay_tests PRIVATE
        "FV_NOACTIVATE_HELPER=\"$<TARGET_FILE:fv_ui_overlay_noactivate_helper>\"")
endif()
