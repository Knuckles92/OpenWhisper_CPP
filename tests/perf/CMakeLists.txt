# The measuring tests of M10.3 (tools/perf/README.md). The directory's name gives every case the
# CTest label "perf" (and CTest names "perf.<case>"), which tools/perf/run_perf.py selects. Each
# case writes out/perf/<metric>.json and asserts only a ten-times sanity ceiling; budgets are
# checked by tools/perf/check_budgets.py. Nothing here reaches the desktop: hooks, tray, devices
# and clipboard are fakes, and every case has its own temporary data folder.
fv_test(NO_MAIN QT Core Gui Widgets
        DEPS fv_testkit fv_testkit_qt fv_ui_controller fv_ui_shell fv_ui_record fv_ui_history
             fv_ui_overlay fv_ui_setup fv_ui_settings fv_ui_downloads fv_appkit fv_appcore
             fv_settings fv_audio fv_recorder fv_speech fv_sql fv_db fv_history fv_dictation
             fv_focus fv_clipboard fv_hotkeys fv_models fv_log fv_engine fv_http fv_base fv_pyport
             fv_ui_theme fv_ui_widgets fv_uimath fv_credentials fv_update fv_backup fv_process
             fv_text fv_llm fv_ui_personalize fv_ui_hotkeys fv_ui_walkthrough fv_automation
             fv_meeting_state fv_meeting_agent fv_net fv_remote fv_remote_host
             fv_remote_wire_testkit Boost::asio OpenSSL::SSL OpenSSL::Crypto)

if(TARGET fv_perf_tests)
    # The build configuration a number was measured with goes into its record: a Debug number
    # is not a release number.
    target_compile_definitions(fv_perf_tests PRIVATE "FV_PERF_CONFIG=\"$<CONFIG>\"")
endif()
