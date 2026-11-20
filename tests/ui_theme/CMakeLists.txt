fv_test(NO_MAIN QT Core Gui Widgets DEPS fv_testkit fv_testkit_qt fv_golden)

# tools/gen/style_props.py regenerates style_props.h from theme.qss with no difference. (The
# palette generator imports OpenWhisper, so it is checked by hand: tools/gen/palettes.py --check.)
if(TARGET fv_ui_theme_tests AND Python3_Interpreter_FOUND)
    add_test(NAME ui_theme.style_props_header_is_generated
        COMMAND Python3::Interpreter -I -B "${PROJECT_SOURCE_DIR}/tools/gen/style_props.py"
                --check)
    set_tests_properties(ui_theme.style_props_header_is_generated PROPERTIES
        LABELS "ui_theme;unit")
endif()
