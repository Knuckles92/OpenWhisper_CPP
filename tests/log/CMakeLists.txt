fv_test(DEPS fv_testkit fv_golden)

# The other process of test_crash_handler.cpp and test_setup.cpp: installs the crash handler (or
# the whole logging setup) and then crashes or logs, so the test process keeps its own handlers.
# In a subfolder so fv_test does not compile it into fv_log_tests.
if(TARGET fv_log_tests)
    add_executable(fv_log_crash_helper crash_helper/main.cpp)
    target_link_libraries(fv_log_crash_helper PRIVATE fv_log)
    target_compile_features(fv_log_crash_helper PRIVATE cxx_std_23)
    openwhisper_cpp_target_defaults(fv_log_crash_helper)
    add_dependencies(fv_log_tests fv_log_crash_helper)
    target_compile_definitions(fv_log_tests PRIVATE
        "FV_LOG_CRASH_HELPER=\"$<TARGET_FILE:fv_log_crash_helper>\"")
endif()
