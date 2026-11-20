# A project that reads the modules under FIXTURE_SRC with FioriVoce's real cmake/ files, so the
# build system's own rules can be tested on small trees (run_fixture.cmake drives it).
cmake_minimum_required(VERSION 3.25...3.31)
project(fv_fixture LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
list(APPEND CMAKE_MODULE_PATH "${FV_CMAKE_DIR}")

if(FAKE_QT)
    # Enough for headers-only modules: fv_module only needs the targets to exist.
    foreach(component IN ITEMS Core Gui Widgets Svg SvgWidgets Network Test)
        add_library(Qt6::${component} INTERFACE IMPORTED)
    endforeach()
else()
    find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets Svg SvgWidgets)
endif()

include(FioriVoceModules)

file(GLOB module_lists "${FIXTURE_SRC}/*/CMakeLists.txt")
list(SORT module_lists)
foreach(list_file IN LISTS module_lists)
    get_filename_component(module_dir "${list_file}" DIRECTORY)
    get_filename_component(module_name "${module_dir}" NAME)
    add_subdirectory("${module_dir}" "${CMAKE_BINARY_DIR}/src/${module_name}")
endforeach()
