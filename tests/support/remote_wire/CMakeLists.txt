# fv_remote_wire_testkit: recorded OpenWhisper remote-engine exchanges
# (tests/fixtures/transcripts/remote_*.json, tools/parity/remote_wire.py), the comparison of live
# frames with them, and ScriptedPeer, a TLS WebSocket server that plays a transcript's host half.
# Headers are included as "remote_wire/transcript.h" and "remote_wire/scripted_peer.h". It links
# fv_net only (no remote/ header), so the remote libraries' tests can use it from the first wave.

add_library(fv_remote_wire_testkit STATIC
    transcript.h transcript.cpp scripted_peer.h scripted_peer.cpp)
target_include_directories(fv_remote_wire_testkit PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(fv_remote_wire_testkit PUBLIC
    fv_net fv_testkit nlohmann_json::nlohmann_json Catch2::Catch2)
target_compile_features(fv_remote_wire_testkit PUBLIC cxx_std_23)
fiorivoce_target_defaults(fv_remote_wire_testkit)

# Its self-test, built as fv_test builds a test directory's executable (the no-dialog and
# per-user-folder guards, Catch2's main, one data folder per test case), but named for the
# testkit and labelled support;unit.
set(target fv_remote_wire_testkit_tests)
set(no_dialogs "${PROJECT_SOURCE_DIR}/cmake/fv_test_no_dialogs.cpp")
set(isolation "${PROJECT_SOURCE_DIR}/cmake/fv_test_isolation.cpp")
add_executable(${target} test_transcript.cpp "${no_dialogs}" "${isolation}")
set_source_files_properties("${no_dialogs}" "${isolation}" PROPERTIES
    SKIP_PRECOMPILE_HEADERS ON SKIP_AUTOMOC ON SKIP_UNITY_BUILD_INCLUSION ON)
target_link_libraries(${target} PRIVATE fv_remote_wire_testkit fv_net Catch2::Catch2WithMain)
target_compile_features(${target} PRIVATE cxx_std_23)
fiorivoce_target_defaults(${target})

set(data_root "${CMAKE_CURRENT_BINARY_DIR}/data")
set(list_name "${target}_TESTS")
catch_discover_tests(${target} TEST_LIST ${list_name} TEST_PREFIX "remote_wire_testkit.")
set(properties_script "${CMAKE_CURRENT_BINARY_DIR}/${target}_properties.cmake")
file(CONFIGURE OUTPUT "${properties_script}" CONTENT [=[
if(@list_name@)
    set_tests_properties(${@list_name@} PROPERTIES LABELS "support;unit")
    foreach(fv_test_name IN LISTS @list_name@)
        string(SHA1 fv_test_key "${fv_test_name}")
        string(SUBSTRING "${fv_test_key}" 0 16 fv_test_key)
        file(MAKE_DIRECTORY "@data_root@/${fv_test_key}")
        set_tests_properties("${fv_test_name}" PROPERTIES
            ENVIRONMENT "FIORIVOCE_DATA_DIR=@data_root@/${fv_test_key}")
    endforeach()
endif()
]=] @ONLY)
set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES "${properties_script}")
