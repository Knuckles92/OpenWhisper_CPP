# The only functions a package calls from its CMakeLists.txt (architecture section 10.1):
#
#   fv_module(KIND <core|qt|ui|feature|exe> [NAME <target>] [GUI] [FEATURES]
#             [DEPS <fv targets>...] [THIRD_PARTY <targets>...]
#             [PRIVATE_THIRD_PARTY <targets>...] [WIN32_LIBS <libs>...] [QT <components>...])
#   fv_feature(ID <id> CLASS <C++ class> [HEADER <include path>])
#   fv_embed(<data/file.json>...)
#   fv_test([DEPS <targets>...] [LABELS <labels>...] [QT <components>...] [NO_MAIN])
#
# A src/<dir>/CMakeLists.txt is one fv_module call (plus fv_feature and fv_embed calls); a
# tests/<dir>/CMakeLists.txt is one fv_test call. Nothing else needs editing to add a module, a
# test, a feature or a source file: the root globs the directories, fv_module and fv_test glob the
# files, and the feature registry is generated.
include_guard(GLOBAL)

include(FioriVoceWarnings)
include(FioriVoceQt)

option(FIORIVOCE_PCH "Compile the shared precompiled headers and reuse them in every target" ON)

set(_FV_MODULES_DIR "${CMAKE_CURRENT_LIST_DIR}")
set(_FV_KINDS core qt ui feature exe)

# Qt components each kind links by default and may link at most (architecture 2.2).
set(_FV_QT_DEFAULT_qt Core Gui)
set(_FV_QT_DEFAULT_ui Core Gui Widgets Svg SvgWidgets)
set(_FV_QT_DEFAULT_feature Core Gui Widgets Svg SvgWidgets)
set(_FV_QT_ALLOWED_qt Core Gui)
set(_FV_QT_ALLOWED_ui Core Gui Widgets Svg SvgWidgets)
set(_FV_QT_ALLOWED_feature Core Gui Widgets Svg SvgWidgets)
# The two named exceptions: QtWidgets in appkit (AppContext's registries build widgets) and
# QtNetwork in ui_shell (QLocalServer for the single instance).
set(_FV_QT_EXTRA_fv_appkit Widgets)
set(_FV_QT_EXTRA_fv_ui_shell Network)

# Kinds a module of each kind may depend on.
set(_FV_DEPS_ALLOWED_core core)
set(_FV_DEPS_ALLOWED_qt core qt)
set(_FV_DEPS_ALLOWED_ui core qt ui feature)
set(_FV_DEPS_ALLOWED_feature core qt ui feature)
set(_FV_DEPS_ALLOWED_exe core qt ui feature exe)

function(_fv_get name out)
    get_property(value GLOBAL PROPERTY "${name}")
    set(${out} "${value}" PARENT_SCOPE)
endfunction()

function(_fv_qt_available out)
    if(TARGET Qt6::Core)
        set(${out} TRUE PARENT_SCOPE)
    else()
        set(${out} FALSE PARENT_SCOPE)
    endif()
endfunction()

# Precompiled headers. A target reuses a donor's PCH only when both compile with the same
# definitions and options (MSVC warns C4605/C4651 otherwise, an error under /WX), and those come
# from everything the target links: each Qt component adds QT_<NAME>_LIB, the Boost libraries add
# BOOST_*_DYN_LINK. So fv_module and fv_test only record the request, and _fv_finalize, once every
# target exists, gives each distinct set of definitions and options one donor: fv_pch_core for
# the Qt-free set, fv_pch_qt for the widget set, fv_pch_<kind>_<hash> for the rest.
function(_fv_request_pch target header_kind)
    if(FIORIVOCE_PCH)
        set_property(GLOBAL APPEND PROPERTY FV_PCH_TARGETS ${target})
        set_property(GLOBAL PROPERTY FV_PCH_KIND_${target} ${header_kind})
    endif()
endfunction()

