fv_test(DEPS fv_testkit fv_golden fv_http)

# The helper lives in a subfolder so fv_test does not compile it into fv_agents_tests.
if(TARGET fv_agents_tests)
    # The coding agents' stand-in for the [platform] ask and pass tests (commands in its header
    # comment). A pass calls the app's tools back over loopback HTTP, hence the socket library.
    add_executable(fv_fake_agent fake_agent/fake_agent.cpp)
    target_compile_features(fv_fake_agent PRIVATE cxx_std_23)
    openwhisper_cpp_target_defaults(fv_fake_agent)
    target_link_libraries(fv_fake_agent PRIVATE nlohmann_json::nlohmann_json
                                                $<$<PLATFORM_ID:Windows>:ws2_32>)
    add_dependencies(fv_agents_tests fv_fake_agent)

    target_compile_definitions(fv_agents_tests PRIVATE
        "FV_FAKE_AGENT=\"$<TARGET_FILE:fv_fake_agent>\"")
endif()
