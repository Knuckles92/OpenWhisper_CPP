fv_test(DEPS fv_testkit fv_golden fv_http Boost::asio OpenSSL::SSL OpenSSL::Crypto)

# src/llm/data/text_model_catalog.json must be what tools/gen/text_model_catalog.py writes from
# the pinned OpenWhisper checkout. The check exists only where OPENWHISPER_CPP_OW_PYTHON and
# OPENWHISPER_CPP_OW_ROOT were set when the tree was configured.
if(TARGET fv_llm_tests AND DEFINED ENV{OPENWHISPER_CPP_OW_PYTHON} AND DEFINED ENV{OPENWHISPER_CPP_OW_ROOT}
   AND EXISTS "$ENV{OPENWHISPER_CPP_OW_PYTHON}")
    add_test(NAME llm.text_model_catalog_is_current
        COMMAND "$ENV{OPENWHISPER_CPP_OW_PYTHON}" -I -B
                "${PROJECT_SOURCE_DIR}/tools/gen/text_model_catalog.py" --check
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
    set_tests_properties(llm.text_model_catalog_is_current PROPERTIES
        LABELS "llm;golden"
        ENVIRONMENT "OPENWHISPER_CPP_OW_ROOT=$ENV{OPENWHISPER_CPP_OW_ROOT}")
elseif(TARGET fv_llm_tests)
    message(STATUS "OPENWHISPER_CPP_OW_PYTHON or OPENWHISPER_CPP_OW_ROOT is not set: "
                   "llm.text_model_catalog_is_current is not registered")
endif()