# The compile definitions and options a target compiles with: its own plus the usage
# requirements of everything it links, transitively. Link-only items carry none.
function(_fv_usage_requirements target out_definitions out_options)
    get_target_property(definitions ${target} COMPILE_DEFINITIONS)
    get_target_property(options ${target} COMPILE_OPTIONS)
    get_target_property(queue ${target} LINK_LIBRARIES)
    foreach(variable IN ITEMS definitions options queue)
        if(NOT ${variable})
            set(${variable} "")
        endif()
    endforeach()
    set(seen "")
    while(queue)
        list(POP_FRONT queue item)
        if(item MATCHES "^\\$<" OR item MATCHES "^::@" OR NOT TARGET "${item}")
            continue()
        endif()
        get_target_property(aliased "${item}" ALIASED_TARGET)
        if(aliased)
            set(item ${aliased})
        endif()
        if(item IN_LIST seen)
            continue()
        endif()
        list(APPEND seen ${item})
        foreach(property IN ITEMS INTERFACE_COMPILE_DEFINITIONS INTERFACE_COMPILE_OPTIONS
                                  INTERFACE_LINK_LIBRARIES)
            get_target_property(value ${item} ${property})
            if(NOT value)
                continue()
            endif()
            if(property STREQUAL "INTERFACE_COMPILE_DEFINITIONS")
                list(APPEND definitions ${value})
            elseif(property STREQUAL "INTERFACE_COMPILE_OPTIONS")
                list(APPEND options ${value})
            else()
                list(APPEND queue ${value})
            endif()
        endforeach()
    endwhile()
    # Generator expressions that are false whatever the configuration, as vcpkg's exported
    # targets write their disabled options.
    foreach(variable IN ITEMS definitions options)
        list(FILTER ${variable} EXCLUDE REGEX "^\\$<\\$<BOOL:(OFF|FALSE|0|)>:")
        list(FILTER ${variable} EXCLUDE REGEX "^\\$<\\$<NOT:\\$<BOOL:ON>>:")
        list(REMOVE_DUPLICATES ${variable})
        list(SORT ${variable})
    endforeach()
    set(${out_definitions} "${definitions}" PARENT_SCOPE)
    set(${out_options} "${options}" PARENT_SCOPE)
endfunction()

function(_fv_pch_donor header_kind definitions options name out)
    if(NOT TARGET ${name})
        add_library(${name} STATIC "${_FV_MODULES_DIR}/pch/pch.cpp")
        target_precompile_headers(${name} PRIVATE "${_FV_MODULES_DIR}/pch/${header_kind}.h")
        fiorivoce_target_defaults(${name})
        target_compile_features(${name} PRIVATE cxx_std_23)
        target_compile_definitions(${name} PRIVATE ${definitions})
        target_compile_options(${name} PRIVATE ${options})
        # For the include paths of what the PCH holds. nlohmann-json defines nothing, and the Qt
        # component linked is one the reusing targets link, so its definitions are in the set.
        if(TARGET nlohmann_json::nlohmann_json)
            target_link_libraries(${name} PRIVATE nlohmann_json::nlohmann_json)
        endif()
        foreach(component IN ITEMS Widgets Gui Core)
            string(TOUPPER "QT_${component}_LIB" marker)
            if(marker IN_LIST definitions)
                target_link_libraries(${name} PRIVATE Qt6::${component})
                break()
            endif()
        endforeach()
        set_target_properties(${name} PROPERTIES FOLDER "pch")
    endif()
    set(${out} ${name} PARENT_SCOPE)
endfunction()

