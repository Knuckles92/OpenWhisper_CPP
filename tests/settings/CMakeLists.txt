fv_test(DEPS fv_testkit fv_golden)

# keys.inc and data/enums.json must be what tools/gen/settings_keys.py writes from the pinned
# OpenWhisper checkout. The generator imports OpenWhisper (read-only, through
# tools/parity/ow_isolation.py), so the check exists only where OPENWHISPER_CPP_OW_PYTHON and
# OPENWHISPER_CPP_OW_ROOT were set when the tree was configured.
if(TARGET fv_settings_tests AND DEFINED ENV{OPENWHISPER_CPP_OW_PYTHON} AND DEFINED ENV{OPENWHISPER_CPP_OW_ROOT}
   AND EXISTS "$ENV{OPENWHISPER_CPP_OW_PYTHON}")
    add_test(NAME settings.keys_are_current
        COMMAND "$ENV{OPENWHISPER_CPP_OW_PYTHON}" -I -B
                "${PROJECT_SOURCE_DIR}/tools/gen/settings_keys.py" --check
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
    set_tests_properties(settings.keys_are_current PROPERTIES
        LABELS "settings;golden"
        ENVIRONMENT "OPENWHISPER_CPP_OW_ROOT=$ENV{OPENWHISPER_CPP_OW_ROOT}")
elseif(TARGET fv_settings_tests)
    message(STATUS "OPENWHISPER_CPP_OW_PYTHON or OPENWHISPER_CPP_OW_ROOT is not set: "
                   "settings.keys_are_current is not registered")
endif()
