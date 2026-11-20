fv_test(DEPS fv_testkit fv_golden fv_settings)

# data/common_words.json must be what tools/gen/dictionary_words.py writes from the pinned
# OpenWhisper checkout. The generator imports OpenWhisper (read-only, through
# tools/parity/ow_isolation.py), so the check exists only where FIORIVOCE_OW_PYTHON and
# FIORIVOCE_OW_ROOT were set when the tree was configured.
if(TARGET fv_text_tests AND DEFINED ENV{FIORIVOCE_OW_PYTHON} AND DEFINED ENV{FIORIVOCE_OW_ROOT}
   AND EXISTS "$ENV{FIORIVOCE_OW_PYTHON}")
    add_test(NAME text.common_words_are_current
        COMMAND "$ENV{FIORIVOCE_OW_PYTHON}" -I -B
                "${PROJECT_SOURCE_DIR}/tools/gen/dictionary_words.py" --check
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
    set_tests_properties(text.common_words_are_current PROPERTIES
        LABELS "text;golden"
        ENVIRONMENT "FIORIVOCE_OW_ROOT=$ENV{FIORIVOCE_OW_ROOT}")
    # data/cleanup_prompts.json the same way, from tools/gen/cleanup_prompts.py.
    add_test(NAME text.cleanup_prompts_are_current
        COMMAND "$ENV{FIORIVOCE_OW_PYTHON}" -I -B
                "${PROJECT_SOURCE_DIR}/tools/gen/cleanup_prompts.py" --check
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
    set_tests_properties(text.cleanup_prompts_are_current PROPERTIES
        LABELS "text;golden"
        ENVIRONMENT "FIORIVOCE_OW_ROOT=$ENV{FIORIVOCE_OW_ROOT}")
elseif(TARGET fv_text_tests)
    message(STATUS "FIORIVOCE_OW_PYTHON or FIORIVOCE_OW_ROOT is not set: "
                   "text.common_words_are_current and text.cleanup_prompts_are_current are not "
                   "registered")
endif()
