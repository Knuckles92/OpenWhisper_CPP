fv_test(DEPS fv_testkit fv_golden Boost::asio)

# The helper lives in a subfolder so fv_test does not compile it into fv_automation_tests.
if(TARGET fv_automation_tests)
    # The program the program hook and program filter tests start (commands in its header
    # comment).
    add_executable(fv_automation_hook_child hook_child/hook_child.cpp)
    target_compile_features(fv_automation_hook_child PRIVATE cxx_std_23)
    target_link_libraries(fv_automation_hook_child PRIVATE nlohmann_json::nlohmann_json)
    if(WIN32)
        target_link_libraries(fv_automation_hook_child PRIVATE shell32)
    endif()
    fiorivoce_target_defaults(fv_automation_hook_child)
    add_dependencies(fv_automation_tests fv_automation_hook_child)
    target_compile_definitions(fv_automation_tests PRIVATE
        "FV_HOOK_CHILD=\"$<TARGET_FILE:fv_automation_hook_child>\"")
endif()
