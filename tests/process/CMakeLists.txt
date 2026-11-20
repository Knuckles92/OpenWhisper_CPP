fv_test(DEPS fv_testkit fv_golden)

# The helpers live in subfolders so fv_test does not compile them into fv_process_tests.
if(TARGET fv_process_tests)
    # The child of test_detached.cpp: writes its working folder and its arguments to the file its
    # first argument names.
    add_executable(fv_process_detached_helper detached_helper/main.cpp)
    target_compile_features(fv_process_detached_helper PRIVATE cxx_std_23)
    openwhisper_cpp_target_defaults(fv_process_detached_helper)
    add_dependencies(fv_process_tests fv_process_detached_helper)

    # The program the run_once, probe and launcher tests start (commands in its header comment).
    add_executable(fv_process_child child/process_child.cpp)
    target_compile_features(fv_process_child PRIVATE cxx_std_23)
    openwhisper_cpp_target_defaults(fv_process_child)
    add_dependencies(fv_process_tests fv_process_child)

    target_compile_definitions(fv_process_tests PRIVATE
        "FV_DETACHED_HELPER=\"$<TARGET_FILE:fv_process_detached_helper>\""
        "FV_PROCESS_CHILD=\"$<TARGET_FILE:fv_process_child>\"")
endif()
