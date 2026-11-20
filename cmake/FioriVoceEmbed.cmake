# cmake -DINPUT=<file> -DOUTPUT=<header> -DSYMBOL=<name> -P FioriVoceEmbed.cmake
# Writes a header holding INPUT's bytes as fv::embedded::<SYMBOL>, a constexpr std::string_view.
# The bytes are char literals rather than one string literal: MSVC caps a string literal at
# 64 KiB, and a char literal never narrows whatever the signedness of char.
foreach(variable IN ITEMS INPUT OUTPUT SYMBOL)
    if(NOT DEFINED ${variable})
        message(FATAL_ERROR "FioriVoceEmbed.cmake needs -D${variable}=...")
    endif()
endforeach()

file(READ "${INPUT}" hex HEX)
string(LENGTH "${hex}" hex_length)
math(EXPR size "${hex_length} / 2")

# 24 bytes per line keeps the lines short.
string(REPEAT "[0-9a-f][0-9a-f]" 24 line_pattern)
string(REGEX REPLACE "${line_pattern}" "\\0@" hex "${hex}")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "'\\\\x\\1'," bytes "${hex}")
string(REPLACE "@" "\n    " bytes "${bytes}")

get_filename_component(input_name "${INPUT}" NAME)
file(CONFIGURE OUTPUT "${OUTPUT}" @ONLY CONTENT
"// Generated from @input_name@ by cmake/FioriVoceEmbed.cmake. Do not edit.
#pragma once

#include <string_view>

namespace fv::embedded {

inline constexpr char @SYMBOL@_bytes[] = {
    @bytes@ '\\0'};

inline constexpr std::string_view @SYMBOL@{@SYMBOL@_bytes, @size@};

}  // namespace fv::embedded
")
