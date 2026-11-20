# The interfaces of architecture sections 3 and 5. Each public header of the contract modules
# is compiled alone in its own translation unit (twice included, so its include guard is tested
# too): the Qt-free ones with no Qt include path, appkit's against Qt. Those object libraries
# link nothing; building them is the test, and fv_contracts_tests depends on them.
set(core_dirs base settings credentials http net sql db process audio speech)
set(core_modules fv_base fv_settings fv_credentials fv_http fv_net fv_sql fv_db fv_process
                 fv_audio fv_speech)

fv_test(DEPS ${core_modules})

# appkit's order.h and destinations.h are Qt-free, so they compile alone without Qt as well, and
# so also in the windows-core and linux-core builds.
set(core_dirs ${core_dirs} appkit/order.h appkit/destinations.h)

# Each entry of `dirs` is a directory under src/ (every public header in it) or one header.
function(contracts_compile_alone name dirs prologue)
    set(sources "")
    foreach(dir IN LISTS dirs)
        if(dir MATCHES "\\.h$")
            set(pattern "${PROJECT_SOURCE_DIR}/src/${dir}")
        else()
            set(pattern "${PROJECT_SOURCE_DIR}/src/${dir}/*.h")
        endif()
        file(GLOB headers CONFIGURE_DEPENDS RELATIVE "${PROJECT_SOURCE_DIR}/src" "${pattern}")
        foreach(header IN LISTS headers)
            string(MAKE_C_IDENTIFIER "${header}" stem)
            set(source "${CMAKE_CURRENT_BINARY_DIR}/alone/${name}/${stem}.cpp")
            file(CONFIGURE OUTPUT "${source}" @ONLY CONTENT
                "@prologue@#include \"@header@\"\n#include \"@header@\"\n")
            list(APPEND sources "${source}")
        endforeach()
    endforeach()
    add_library(${name} OBJECT ${sources})
    target_link_libraries(${name} PRIVATE ${ARGN})
    fiorivoce_target_defaults(${name})
    if(TARGET fv_contracts_tests)
        add_dependencies(fv_contracts_tests ${name})
    endif()
endfunction()

if(TARGET fv_contracts_tests)
    set(no_qt [=[
#if __has_include(<QtCore/qglobal.h>) || __has_include(<qglobal.h>)
#error "A Qt-free contract header is being compiled with a Qt include path."
#endif
]=])
    contracts_compile_alone(fv_contracts_core_alone "${core_dirs}" "${no_qt}" ${core_modules})
    if(TARGET fv_appkit)
        contracts_compile_alone(fv_contracts_qt_alone "appkit" "" fv_appkit)
    endif()
endif()
