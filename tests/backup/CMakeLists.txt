fv_test(DEPS fv_testkit fv_golden)

# The other process of test_startup_gate.cpp: holds a shared data lease on its argument until its
# standard input closes. In a subfolder so fv_test does not compile it into fv_backup_tests. The
# test drives it through tests/base's ChildProcess.
if(TARGET fv_backup_tests)
    add_executable(fv_backup_lease_helper lease_helper/main.cpp)
    target_link_libraries(fv_backup_lease_helper PRIVATE fv_base)
    target_compile_features(fv_backup_lease_helper PRIVATE cxx_std_23)
    fiorivoce_target_defaults(fv_backup_lease_helper)
    add_dependencies(fv_backup_tests fv_backup_lease_helper)
    target_compile_definitions(fv_backup_tests PRIVATE
        "FV_BACKUP_LEASE_HELPER=\"$<TARGET_FILE:fv_backup_lease_helper>\"")
    target_sources(fv_backup_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests/base/test_support.cpp")
    target_include_directories(fv_backup_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests/base")
endif()
