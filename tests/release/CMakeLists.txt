# The release tools' tests (tools/release/tests): Python 3.12 standard library only, no C++. They
# run beside the parity scripts' own tests the way tests/support registers those, and use a copy
# of the release identity (tools/release/tests/fixtures), so they need no other module's files.
if(Python3_Interpreter_FOUND)
    add_test(NAME release.tools
        COMMAND Python3::Interpreter -I -B -m unittest discover
                -s "${PROJECT_SOURCE_DIR}/tools/release/tests" -p "test_*.py")
    set_tests_properties(release.tools PROPERTIES LABELS "release;unit" TIMEOUT 300)
else()
    message(STATUS "Python 3.12 not found: release.tools is not registered")
endif()
