# Links every third-party library of vcpkg.json and makes one real call into each, so a port that
# builds but cannot be used fails here rather than in the package that first needs it.
set(third_party
    nlohmann_json::nlohmann_json
    PCRE2::8BIT
    unofficial::sqlite3::sqlite3
    CURL::libcurl
    Boost::asio
    Boost::beast
    OpenSSL::SSL
    OpenSSL::Crypto
    portaudio
    FFmpeg::avformat
    FFmpeg::avcodec
    FFmpeg::swresample
    pocketfft::pocketfft
    MINIZIP::minizip-ng
    ZLIB::ZLIB
    pugixml::pugixml)
if(WIN32)
    list(APPEND third_party WIL::WIL)
endif()

fv_test(DEPS ${third_party})

# PCRE2's native API is chosen per translation unit; Asio wants the Windows version it targets.
target_compile_definitions(fv_deps_tests PRIVATE PCRE2_CODE_UNIT_WIDTH=8
    $<$<PLATFORM_ID:Windows>:_WIN32_WINNT=0x0A00>)
