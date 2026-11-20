fv_test(NO_MAIN QT Core Gui Widgets DEPS fv_testkit fv_testkit_qt)

# The other process of test_qt_bridge.cpp's assert_not_gui_thread test: the assertion aborts, so
# it cannot run inside the test executable. In a subfolder so fv_test does not compile it into
# fv_appkit_tests.
if(TARGET fv_appkit_tests)
    add_executable(fv_appkit_gui_thread_helper gui_thread_helper/main.cpp)
    target_link_libraries(fv_appkit_gui_thread_helper PRIVATE fv_appkit)
    target_compile_features(fv_appkit_gui_thread_helper PRIVATE cxx_std_23)
    openwhisper_cpp_target_defaults(fv_appkit_gui_thread_helper)
    fv_add_qt_runtime()
    add_dependencies(fv_appkit_gui_thread_helper fv_qt_runtime)
    add_dependencies(fv_appkit_tests fv_appkit_gui_thread_helper)
    target_compile_definitions(fv_appkit_tests PRIVATE
        "FV_GUI_THREAD_HELPER=\"$<TARGET_FILE:fv_appkit_gui_thread_helper>\"")
endif()
