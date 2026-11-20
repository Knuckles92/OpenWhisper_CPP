# cmake -DFIXTURE=<fixture dir> -DBUILD=<build dir> -DMODE=<configure-fails|run>
#       -DEXPECT=<regex> -DFV_CMAKE_DIR=<cmake/> -DFV_REAL_SRC=<src/> -DGENERATOR=<generator>
#       -DCXX=<compiler> [-DMAKE_PROGRAM=<ninja>] [-DQT6_DIR=<dir>] -P run_fixture.cmake
#
# configure-fails: configuring the fixture must fail with output matching EXPECT.
# run: the fixture must configure, build (Debug) and its fixture_app must print EXPECT.
foreach(variable IN ITEMS FIXTURE BUILD MODE EXPECT FV_CMAKE_DIR GENERATOR CXX)
    if(NOT DEFINED ${variable})
        message(FATAL_ERROR "run_fixture.cmake needs -D${variable}=...")
    endif()
endforeach()

file(REMOVE_RECURSE "${BUILD}")
set(arguments
    -S "${CMAKE_CURRENT_LIST_DIR}/project" -B "${BUILD}" -G "${GENERATOR}"
    "-DCMAKE_CXX_COMPILER=${CXX}" "-DFV_CMAKE_DIR=${FV_CMAKE_DIR}"
    "-DFIXTURE_SRC=${FIXTURE}/src" "-DFV_REAL_SRC=${FV_REAL_SRC}")
if(MAKE_PROGRAM)
    list(APPEND arguments "-DCMAKE_MAKE_PROGRAM=${MAKE_PROGRAM}")
endif()
if(MODE STREQUAL "configure-fails")
    list(APPEND arguments -DFAKE_QT=ON)
elseif(QT6_DIR)
    list(APPEND arguments "-DQt6_DIR=${QT6_DIR}")
endif()

execute_process(COMMAND "${CMAKE_COMMAND}" ${arguments}
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
set(all "${output}\n${errors}")

if(MODE STREQUAL "configure-fails")
    if(result EQUAL 0)
        message(FATAL_ERROR "Configuring ${FIXTURE} succeeded but should have failed:\n${all}")
    endif()
    if(NOT all MATCHES "${EXPECT}")
        message(FATAL_ERROR "Configuring ${FIXTURE} failed without '${EXPECT}':\n${all}")
    endif()
    message(STATUS "Configure failed as expected: ${EXPECT}")
    return()
endif()

if(NOT result EQUAL 0)
    message(FATAL_ERROR "Configuring ${FIXTURE} failed:\n${all}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${BUILD}" --config Debug
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Building ${FIXTURE} failed:\n${output}\n${errors}")
endif()
file(GLOB_RECURSE programs "${BUILD}/bin/fixture_app" "${BUILD}/bin/fixture_app.exe")
if(NOT programs)
    message(FATAL_ERROR "No fixture_app under ${BUILD}/bin")
endif()
list(GET programs 0 program)
execute_process(COMMAND "${program}" RESULT_VARIABLE result OUTPUT_VARIABLE output)
if(NOT result EQUAL 0 OR NOT output MATCHES "${EXPECT}")
    message(FATAL_ERROR "fixture_app exited ${result} and printed '${output}', "
                        "expected '${EXPECT}'")
endif()
message(STATUS "fixture_app printed: ${output}")