function(_fv_apply_pch)
    _fv_get(FV_PCH_TARGETS targets)
    set(keys "")
    foreach(target IN LISTS targets)
        if(NOT TARGET ${target})
            continue()
        endif()
        _fv_get(FV_PCH_KIND_${target} header_kind)
        _fv_usage_requirements(${target} definitions options)
        # Qt6::Test defines QT_TESTCASE_BUILDDIR and QT_TESTCASE_SOURCEDIR from the directories of
        # the target that links it, so no shared donor compiles with the same definitions (MSVC
        # C4651, an error under /WX). Such a target builds its own precompiled header.
        if("${definitions}" MATCHES "QT_TESTCASE_(BUILDDIR|SOURCEDIR)")
            target_precompile_headers(${target} PRIVATE "${_FV_MODULES_DIR}/pch/${header_kind}.h")
            continue()
        endif()
        string(SHA1 key "${header_kind}|${definitions}|${options}")
        _fv_get(FV_PCH_DONOR_${key} donor)
        if(NOT donor)
            string(SUBSTRING "${key}" 0 8 short)
            set(donor "fv_pch_${header_kind}_${short}")
            if(header_kind STREQUAL "core" AND NOT TARGET fv_pch_core)
                set(donor fv_pch_core)
            elseif(header_kind STREQUAL "qt" AND NOT TARGET fv_pch_qt)
                _fv_get(FV_MODULE_QT_${target} components)
                set(widget_set ${_FV_QT_DEFAULT_ui})
                list(SORT components)
                list(SORT widget_set)
                if(components STREQUAL widget_set)
                    set(donor fv_pch_qt)
                endif()
            endif()
            _fv_pch_donor(${header_kind} "${definitions}" "${options}" ${donor} donor)
            set_property(GLOBAL PROPERTY FV_PCH_DONOR_${key} ${donor})
        endif()
        target_precompile_headers(${target} REUSE_FROM ${donor})
    endforeach()
endfunction()

