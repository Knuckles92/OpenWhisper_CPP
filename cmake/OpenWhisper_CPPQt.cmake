include_guard(GLOBAL)

# fv_qt_runtime: copies the Qt DLLs and plugins that OpenWhisper_CPP's executables load into the
# runtime output folder, once per build instead of once per executable, so every executable runs
# from the build tree without Qt on PATH. Other platforms find Qt through the rpath, so there the
# target does nothing.
function(fv_add_qt_runtime)
    if(TARGET fv_qt_runtime)
        return()
    endif()
    if(NOT WIN32)
        add_custom_target(fv_qt_runtime)
        return()
    endif()

    get_property(multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    if(multi_config)
        set(dest "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
    else()
        set(dest "${CMAKE_BINARY_DIR}/bin")
    endif()

    set(dlls "")
    foreach(module IN ITEMS Core Gui Widgets Svg SvgWidgets Network Test)
        if(TARGET Qt6::${module})
            list(APPEND dlls "$<TARGET_FILE:Qt6::${module}>")
        endif()
    endforeach()
    set(commands COMMAND "${CMAKE_COMMAND}" -E make_directory "${dest}"
                 COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${dlls} "${dest}")

    # <plugin folder>:<imported plugin target>. offscreen is what CTest runs widget tests on;
    # windows11 (QModernWindowsStyle) is the base style the reference screenshots pin.
    foreach(entry IN ITEMS
            platforms:QWindowsIntegrationPlugin
            platforms:QOffscreenIntegrationPlugin
            styles:QModernWindowsStylePlugin
            imageformats:QSvgPlugin
            imageformats:QICOPlugin
            imageformats:QGifPlugin
            imageformats:QJpegPlugin
            iconengines:QSvgIconPlugin)
        string(REPLACE ":" ";" parts "${entry}")
        list(GET parts 0 folder)
        list(GET parts 1 plugin)
        if(TARGET Qt6::${plugin})
            list(APPEND commands
                COMMAND "${CMAKE_COMMAND}" -E make_directory "${dest}/${folder}"
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "$<TARGET_FILE:Qt6::${plugin}>" "${dest}/${folder}")
        endif()
    endforeach()

    add_custom_target(fv_qt_runtime ${commands}
        COMMENT "Copying the Qt runtime into the output folder"
        VERBATIM)
endfunction()

# openwhisper_cpp_deploy_qt(<target>): the target runs from the build tree. Kept for the CMake files
# written before fv_module; it only adds the dependency on fv_qt_runtime.
function(openwhisper_cpp_deploy_qt target)
    fv_add_qt_runtime()
    add_dependencies(${target} fv_qt_runtime)
endfunction()
