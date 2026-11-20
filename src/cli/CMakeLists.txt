# openwhisper-cpp-cli: console, Qt-free. The `default` preset (OPENWHISPER_CPP_ENGINE_ONLY) configures only
# src/engine and this directory, without vcpkg, so there the executable keeps to the engine's
# smoke path (transcribe, --help, --version); --diagnostics, --self-test, --workflow-smoke, --api
# and --channel need the core libraries and exist in every other configuration.
if(OPENWHISPER_CPP_ENGINE_ONLY)
    add_executable(openwhisper-cpp-cli main.cpp transcribe.cpp usage.cpp)
    target_include_directories(openwhisper-cpp-cli PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_compile_definitions(openwhisper-cpp-cli PRIVATE FV_CLI_ENGINE_ONLY)
    target_link_libraries(openwhisper-cpp-cli PRIVATE OpenWhisper_CPP::engine)
    openwhisper_cpp_target_defaults(openwhisper-cpp-cli)
else()
    fv_module(KIND exe NAME openwhisper-cpp-cli
              DEPS fv_engine fv_base fv_pyport fv_log fv_settings fv_models fv_audio fv_speech
                   fv_db fv_history fv_recorder fv_backup fv_agentapi fv_automation fv_net
                   fv_http fv_update
              WIN32_LIBS psapi)
endif()
