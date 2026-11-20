# Every find_package of the tree. Modules name the targets this file provides; they never call
# find_package themselves (architecture 10.1). The ports come from vcpkg.json.
include_guard(GLOBAL)

# Catch2: vcpkg's when there is one, else fetched, so the engine-only build needs no vcpkg.
include(FetchContent)
FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.16.1
    GIT_SHALLOW TRUE
    FIND_PACKAGE_ARGS 3)
FetchContent_MakeAvailable(Catch2)
if(Catch2_SOURCE_DIR)
    list(APPEND CMAKE_MODULE_PATH "${Catch2_SOURCE_DIR}/extras")
endif()
include(Catch)

find_package(Threads REQUIRED)

if(NOT FIORIVOCE_ENGINE_ONLY)
    find_package(nlohmann_json CONFIG REQUIRED)
    find_package(pcre2 CONFIG REQUIRED COMPONENTS 8BIT)
    find_package(unofficial-sqlite3 CONFIG REQUIRED)
    find_package(CURL REQUIRED)
    find_package(OpenSSL REQUIRED)
    find_package(boost_asio CONFIG REQUIRED)
    find_package(boost_beast CONFIG REQUIRED)
    find_package(portaudio CONFIG REQUIRED)
    # The Linux static triplet exports only portaudio_static. Keep the package's ALSA and
    # threading link dependencies on the imported target used by fv_audio.
    if(NOT TARGET portaudio AND TARGET portaudio_static)
        add_library(portaudio ALIAS portaudio_static)
    endif()
    find_package(minizip-ng CONFIG REQUIRED)
    find_package(ZLIB REQUIRED)
    find_package(pugixml CONFIG REQUIRED)
    if(WIN32)
        find_package(wil CONFIG REQUIRED)
    endif()

    # FFmpeg: vcpkg's FindFFMPEG gives variables, not targets. One imported target per library,
    # each a real library target with a location: as bare paths on an interface target the
    # static libraries of the Linux triplet reached the link line in an order the linker could
    # not resolve (libavcodec.a before libavformat.a, which needs it).
    find_package(FFMPEG REQUIRED)
    foreach(library IN ITEMS avformat avcodec swresample avutil)
        if(NOT TARGET FFmpeg::${library})
            set(release "${FFMPEG_lib${library}_LIBRARY_RELEASE}")
            set(debug "${FFMPEG_lib${library}_LIBRARY_DEBUG}")
            if(NOT release AND NOT debug)
                message(FATAL_ERROR "FFmpeg was found without lib${library}")
            endif()
            if(NOT release)
                set(release "${debug}")
            endif()
            add_library(FFmpeg::${library} UNKNOWN IMPORTED)
            set_target_properties(FFmpeg::${library} PROPERTIES
                IMPORTED_LOCATION "${release}"
                IMPORTED_LINK_INTERFACE_LANGUAGES "C")
            if(debug)
                set_target_properties(FFmpeg::${library} PROPERTIES
                    IMPORTED_LOCATION_DEBUG "${debug}")
            endif()
            target_include_directories(FFmpeg::${library} INTERFACE ${FFMPEG_INCLUDE_DIRS})
        endif()
    endforeach()
    target_link_libraries(FFmpeg::avformat INTERFACE FFmpeg::avcodec FFmpeg::avutil)
    target_link_libraries(FFmpeg::avcodec INTERFACE FFmpeg::avutil)
    target_link_libraries(FFmpeg::swresample INTERFACE FFmpeg::avutil)

    # pocketfft is one header with no CMake package.
    find_path(POCKETFFT_INCLUDE_DIR pocketfft_hdronly.h REQUIRED)
    if(NOT TARGET pocketfft::pocketfft)
        add_library(pocketfft::pocketfft INTERFACE IMPORTED)
        target_include_directories(pocketfft::pocketfft INTERFACE "${POCKETFFT_INCLUDE_DIR}")
    endif()
endif()

# Qt 6.8 LTS is the floor: QStyleHints::setColorScheme needs 6.8, and the windows11 style the
# reference screenshots pin needs 6.7.
set(FIORIVOCE_HAVE_QT OFF)
if(NOT FIORIVOCE_ENGINE_ONLY AND NOT FIORIVOCE_BUILD_APP STREQUAL "OFF")
    set(qt_components Core Gui Widgets Svg SvgWidgets Network Test)
    if(FIORIVOCE_BUILD_APP STREQUAL "AUTO")
        find_package(Qt6 6.8 QUIET COMPONENTS ${qt_components})
    else()
        find_package(Qt6 6.8 REQUIRED COMPONENTS ${qt_components})
    endif()
    if(Qt6_FOUND)
        set(FIORIVOCE_HAVE_QT ON)
    else()
        message(STATUS "Qt 6.8 or later not found: skipping the desktop app and every Qt module. "
                       "Point Qt6_ROOT or CMAKE_PREFIX_PATH at a Qt 6 install to build them.")
    endif()
endif()

# The lints and the build-system tests are Python 3.12 standard library scripts.
if(FIORIVOCE_REQUIRE_LINT)
    find_package(Python3 3.12 REQUIRED COMPONENTS Interpreter)
else()
    find_package(Python3 3.12 QUIET COMPONENTS Interpreter)
endif()

# sccache is optional (nothing depends on it); it caches /Z7 objects only.
if(NOT CMAKE_CXX_COMPILER_LAUNCHER)
    find_program(FIORIVOCE_SCCACHE sccache)
    if(FIORIVOCE_SCCACHE)
        set(CMAKE_CXX_COMPILER_LAUNCHER "${FIORIVOCE_SCCACHE}")
        set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT Embedded)
    endif()
endif()
