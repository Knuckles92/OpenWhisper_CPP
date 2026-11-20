fv_test(DEPS fv_testkit fv_engine fv_base fv_pyport fv_audio fv_speech fv_db fv_history
             fv_recorder fv_backup fv_agentapi fv_automation fv_net fv_http fv_process
             fv_settings fv_credentials fv_update fv_log fv_models)

# The tests run openwhisper-cpp-cli as a child process, as OpenWhisper's tests run main.py. The decoy
# runtime (decoy/) stands in for the NeMo-Speech.cpp library to show whether a command loaded one.
if(TARGET fv_cli_tests AND TARGET openwhisper-cpp-cli)
    add_library(fv_cli_decoy_runtime MODULE decoy/decoy_runtime.cpp)
    openwhisper_cpp_target_defaults(fv_cli_decoy_runtime)
    add_dependencies(fv_cli_tests openwhisper-cpp-cli fv_cli_decoy_runtime)
    target_compile_definitions(fv_cli_tests PRIVATE
        "FV_CLI_EXECUTABLE=\"$<TARGET_FILE:openwhisper-cpp-cli>\""
        "FV_CLI_DECOY_RUNTIME=\"$<TARGET_FILE:fv_cli_decoy_runtime>\"")

    # The installed check, the diagnostics report and the pieces of the workflow smoke are tested
    # in process too: an executable cannot be linked, so its sources are compiled into the tests.
    set(cli_sources pe_imports.cpp installed_check.cpp workflow_smoke.cpp channel_command.cpp
                    commands.cpp)
    if(WIN32)
        list(APPEND cli_sources win/installed_layout_win.cpp win/smoke_win.cpp)
        target_link_libraries(fv_cli_tests PRIVATE psapi)
    else()
        list(APPEND cli_sources posix/installed_layout_posix.cpp posix/smoke_posix.cpp)
    endif()
    list(TRANSFORM cli_sources PREPEND "${PROJECT_SOURCE_DIR}/src/cli/")
    target_sources(fv_cli_tests PRIVATE ${cli_sources})

    # openwhisper-cpp --channel hands the command to openwhisper-cpp-cli; the windowed channel tests start it
    # (it opens nothing before forwarding).
    if(TARGET openwhisper-cpp)
        add_dependencies(fv_cli_tests openwhisper-cpp)
        target_compile_definitions(fv_cli_tests PRIVATE
            "FV_APP_EXECUTABLE=\"$<TARGET_FILE:openwhisper-cpp>\"")
    endif()

    # The release gate end to end, as test_release_health.py's fresh-process test runs it:
    # check_release_health.py runs openwhisper-cpp-cli --workflow-smoke under its 45 s deadline and
    # checks the report it saves. The data root is a folder of the build tree; the smoke itself
    # works in a temporary folder of its own.
    if(Python3_Interpreter_FOUND)
        set(fv_cli_gate_dir "${CMAKE_CURRENT_BINARY_DIR}/release_health")
        add_test(NAME "cli.release health gate passes the workflow smoke"
            COMMAND Python3::Interpreter -I -B
                    "${PROJECT_SOURCE_DIR}/tools/release/check_release_health.py"
                    --executable "$<TARGET_FILE:openwhisper-cpp-cli>"
                    --output "${fv_cli_gate_dir}/release-health.json")
        set_tests_properties("cli.release health gate passes the workflow smoke" PROPERTIES
            LABELS "cli;platform" TIMEOUT 120
            ENVIRONMENT "OPENWHISPER_CPP_DATA_DIR=${fv_cli_gate_dir}/data")
    endif()
endif()
