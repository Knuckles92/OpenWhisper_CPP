fv_test(DEPS fv_testkit fv_golden fv_http Boost::asio OpenSSL::SSL OpenSSL::Crypto)

# test_loopback_feed.cpp serves a setup through the release tools' loopback feed
# (tools/release/serve_update_feed.py); without Python it skips.
if(TARGET fv_update_tests AND TARGET Python3::Interpreter)
    target_compile_definitions(fv_update_tests PRIVATE
        "FV_PYTHON=\"$<TARGET_FILE:Python3::Interpreter>\""
        "FV_FEED_SERVER=\"${PROJECT_SOURCE_DIR}/tools/release/serve_update_feed.py\"")
endif()
