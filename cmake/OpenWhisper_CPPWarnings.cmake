include_guard(GLOBAL)

option(OPENWHISPER_CPP_WARNINGS_AS_ERRORS "Fail the build on a warning in OpenWhisper_CPP's own targets" OFF)

# openwhisper_cpp_target_defaults(<target>): warnings and source encoding for our own
# targets. Third-party code never goes through this.
function(openwhisper_cpp_target_defaults target)
    # Qt-free headers use emit, slots and signals as ordinary names (fv::Signal::emit), which
    # Qt's keyword macros would rewrite in any file that also includes a Qt header. Qt code
    # writes Q_EMIT, Q_SIGNALS and Q_SLOTS instead. Every target gets it, Qt-free ones too, so
    # that targets sharing a precompiled header compile with the same definitions.
    target_compile_definitions(${target} PRIVATE QT_NO_KEYWORDS)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
        target_compile_definitions(${target} PRIVATE
            NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
        if(OPENWHISPER_CPP_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        # Designated initializers that leave the rest of a struct at its defaults are the house
        # style, so the warning about fields they do not name says nothing.
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic
            -Wno-missing-field-initializers)
        if(OPENWHISPER_CPP_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()
