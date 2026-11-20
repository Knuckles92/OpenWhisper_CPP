fv_test(DEPS fv_testkit fv_golden)

# keys.inc and data/enums.json must be what tools/gen/settings_keys.py writes from the pinned
# OpenWhisper checkout. The generator imports OpenWhisper (read-only, through
# tools/parity/ow_isolation.py), so the check exists only where FIORIVOCE_OW_PYTHON and
# FIORIVOCE_OW_ROOT were set when the tree was configured.
if(TARGET fv_settings_tests AND DEFINED ENV{FIORIVOCE_OW_PYTHON} AND DEFINED ENV{FIORIVOCE_OW_ROOT}
   AND EXISTS "$ENV{FIORIVOCE_OW_PYTHON}")
    add_test(NAME settings.keys_are_current
        COMMAND "$ENV{FIORIVOCE_OW_PYTHON}" -I -B
                "${PROJECT_SOURCE_DIR}/tools/gen/settings_keys.py" --check
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
    set_tests_properties(settings.keys_are_current PROPERTIES
        LABELS "settings;golden"
        ENVIRONMENT "FIORIVOCE_OW_ROOT=$ENV{FIORIVOCE_OW_ROOT}")
elseif(TARGET fv_settings_tests)
    message(STATUS "FIORIVOCE_OW_PYTHON or FIORIVOCE_OW_ROOT is not set: "
                   "settings.keys_are_current is not registered")
endif()
