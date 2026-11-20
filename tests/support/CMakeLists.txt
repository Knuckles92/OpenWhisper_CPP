# The test support libraries every tests/<dir> links (architecture 9.1), and their self-tests:
#
#   fv_testkit     Qt-free: temporary data root, RealtimeGuard, paths to tests/golden and
#                  tests/reference; links fv_base, so M0.2's fakes reach later tests through it
#   fv_golden      golden-vector loader and comparators (Catch2 sections per case)
#   fv_testkit_qt  Catch2 main with the pinned offscreen QApplication (fv_test(NO_MAIN ...))
#   fv_screenshot  tree dump and comparison, pixel comparison, diff images
#
# Headers are included as "testkit/testkit.h", "golden/golden.h", "screenshot/screenshot.h".
# fv_test learns that fv_testkit_qt and fv_screenshot need Qt from the FV_MODULE_QT_<target>
# properties set below, which test directories sorting before "support" are configured too early
# to see.

add_library(fv_testkit STATIC testkit/testkit.h testkit/testkit.cpp testkit/realtime_guard.cpp)
target_include_directories(fv_testkit PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(fv_testkit PUBLIC fv_base Catch2::Catch2)
target_compile_features(fv_testkit PUBLIC cxx_std_23)
target_compile_definitions(fv_testkit PRIVATE
    "FV_TESTKIT_SOURCE_DIR=\"${PROJECT_SOURCE_DIR}\""
    "FV_TESTKIT_BINARY_DIR=\"${CMAKE_BINARY_DIR}\"")
fiorivoce_target_defaults(fv_testkit)

add_library(fv_golden STATIC golden/golden.h golden/golden.cpp)
target_link_libraries(fv_golden PUBLIC fv_testkit nlohmann_json::nlohmann_json Catch2::Catch2)
fiorivoce_target_defaults(fv_golden)

if(FIORIVOCE_HAVE_QT)
    add_library(fv_testkit_qt STATIC testkit/qt_main.cpp)
    target_link_libraries(fv_testkit_qt PUBLIC fv_testkit Qt6::Widgets Catch2::Catch2)
    fiorivoce_target_defaults(fv_testkit_qt)

    add_library(fv_screenshot STATIC screenshot/screenshot.h screenshot/screenshot.cpp
                                     screenshot/dumped_widgets.h)
    target_link_libraries(fv_screenshot PUBLIC fv_testkit Qt6::Widgets Catch2::Catch2)
    set_target_properties(fv_screenshot PROPERTIES AUTOMOC ON)
    if(FIORIVOCE_SCREENSHOT_TESTS)
        target_compile_definitions(fv_screenshot PRIVATE FV_SCREENSHOT_PIXELS)
    endif()
    fiorivoce_target_defaults(fv_screenshot)

    # fv_test reads a dependency's Qt components from this property for the test environment
    # (offscreen platform, font folder) and the precompiled-header set.
    foreach(target IN ITEMS fv_testkit_qt fv_screenshot)
        set_property(GLOBAL PROPERTY FV_MODULE_QT_${target} "Core;Gui;Widgets")
    endforeach()

    # QT also links Qt6::Test (QSignalSpy, the QTest input helpers), which widget tests in other
    # directories use; this test keeps that path building under /WX.
    fv_test(NO_MAIN QT Core Gui Widgets DEPS fv_testkit fv_golden fv_testkit_qt fv_screenshot)
    target_compile_definitions(fv_support_tests PRIVATE FV_SUPPORT_SCREENSHOT)
else()
    set_property(GLOBAL APPEND PROPERTY FV_SKIPPED_MODULES fv_testkit_qt fv_screenshot)
    fv_test(DEPS fv_testkit fv_golden)
endif()

if(TARGET fv_net)
    add_subdirectory(remote_wire)
endif()

# The parity scripts' own tests, and every committed PARITY.json against the schema.
if(Python3_Interpreter_FOUND)
    add_test(NAME support.parity_tools
        COMMAND Python3::Interpreter -I -B -m unittest discover
                -s "${PROJECT_SOURCE_DIR}/tools/parity/tests" -p "test_*.py")
    add_test(NAME support.parity_json
        COMMAND Python3::Interpreter -I -B "${PROJECT_SOURCE_DIR}/tools/parity/parity_report.py"
                --check)
    set_tests_properties(support.parity_tools support.parity_json PROPERTIES
        LABELS "support;unit")
endif()
