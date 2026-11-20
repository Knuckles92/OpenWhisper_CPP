# Each tests/<dir> is added by the top-level CMakeLists.txt and declares itself with fv_test().
# This file holds only what belongs to no single test directory.

# The development bench's widget test, until M0.11 retires src/app's bench and tests/app with it.
# It uses Qt Test's own runner, so it is registered by hand rather than through fv_test.
if(TARGET fv_app AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/app/test_main_window.cpp")
    qt_add_executable(fv_app_tests app/test_main_window.cpp)
    set_target_properties(fv_app_tests PROPERTIES AUTOMOC ON)
    target_link_libraries(fv_app_tests PRIVATE OpenWhisper_CPP::app Qt6::Test)
    openwhisper_cpp_target_defaults(fv_app_tests)
    openwhisper_cpp_deploy_qt(fv_app_tests)
    add_test(NAME app.main_window COMMAND fv_app_tests)
    set_tests_properties(app.main_window PROPERTIES
        LABELS "app;widget"
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
