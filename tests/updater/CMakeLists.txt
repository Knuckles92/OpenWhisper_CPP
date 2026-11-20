fv_test(DEPS fv_testkit fv_base fv_process fv_update fv_update_native)

# The tests run the built fiorivoce-updater as a child process, as the build scripts, the
# installer and the application run it. What its command line decides is tested in process by
# tests/update_native (update_native/updater_cli.h); here it is the program itself.
if(TARGET fv_updater_tests AND TARGET fiorivoce-updater)
    add_dependencies(fv_updater_tests fiorivoce-updater)
    target_compile_definitions(fv_updater_tests PRIVATE
        "FV_UPDATER_EXECUTABLE=\"$<TARGET_FILE:fiorivoce-updater>\"")
endif()
