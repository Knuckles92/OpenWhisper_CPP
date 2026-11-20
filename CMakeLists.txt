cmake_minimum_required(VERSION 3.25...3.31)

# The product version. A release build passes -DFIORIVOCE_VERSION=<major.minor.patch> (the
# packaging script does); without it the tree builds as the development version below.
set(FIORIVOCE_VERSION "0.1.0" CACHE STRING "Product version, as major.minor.patch")
if(NOT FIORIVOCE_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR
        "FIORIVOCE_VERSION must be major.minor.patch, not '${FIORIVOCE_VERSION}'")
endif()

project(FioriVoce
    VERSION ${FIORIVOCE_VERSION}
    DESCRIPTION "Cross-platform dictation and meeting transcription on Parakeet"
    LANGUAGES C CXX)

# C++23 for std::expected, which the interfaces of architecture section 3 return.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# No C++20 modules in the tree, so no dependency scanning step per source.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

# Every executable lands in one folder, so the tests and the app find each
# other's DLLs on Windows without PATH edits.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# vcpkg's libraries are static; Qt and the system's own libraries are shared. An ELF executable
# exports every symbol that a shared library it loads refers to, so the parts of a static zlib
# (or OpenSSL) linked into it stand in for the system's copy, function by function: Qt's PNG
# reader then set a stream up with one zlib and reset it with the other, and crashed in free()
# the first time an icon was read. Keeping archive symbols out of the dynamic symbol table
# leaves every library with its own copy.
if(UNIX AND NOT APPLE)
    add_link_options("LINKER:--exclude-libs,ALL")
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(FIORIVOCE_BUILD_TESTS "Build the unit tests" ${PROJECT_IS_TOP_LEVEL})
set(FIORIVOCE_BUILD_APP "AUTO" CACHE STRING
    "Build the Qt modules and the desktop app: ON, OFF, or AUTO (only when Qt 6.8+ is found)")
set_property(CACHE FIORIVOCE_BUILD_APP PROPERTY STRINGS AUTO ON OFF)
# Without vcpkg the third-party libraries are missing, so only the engine, the CLI and their
# tests are built (the default preset).
if(DEFINED VCPKG_TOOLCHAIN)
    set(engine_only_default OFF)
else()
    set(engine_only_default ON)
endif()
option(FIORIVOCE_ENGINE_ONLY "Build only src/engine, src/cli and tests/engine (no vcpkg needed)"
    ${engine_only_default})
option(FIORIVOCE_REQUIRE_LINT "Fail configure when Python 3.12 for the lint tests is missing" OFF)
option(FIORIVOCE_SCREENSHOT_TESTS "Run the reference-screenshot comparisons" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(FioriVoceWarnings)
include(FioriVoceDeps)
include(FioriVoceModules)

if(FIORIVOCE_BUILD_TESTS)
    enable_testing()
endif()

add_subdirectory(third_party/nemo_speech)
add_subdirectory(third_party/onnxruntime)

# Every src/<dir> and tests/<dir> with a CMakeLists.txt, in sorted order. CONFIGURE_DEPENDS makes
# the build re-run CMake when a directory appears or disappears.
if(FIORIVOCE_ENGINE_ONLY)
    set(module_lists "${CMAKE_CURRENT_SOURCE_DIR}/src/engine/CMakeLists.txt"
                     "${CMAKE_CURRENT_SOURCE_DIR}/src/cli/CMakeLists.txt")
    set(test_lists "${CMAKE_CURRENT_SOURCE_DIR}/tests/engine/CMakeLists.txt")
else()
    file(GLOB module_lists CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*/CMakeLists.txt")
    file(GLOB test_lists CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/*/CMakeLists.txt")
endif()
list(SORT module_lists)
list(SORT test_lists)
# tests/support defines the test libraries, and the Qt and skipped-module properties that
# fv_test reads for every other test directory, so it is configured first.
set(support_list "${CMAKE_CURRENT_SOURCE_DIR}/tests/support/CMakeLists.txt")
if(support_list IN_LIST test_lists)
    list(REMOVE_ITEM test_lists "${support_list}")
    list(PREPEND test_lists "${support_list}")
endif()

foreach(list_file IN LISTS module_lists)
    get_filename_component(module_dir "${list_file}" DIRECTORY)
    get_filename_component(module_name "${module_dir}" NAME)
    # src/app is fiorivoce.exe, a Qt executable: it is skipped without Qt.
    if(module_name STREQUAL "app" AND NOT FIORIVOCE_HAVE_QT)
        message(STATUS "Skipping src/app: Qt 6 was not found or FIORIVOCE_BUILD_APP is OFF")
        continue()
    endif()
    add_subdirectory("${module_dir}")
endforeach()

# The meeting dashboard (M7, lead decision 1). webui/dist is committed and never built here; it
# is copied to webui/dist beside the executables, where the dashboard server looks for it (the
# folder of the running executable, which the test programs share). packaging/windows/stage.ps1
# copies it into the install folder.
if(NOT FIORIVOCE_ENGINE_ONLY AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/webui/dist/index.html")
    get_property(multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    if(multi_config)
        set(webui_dist_out "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>/webui/dist")
    else()
        set(webui_dist_out "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/webui/dist")
    endif()
    file(GLOB_RECURSE webui_dist_files CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/webui/dist/*")
    # The old copy goes first, so a file the bundle no longer has is not served. The copy keeps
    # the files' times, so index.html is touched to be newer than every input.
    add_custom_command(OUTPUT "${webui_dist_out}/index.html"
        COMMAND "${CMAKE_COMMAND}" -E rm -rf "${webui_dist_out}"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/webui/dist" "${webui_dist_out}"
        COMMAND "${CMAKE_COMMAND}" -E touch "${webui_dist_out}/index.html"
        DEPENDS ${webui_dist_files}
        COMMENT "Copying the meeting dashboard bundle to bin/webui/dist"
        VERBATIM)
    add_custom_target(fv_webui_dist ALL DEPENDS "${webui_dist_out}/index.html")
    if(TARGET fiorivoce)
        add_dependencies(fiorivoce fv_webui_dist)
    endif()
endif()

if(FIORIVOCE_BUILD_TESTS)
    foreach(list_file IN LISTS test_lists)
        get_filename_component(test_dir "${list_file}" DIRECTORY)
        add_subdirectory("${test_dir}")
    endforeach()
    # Tests that belong to no single module (tests/CMakeLists.txt) and the lints.
    add_subdirectory(tests)
    if(NOT FIORIVOCE_ENGINE_ONLY)
        add_subdirectory(tools/lint)
    endif()
endif()
