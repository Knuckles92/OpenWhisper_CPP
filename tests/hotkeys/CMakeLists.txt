fv_test(DEPS fv_testkit fv_golden)

# src/hotkeys/data/keyboard_names.json must be what tools/gen/keyboard_names.py writes from the
# `keyboard` package in OpenWhisper's interpreter. The generator only asks the layout for key
# names (no hook, no input), and the committed file is the US layout's, so the check exists only
# where OPENWHISPER_CPP_OW_PYTHON was set when the tree was configured.
if(TARGET fv_hotkeys_tests AND WIN32 AND DEFINED ENV{OPENWHISPER_CPP_OW_PYTHON}
   AND EXISTS "$ENV{OPENWHISPER_CPP_OW_PYTHON}")
    add_test(NAME hotkeys.keyboard_names_are_current
        COMMAND "$ENV{OPENWHISPER_CPP_OW_PYTHON}" -I -B
                "${PROJECT_SOURCE_DIR}/tools/gen/keyboard_names.py" --check
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
    set_tests_properties(hotkeys.keyboard_names_are_current PROPERTIES LABELS "hotkeys;golden")
elseif(TARGET fv_hotkeys_tests)
    message(STATUS "OPENWHISPER_CPP_OW_PYTHON is not set: "
                   "hotkeys.keyboard_names_are_current is not registered")
endif()
