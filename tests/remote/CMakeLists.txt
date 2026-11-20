fv_test(DEPS fv_testkit fv_golden fv_remote_wire_testkit fv_speech)

# The helper lives in a subfolder so fv_test does not compile it into fv_remote_tests.
if(TARGET fv_remote_tests)
    # The stand-in tailscale.exe of test_tailscale_cli.cpp's real-launcher test (modes in its
    # header comment). The test copies it into FV_REMOTE_SCRATCH, inside the build tree.
    add_executable(fv_remote_fake_tailscale fake_tailscale/main.cpp)
    target_compile_features(fv_remote_fake_tailscale PRIVATE cxx_std_23)
    fiorivoce_target_defaults(fv_remote_fake_tailscale)
    add_dependencies(fv_remote_tests fv_remote_fake_tailscale)

    target_compile_definitions(fv_remote_tests PRIVATE
        "FV_FAKE_TAILSCALE=\"$<TARGET_FILE:fv_remote_fake_tailscale>\""
        "FV_REMOTE_SCRATCH=\"${CMAKE_CURRENT_BINARY_DIR}/scratch\"")
endif()
