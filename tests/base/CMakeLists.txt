fv_test()

# The other process of test_data_lease.cpp: holds a shared lease on its argument until its
# standard input closes. In a subfolder so fv_test does not compile it into fv_base_tests.
if(TARGET fv_base_tests)
    add_executable(fv_base_lease_helper lease_helper/main.cpp)
    target_link_libraries(fv_base_lease_helper PRIVATE fv_base)
    target_compile_features(fv_base_lease_helper PRIVATE cxx_std_23)
    fiorivoce_target_defaults(fv_base_lease_helper)
    add_dependencies(fv_base_tests fv_base_lease_helper)
    target_compile_definitions(fv_base_tests PRIVATE
        "FV_LEASE_HELPER=\"$<TARGET_FILE:fv_base_lease_helper>\"")
endif()
