# The lints of architecture 9.5 as CTest tests (label "lint"), their self-tests, and tests of the
# build system's own rules on small fixture projects (label "cmake").
if(NOT Python3_Interpreter_FOUND)
    message(STATUS "Python 3.12 not found: the lint tests are not registered")
else()
    foreach(check IN ITEMS layering ownership qss_names brand style_props)
        add_test(NAME lint.${check}
            COMMAND Python3::Interpreter -I -B "${CMAKE_CURRENT_SOURCE_DIR}/check_${check}.py"
                    --root "${PROJECT_SOURCE_DIR}")
        set_tests_properties(lint.${check} PROPERTIES LABELS "lint")
    endforeach()

    foreach(selftest IN ITEMS check_layering lint_checks)
        add_test(NAME lint.selftest.${selftest}
            COMMAND Python3::Interpreter -I -B -m unittest discover -v
                    -s "${CMAKE_CURRENT_SOURCE_DIR}/tests" -p "test_${selftest}.py")
    endforeach()
    set_tests_properties(lint.selftest.check_layering lint.selftest.lint_checks
        PROPERTIES LABELS "lint")

    # webui/ is OpenWhisper's dashboard with the counted replacements of tools/webui (M7.6).
    add_test(NAME lint.webui_rebrand
        COMMAND Python3::Interpreter -I -B "${PROJECT_SOURCE_DIR}/tools/webui/rebrand.py" --check
                --root "${PROJECT_SOURCE_DIR}")
    add_test(NAME lint.selftest.webui_rebrand
        COMMAND Python3::Interpreter -I -B -m unittest discover -v
                -s "${PROJECT_SOURCE_DIR}/tools/webui" -p "test_rebrand.py")
    set_tests_properties(lint.webui_rebrand lint.selftest.webui_rebrand PROPERTIES LABELS "lint")
endif()

# configure-fails fixtures use stand-in Qt targets, so they need no Qt and compile nothing.
set(fixture_runner "${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/run_fixture.cmake")
set(fixture_common
    "-DFV_CMAKE_DIR=${PROJECT_SOURCE_DIR}/cmake" "-DFV_REAL_SRC=${PROJECT_SOURCE_DIR}/src"
    "-DGENERATOR=${CMAKE_GENERATOR}" "-DCXX=${CMAKE_CXX_COMPILER}"
    "-DMAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}")
# Each fixture as <folder>:<regex its configure error must match>; every one of these messages is
# part of the "Module layering (architecture 2.2) is violated" error of _fv_finalize.
foreach(entry IN ITEMS
        "core_depends_on_ui:may not depend on fv_widgets \\(ui\\)"
        "core_links_qt:is a core module and links Qt6::Core"
        "core_links_module_as_third_party:names the module fv_widgets as a third-party library")
    string(FIND "${entry}" ":" colon)
    string(SUBSTRING "${entry}" 0 ${colon} fixture)
    math(EXPR colon "${colon} + 1")
    string(SUBSTRING "${entry}" ${colon} -1 expect)
    add_test(NAME cmake.layering.${fixture}
        COMMAND "${CMAKE_COMMAND}" ${fixture_common}
                "-DFIXTURE=${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/${fixture}"
                "-DBUILD=${CMAKE_CURRENT_BINARY_DIR}/fixtures/${fixture}"
                -DMODE=configure-fails
                "-DEXPECT=${expect}"
                -P "${fixture_runner}")
    set_tests_properties(cmake.layering.${fixture} PROPERTIES LABELS "cmake")
endforeach()

# Two features in the generated registry, plus fv_embed, built and run for real.
if(FIORIVOCE_HAVE_QT)
    add_test(NAME cmake.feature_registry
        COMMAND "${CMAKE_COMMAND}" ${fixture_common} "-DQT6_DIR=${Qt6_DIR}"
                "-DFIXTURE=${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/feature_registry"
                "-DBUILD=${CMAKE_CURRENT_BINARY_DIR}/fixtures/feature_registry"
                -DMODE=run "-DEXPECT=^alpha=alpha beta=beta \\| embedded ok"
                -P "${fixture_runner}")
    set_tests_properties(cmake.feature_registry PROPERTIES LABELS "cmake" TIMEOUT 600)
endif()
