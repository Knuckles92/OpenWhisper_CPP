fv_test(NO_MAIN QT Core Gui Widgets Network
        DEPS fv_testkit fv_testkit_qt fv_screenshot fv_golden fv_ui_shell fv_appkit fv_settings
             fv_credentials fv_ui_theme fv_ui_widgets fv_uimath fv_audio fv_db fv_appcore
             fv_backup fv_update fv_agents fv_process)

# The early command line is tested on the real executable, as a child process: --help and
# --version must answer before the app takes its lease or writes its log.
if(TARGET fv_ui_shell_tests AND TARGET fiorivoce)
    add_dependencies(fv_ui_shell_tests fiorivoce)
    target_compile_definitions(fv_ui_shell_tests PRIVATE
        "FV_GUI_EXECUTABLE=\"$<TARGET_FILE:fiorivoce>\"")
endif()
