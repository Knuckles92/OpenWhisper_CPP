fv_test(NO_MAIN QT Core Gui Widgets DEPS fv_testkit fv_testkit_qt fv_golden)

# The other process of test_platform.cpp's [interactive] test: TemporaryClipboard on Qt's
# windows platform, which the offscreen test executable cannot load. In a subfolder so fv_test
# does not compile it into fv_clipboard_tests.
if(TARGET fv_clipboard_tests AND WIN32)
    add_executable(fv_clipboard_platform_helper platform_helper/main.cpp)
    target_link_libraries(fv_clipboard_platform_helper PRIVATE fv_clipboard user32)
    target_compile_features(fv_clipboard_platform_helper PRIVATE cxx_std_23)
    openwhisper_cpp_target_defaults(fv_clipboard_platform_helper)
    fv_add_qt_runtime()
    add_dependencies(fv_clipboard_platform_helper fv_qt_runtime)
    add_dependencies(fv_clipboard_tests fv_clipboard_platform_helper)
    target_compile_definitions(fv_clipboard_tests PRIVATE
        "FV_CLIPBOARD_PLATFORM_HELPER=\"$<TARGET_FILE:fv_clipboard_platform_helper>\"")
endif()
