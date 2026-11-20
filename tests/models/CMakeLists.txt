fv_test(DEPS fv_testkit fv_golden fv_uimath Boost::asio OpenSSL::SSL OpenSSL::Crypto)

# src/models/data/speech_catalog.json must be what tools/gen/speech_catalog.py writes from the
# pinned OpenWhisper checkout. The check exists only where OPENWHISPER_CPP_OW_PYTHON and
# OPENWHISPER_CPP_OW_ROOT were set when the tree was configured.
if(TARGET fv_models_tests AND DEFINED ENV{OPENWHISPER_CPP_OW_PYTHON} AND DEFINED ENV{OPENWHISPER_CPP_OW_ROOT}
   AND EXISTS "$ENV{OPENWHISPER_CPP_OW_PYTHON}")
    add_test(NAME models.speech_catalog_is_current
        COMMAND "$ENV{OPENWHISPER_CPP_OW_PYTHON}" -I -B
                "${PROJECT_SOURCE_DIR}/tools/gen/speech_catalog.py" --check
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
    set_tests_properties(models.speech_catalog_is_current PROPERTIES
        LABELS "models;golden"
        ENVIRONMENT "OPENWHISPER_CPP_OW_ROOT=$ENV{OPENWHISPER_CPP_OW_ROOT}")
elseif(TARGET fv_models_tests)
    message(STATUS "OPENWHISPER_CPP_OW_PYTHON or OPENWHISPER_CPP_OW_ROOT is not set: "
                   "models.speech_catalog_is_current is not registered")
endif()
