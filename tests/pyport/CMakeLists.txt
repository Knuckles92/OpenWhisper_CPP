fv_test()

# The golden replay reads the committed vectors from the source tree.
if(TARGET fv_pyport_tests)
    target_compile_definitions(fv_pyport_tests PRIVATE
        FV_PYPORT_GOLDEN="${PROJECT_SOURCE_DIR}/tests/golden/builtins.json")
endif()

# The committed Unicode tables must be what tools/gen/unicode_tables.py writes. The generator
# refuses any unicodedata but Python 3.12's 15.0.0, and the interpreter the lints use may be a
# newer Python, so the check looks for a 3.12 of its own: FIORIVOCE_PYTHON312 if set, else
# FIORIVOCE_OW_PYTHON (OpenWhisper's interpreter, architecture 9), else the lints' Python when it
# is 3.12, else a registered or PATH Python 3.12.
function(_fv_pyport_python312 out)
    set(candidates "")
    if(FIORIVOCE_PYTHON312)
        list(APPEND candidates "${FIORIVOCE_PYTHON312}")
    endif()
    if(DEFINED ENV{FIORIVOCE_OW_PYTHON})
        list(APPEND candidates "$ENV{FIORIVOCE_OW_PYTHON}")
    endif()
    if(TARGET Python3::Interpreter)
        list(APPEND candidates "${Python3_EXECUTABLE}")
    endif()
    find_program(_fv_python312_found NAMES python3.12 python312 python
        PATHS "[HKEY_CURRENT_USER\\SOFTWARE\\Python\\PythonCore\\3.12\\InstallPath]"
              "[HKEY_LOCAL_MACHINE\\SOFTWARE\\Python\\PythonCore\\3.12\\InstallPath]"
        NO_DEFAULT_PATH NO_CACHE)
    if(_fv_python312_found)
        list(APPEND candidates "${_fv_python312_found}")
    endif()
    find_program(_fv_python312_path NAMES python3.12 NO_CACHE)
    if(_fv_python312_path)
        list(APPEND candidates "${_fv_python312_path}")
    endif()
    foreach(candidate IN LISTS candidates)
        if(NOT EXISTS "${candidate}")
            continue()
        endif()
        execute_process(
            COMMAND "${candidate}" -I -B -c
                    "import sys, unicodedata; print(sys.version_info[:2] == (3, 12) and unicodedata.unidata_version == '15.0.0')"
            OUTPUT_VARIABLE answer OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE result ERROR_QUIET)
        if(result EQUAL 0 AND answer STREQUAL "True")
            set(${out} "${candidate}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    set(${out} "" PARENT_SCOPE)
endfunction()

if(TARGET fv_pyport_tests)
    _fv_pyport_python312(fv_pyport_python312)
    if(fv_pyport_python312)
        add_test(NAME pyport.unicode_tables_are_current
            COMMAND "${fv_pyport_python312}" -I -B
                    "${PROJECT_SOURCE_DIR}/tools/gen/unicode_tables.py" --check)
        set_tests_properties(pyport.unicode_tables_are_current PROPERTIES LABELS "pyport;golden")
    else()
        message(STATUS "No Python 3.12 found (set FIORIVOCE_PYTHON312 or FIORIVOCE_OW_PYTHON): "
                       "pyport.unicode_tables_are_current is not registered")
    endif()
endif()