# Sources of the module in the current directory: the top level, detail/ and testing/, and win/
# on Windows or posix/ elsewhere.
function(_fv_glob_sources out_sources out_has_cpp)
    set(patterns "")
    foreach(sub IN ITEMS "" "detail/" "testing/")
        list(APPEND patterns "${sub}*.cpp" "${sub}*.h" "${sub}*.inc")
    endforeach()
    if(WIN32)
        list(APPEND patterns "win/*.cpp" "win/*.h")
    else()
        list(APPEND patterns "posix/*.cpp" "posix/*.h")
    endif()
    file(GLOB sources CONFIGURE_DEPENDS LIST_DIRECTORIES false
        RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" ${patterns})
    list(SORT sources)
    set(has_cpp FALSE)
    foreach(source IN LISTS sources)
        if(source MATCHES "\\.cpp$")
            set(has_cpp TRUE)
        endif()
    endforeach()
    set(${out_sources} "${sources}" PARENT_SCOPE)
    set(${out_has_cpp} ${has_cpp} PARENT_SCOPE)
endfunction()

function(fv_module)
    cmake_parse_arguments(PARSE_ARGV 0 arg "GUI;FEATURES" "KIND;NAME"
        "DEPS;THIRD_PARTY;PRIVATE_THIRD_PARTY;WIN32_LIBS;QT")
    if(arg_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "fv_module: unknown arguments ${arg_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT arg_KIND IN_LIST _FV_KINDS)
        message(FATAL_ERROR "fv_module: KIND must be one of ${_FV_KINDS}, not '${arg_KIND}'")
    endif()
    set(kind ${arg_KIND})
    get_filename_component(dir "${CMAKE_CURRENT_SOURCE_DIR}" NAME)
    get_filename_component(include_root "${CMAKE_CURRENT_SOURCE_DIR}" DIRECTORY)

    if(kind STREQUAL "exe")
        if(NOT arg_NAME)
            message(FATAL_ERROR "fv_module: an exe module needs NAME (src/${dir})")
        endif()
        set(target ${arg_NAME})
    else()
        if(arg_NAME OR arg_GUI OR arg_FEATURES)
            message(FATAL_ERROR "fv_module: NAME, GUI and FEATURES are for KIND exe (src/${dir})")
        endif()
        set(target fv_${dir})
    endif()

    _fv_get(FV_MODULE_DIR_${target} existing_dir)
    if(existing_dir)
        message(FATAL_ERROR "fv_module: ${target} is declared twice (${existing_dir} and "
                            "${CMAKE_CURRENT_SOURCE_DIR})")
    endif()

    # Qt components: the kind's set plus the module's named exceptions; an exe asks for its own.
    set(qt_components "")
    if(kind STREQUAL "exe")
        set(qt_components ${arg_QT})
        if(arg_GUI OR arg_FEATURES)
            list(APPEND qt_components Core Gui Widgets)
        endif()
    elseif(NOT kind STREQUAL "core")
        set(allowed ${_FV_QT_ALLOWED_${kind}} ${_FV_QT_EXTRA_${target}})
        foreach(component IN LISTS arg_QT)
            if(NOT component IN_LIST allowed)
                message(FATAL_ERROR "fv_module: a ${kind} module may not link Qt ${component} "
                                    "(${target}; allowed: ${allowed})")
            endif()
        endforeach()
        set(qt_components ${_FV_QT_DEFAULT_${kind}} ${arg_QT})
    elseif(arg_QT)
        message(FATAL_ERROR "fv_module: ${target} is a core module and may not link Qt")
    endif()
    list(REMOVE_DUPLICATES qt_components)

    set_property(GLOBAL APPEND PROPERTY FV_MODULES ${target})
    set_property(GLOBAL PROPERTY FV_MODULE_DIR_${target} "${CMAKE_CURRENT_SOURCE_DIR}")
    set_property(GLOBAL PROPERTY FV_MODULE_KIND_${target} ${kind})
    set_property(GLOBAL PROPERTY FV_MODULE_DEPS_${target} "${arg_DEPS}")
    set_property(GLOBAL PROPERTY FV_MODULE_QT_${target} "${qt_components}")
    set_property(GLOBAL PROPERTY FV_MODULE_THIRD_PARTY_${target}
        "${arg_THIRD_PARTY};${arg_PRIVATE_THIRD_PARTY}")

    _fv_qt_available(have_qt)
    if(qt_components AND NOT have_qt)
        set_property(GLOBAL APPEND PROPERTY FV_SKIPPED_MODULES ${target})
        message(STATUS "Skipping ${target}: Qt 6 was not found or FIORIVOCE_BUILD_APP is OFF")
        return()
    endif()

    _fv_glob_sources(sources has_cpp)

    if(kind STREQUAL "exe")
        if(arg_GUI)
            add_executable(${target} WIN32 ${sources})
        else()
            add_executable(${target} ${sources})
        endif()
        set(scope PUBLIC)
        set(private_scope PRIVATE)
    elseif(has_cpp)
        add_library(${target} STATIC ${sources})
        set(scope PUBLIC)
        set(private_scope PRIVATE)
    else()
        # Headers only: the contracts exist before their implementations.
        add_library(${target} INTERFACE)
        if(sources)
            target_sources(${target} PRIVATE ${sources})
        endif()
        set(scope INTERFACE)
        set(private_scope INTERFACE)
    endif()

    target_include_directories(${target} ${scope}
        "${include_root}" "${CMAKE_BINARY_DIR}/generated")
    target_compile_features(${target} ${scope} cxx_std_23)
    if(arg_DEPS)
        target_link_libraries(${target} ${scope} ${arg_DEPS})
    endif()
    if(arg_THIRD_PARTY)
        target_link_libraries(${target} ${scope} ${arg_THIRD_PARTY})
    endif()
    # A headers-only module has no sources for a private library to serve; it links them once
    # its first .cpp turns it into a static library.
    if(arg_PRIVATE_THIRD_PARTY AND NOT scope STREQUAL "INTERFACE")
        target_link_libraries(${target} ${private_scope} ${arg_PRIVATE_THIRD_PARTY})
    endif()
    if(WIN32 AND arg_WIN32_LIBS)
        target_link_libraries(${target} ${private_scope} ${arg_WIN32_LIBS})
    endif()
    foreach(component IN LISTS qt_components)
        target_link_libraries(${target} ${scope} Qt6::${component})
    endforeach()
    if(kind STREQUAL "exe" AND arg_FEATURES)
        target_link_libraries(${target} PRIVATE fv_feature_registry)
        set_property(GLOBAL APPEND PROPERTY FV_FEATURE_EXES ${target})
    endif()

    if(NOT scope STREQUAL "INTERFACE")
        fiorivoce_target_defaults(${target})
        if(qt_components)
            _fv_request_pch(${target} qt)
        else()
            _fv_request_pch(${target} core)
        endif()
        if(qt_components)
            set_target_properties(${target} PROPERTIES AUTOMOC ON)
            if(IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/resources")
                file(GLOB_RECURSE resources CONFIGURE_DEPENDS LIST_DIRECTORIES false
                    RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" "resources/*")
                if(resources)
                    qt_add_resources(${target} "${target}_resources"
                        PREFIX "/${dir}" BASE resources FILES ${resources})
                endif()
            endif()
        endif()
        if(kind STREQUAL "exe" AND qt_components)
            fv_add_qt_runtime()
            add_dependencies(${target} fv_qt_runtime)
        endif()
    elseif(IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/resources")
        message(FATAL_ERROR "fv_module: ${target} has resources/ but no .cpp to compile them into")
    endif()
endfunction()

# fv_feature(ID <id> CLASS <class> [HEADER <path>]): registers the module's Feature subclass with
# the generated registry. HEADER, relative to src/, defaults to <dir>/feature.h.
function(fv_feature)
    cmake_parse_arguments(PARSE_ARGV 0 arg "" "ID;CLASS;HEADER" "")
    if(arg_UNPARSED_ARGUMENTS OR NOT arg_ID OR NOT arg_CLASS)
        message(FATAL_ERROR "fv_feature: usage is fv_feature(ID <id> CLASS <class> [HEADER <h>])")
    endif()
    if(NOT arg_ID MATCHES "^[a-z][a-z0-9_]*$")
        message(FATAL_ERROR "fv_feature: the id '${arg_ID}' must be lower_snake_case")
    endif()
    get_filename_component(dir "${CMAKE_CURRENT_SOURCE_DIR}" NAME)
    set(target fv_${dir})
    _fv_get(FV_MODULE_KIND_${target} kind)
    if(NOT kind STREQUAL "feature")
        message(FATAL_ERROR "fv_feature: call it after fv_module(KIND feature ...) (src/${dir})")
    endif()
    _fv_get(FV_FEATURE_TARGET_${arg_ID} existing)
    if(existing)
        message(FATAL_ERROR "fv_feature: the id '${arg_ID}' is used by ${existing} and ${target}")
    endif()
    if(NOT arg_HEADER)
        set(arg_HEADER "${dir}/feature.h")
    endif()
    set_property(GLOBAL APPEND PROPERTY FV_FEATURES ${arg_ID})
    set_property(GLOBAL PROPERTY FV_FEATURE_TARGET_${arg_ID} ${target})
    set_property(GLOBAL PROPERTY FV_FEATURE_CLASS_${arg_ID} "${arg_CLASS}")
    set_property(GLOBAL PROPERTY FV_FEATURE_HEADER_${arg_ID} "${arg_HEADER}")
endfunction()

# fv_embed(<files>...): each data/<name>.json of the module becomes the header
# "<dir>/data/<name>.json.h" (in the build tree) declaring
#   namespace fv::embedded { inline constexpr std::string_view <dir>_<name>; }
# holding the file's bytes. The header is regenerated when the file changes.
function(fv_embed)
    if(NOT ARGN)
        message(FATAL_ERROR "fv_embed: name at least one file")
    endif()
    get_filename_component(dir "${CMAKE_CURRENT_SOURCE_DIR}" NAME)
    set(target fv_${dir})
    if(NOT TARGET ${target})
        _fv_get(FV_SKIPPED_MODULES skipped)
        if(target IN_LIST skipped)
            return()
        endif()
        message(FATAL_ERROR "fv_embed: call it after fv_module (src/${dir})")
    endif()
    set(outputs "")
    foreach(file IN LISTS ARGN)
        get_filename_component(input "${file}" ABSOLUTE)
        if(NOT EXISTS "${input}")
            message(FATAL_ERROR "fv_embed: ${input} does not exist")
        endif()
        get_filename_component(name "${file}" NAME)
        get_filename_component(stem "${file}" NAME_WE)
        string(MAKE_C_IDENTIFIER "${dir}_${stem}" symbol)
        set(output "${CMAKE_BINARY_DIR}/generated/${dir}/data/${name}.h")
        add_custom_command(OUTPUT "${output}"
            COMMAND "${CMAKE_COMMAND}" "-DINPUT=${input}" "-DOUTPUT=${output}"
                    "-DSYMBOL=${symbol}" -P "${_FV_MODULES_DIR}/FioriVoceEmbed.cmake"
            DEPENDS "${input}" "${_FV_MODULES_DIR}/FioriVoceEmbed.cmake"
            COMMENT "Embedding ${dir}/${file}"
            VERBATIM)
        list(APPEND outputs "${output}")
    endforeach()
    add_custom_target(${target}_embed DEPENDS ${outputs})
    add_dependencies(${target} ${target}_embed)
endfunction()

# fv_test(...) in tests/<dir>/CMakeLists.txt: builds fv_<dir>_tests from the directory's .cpp
# files, linked against fv_<dir> when that module exists, and registers every Catch2 test case
# with CTest under the labels <dir> and its kind. A test's kind is its Catch2 tag [decode],
# [platform], [golden], [widget] or [screenshot]; an untagged test is "unit".
function(fv_test)
    cmake_parse_arguments(PARSE_ARGV 0 arg "NO_MAIN" "" "DEPS;LABELS;QT")
    if(arg_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "fv_test: unknown arguments ${arg_UNPARSED_ARGUMENTS}")
    endif()
    get_filename_component(dir "${CMAKE_CURRENT_SOURCE_DIR}" NAME)
    set(target fv_${dir}_tests)

    _fv_get(FV_SKIPPED_MODULES skipped)
    _fv_qt_available(have_qt)
    foreach(dependency IN ITEMS fv_${dir} ${arg_DEPS})
        if(dependency IN_LIST skipped)
            message(STATUS "Skipping ${target}: ${dependency} is not built")
            return()
        endif()
    endforeach()
    if(arg_QT AND NOT have_qt)
        message(STATUS "Skipping ${target}: Qt 6 was not found or FIORIVOCE_BUILD_APP is OFF")
        return()
    endif()

    file(GLOB sources CONFIGURE_DEPENDS LIST_DIRECTORIES false
        RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" "*.cpp" "*.h")
    list(SORT sources)
    add_executable(${target} ${sources})
    # No test executable may show an assertion or abort dialog: the guard sends the message and
    # the call stack to the log and ends the test (lead amendment 24).
    set(no_dialogs "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/fv_test_no_dialogs.cpp")
    # Nor may one reach the user's real per-user folders, tools or keys: this guard gives the
    # process private ones before main() (lead amendment 25).
    set(isolation "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/fv_test_isolation.cpp")
    target_sources(${target} PRIVATE "${no_dialogs}" "${isolation}")
    set_source_files_properties("${no_dialogs}" "${isolation}" PROPERTIES
        SKIP_PRECOMPILE_HEADERS ON SKIP_AUTOMOC ON SKIP_UNITY_BUILD_INCLUSION ON)
    if(TARGET fv_${dir})
        target_link_libraries(${target} PRIVATE fv_${dir})
    endif()
    target_link_libraries(${target} PRIVATE ${arg_DEPS})
    if(arg_NO_MAIN)
        target_link_libraries(${target} PRIVATE Catch2::Catch2)
    else()
        target_link_libraries(${target} PRIVATE Catch2::Catch2WithMain)
    endif()
    target_compile_features(${target} PRIVATE cxx_std_23)

    # Qt components of the test itself and of everything it links, so the PCH donor's Qt set
    # matches the compile definitions the test ends up with.
    set(qt_components ${arg_QT})
    foreach(dependency IN ITEMS fv_${dir} ${arg_DEPS})
        _fv_get(FV_MODULE_QT_${dependency} dependency_qt)
        list(APPEND qt_components ${dependency_qt})
    endforeach()
    if(arg_QT)
        list(APPEND qt_components Test)
    endif()
    list(REMOVE_DUPLICATES qt_components)
    foreach(component IN LISTS qt_components)
        target_link_libraries(${target} PRIVATE Qt6::${component})
    endforeach()

    fiorivoce_target_defaults(${target})
    if(qt_components)
        set_target_properties(${target} PROPERTIES AUTOMOC ON)
        fv_add_qt_runtime()
        add_dependencies(${target} fv_qt_runtime)
    endif()
    if(qt_components)
        _fv_request_pch(${target} qt)
    else()
        _fv_request_pch(${target} core)
    endif()

    # Every test case gets its own data folder (architecture 9.1): ctest -j runs the cases of one
    # executable in parallel processes, which must not share settings or a database.
    set(data_root "${CMAKE_CURRENT_BINARY_DIR}/data")
    set(environment "")
    if(qt_components)
        list(APPEND environment "QT_QPA_PLATFORM=offscreen")
        if(WIN32)
            file(TO_CMAKE_PATH "$ENV{WINDIR}/Fonts" font_dir)
            list(APPEND environment "QT_QPA_FONTDIR=${font_dir}")
        endif()
    endif()

    # catch_discover_tests flattens PROPERTIES into one list, which splits list-valued properties
    # such as LABELS and ENVIRONMENT. So each kind's discovery names its own TEST_LIST, and a
    # script CTest includes after Catch2's sets the properties on those lists, and each test's
    # data folder, named by a hash of the test's name.
    set(properties_script "${CMAKE_CURRENT_BINARY_DIR}/${target}_properties.cmake")
    set(environment_suffix "")
    if(environment)
        list(JOIN environment ";" environment_suffix)
        set(environment_suffix ";${environment_suffix}")
    endif()
    set(script "")
    set(excluded "")
    foreach(kind IN ITEMS interactive screenshot widget golden decode platform unit)
        if(kind STREQUAL "unit")
            set(spec "${excluded}")
        else()
            # A test with two kind tags is registered once, under the first kind in this order.
            set(spec "[${kind}]${excluded}")
            string(APPEND excluded "~[${kind}]")
        endif()
        set(list_name "${target}_${kind}_TESTS")
        # Ported tests keep OpenWhisper's names, which repeat across directories: the prefix
        # keeps CTest names unique and lets `ctest -R "^<dir>\\."` select one directory.
        catch_discover_tests(${target} TEST_SPEC "${spec}" TEST_LIST ${list_name}
            TEST_PREFIX "${dir}.")
        set(labels ${dir} ${kind} ${arg_LABELS})
        string(CONFIGURE [=[
if(@list_name@)
    set_tests_properties(${@list_name@} PROPERTIES LABELS "@labels@")
    foreach(fv_test_name IN LISTS @list_name@)
        string(SHA1 fv_test_key "${fv_test_name}")
        string(SUBSTRING "${fv_test_key}" 0 16 fv_test_key)
        file(MAKE_DIRECTORY "@data_root@/${fv_test_key}")
        set_tests_properties("${fv_test_name}" PROPERTIES
            ENVIRONMENT "FIORIVOCE_DATA_DIR=@data_root@/${fv_test_key}@environment_suffix@")
    endforeach()
endif()
]=] block @ONLY)
        string(APPEND script "${block}")
    endforeach()
    file(CONFIGURE OUTPUT "${properties_script}" CONTENT "${script}" @ONLY)
    set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES "${properties_script}")
endfunction()

# Checks the declared layering (architecture 2.2) once every directory has been read, and writes
# the feature registry. Runs at the end of the top-level CMakeLists.txt.
function(_fv_finalize)
    _fv_get(FV_MODULES modules)
    _fv_get(FV_SKIPPED_MODULES skipped)
    set(errors "")
    foreach(module IN LISTS modules)
        _fv_get(FV_MODULE_KIND_${module} kind)
        _fv_get(FV_MODULE_DEPS_${module} deps)
        _fv_get(FV_MODULE_THIRD_PARTY_${module} third_party)
        set(allowed ${_FV_DEPS_ALLOWED_${kind}})
        foreach(dependency IN LISTS deps)
            _fv_get(FV_MODULE_KIND_${dependency} dependency_kind)
            if(NOT dependency_kind)
                if(NOT kind STREQUAL "exe")
                    string(APPEND errors "\n  ${module} (${kind}) depends on ${dependency}, "
                        "which is not a module; third-party targets go in THIRD_PARTY")
                endif()
            elseif(NOT dependency_kind IN_LIST allowed)
                string(APPEND errors "\n  ${module} (${kind}) may not depend on ${dependency} "
                                     "(${dependency_kind})")
            elseif(dependency IN_LIST skipped AND NOT module IN_LIST skipped)
                string(APPEND errors "\n  ${module} depends on ${dependency}, which is not built")
            endif()
        endforeach()
        # A module named among the third-party libraries would bypass the kind rules above.
        foreach(link IN LISTS third_party)
            _fv_get(FV_MODULE_KIND_${link} link_kind)
            if(link_kind)
                string(APPEND errors "\n  ${module} names the module ${link} as a third-party "
                                     "library; modules go in DEPS")
            endif()
        endforeach()
        if(kind STREQUAL "ui" OR kind STREQUAL "feature")
            list(REMOVE_ITEM third_party "")
            if(third_party)
                list(JOIN third_party ", " third_party_joined)
                string(APPEND errors "\n  ${module} (${kind}) may not link third-party libraries "
                                     "(${third_party_joined})")
            endif()
        endif()
        if(kind STREQUAL "core")
            set(links ${third_party})
            if(TARGET ${module})
                get_target_property(direct ${module} LINK_LIBRARIES)
                get_target_property(interface ${module} INTERFACE_LINK_LIBRARIES)
                list(APPEND links ${direct} ${interface})
            endif()
            foreach(link IN LISTS links)
                if(link MATCHES "^Qt[0-9]*::")
                    string(APPEND errors "\n  ${module} is a core module and links ${link}")
                endif()
            endforeach()
        endif()
    endforeach()
    if(errors)
        # One line per violation, each built with string(APPEND): list(APPEND) would make every
        # argument its own line.
        message(FATAL_ERROR "Module layering (architecture 2.2) is violated:${errors}")
    endif()

    _fv_write_feature_registry()
    _fv_apply_pch()
endfunction()

function(_fv_write_feature_registry)
    _fv_get(FV_FEATURES ids)
    _fv_get(FV_FEATURE_EXES exes)
    if(NOT ids AND NOT exes)
        return()
    endif()
    if(NOT TARGET fv_appkit)
        if(exes)
            message(FATAL_ERROR "${exes} link the feature registry, which needs fv_appkit")
        endif()
        return()
    endif()
    list(SORT ids)
    set(includes "")
    set(entries "")
    set(links "")
    foreach(id IN LISTS ids)
        _fv_get(FV_FEATURE_TARGET_${id} target)
        _fv_get(FV_FEATURE_CLASS_${id} class)
        _fv_get(FV_FEATURE_HEADER_${id} header)
        if(NOT TARGET ${target})
            continue()
        endif()
        string(APPEND includes "#include \"${header}\"\n")
        string(APPEND entries "    FeatureFactory{\"${id}\", &make<${class}>},\n")
        list(APPEND links ${target})
    endforeach()
    list(LENGTH links count)
    set(output "${CMAKE_BINARY_DIR}/generated/fv_feature_registry/registry.cpp")
    file(GENERATE OUTPUT "${output}" CONTENT
"// Generated by cmake/FioriVoceModules.cmake from the fv_feature() calls. Do not edit.
#include <array>
#include <memory>
#include <span>

#include \"appkit/feature.h\"
${includes}
namespace fv {
namespace {

template <class T>
std::unique_ptr<Feature> make() {
    return std::make_unique<T>();
}

// Sorted by id; FeatureRegistry orders construction by required_features().
const std::array<FeatureFactory, ${count}> kFeatures{{
${entries}}};

}  // namespace

std::span<const FeatureFactory> registered_features() {
    return kFeatures;
}

}  // namespace fv
")
    add_library(fv_feature_registry STATIC "${output}")
    target_link_libraries(fv_feature_registry PUBLIC fv_appkit ${links})
    fiorivoce_target_defaults(fv_feature_registry)
endfunction()

get_property(_fv_finalize_scheduled GLOBAL PROPERTY FV_FINALIZE_SCHEDULED)
if(NOT _fv_finalize_scheduled)
    set_property(GLOBAL PROPERTY FV_FINALIZE_SCHEDULED TRUE)
    cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL _fv_finalize)
endif()
