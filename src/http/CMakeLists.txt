fv_module(KIND core
          DEPS fv_base
          PRIVATE_THIRD_PARTY CURL::libcurl
          WIN32_LIBS advapi32)

# The User-Agent carries the project version; engine/version.h has it too, but fv_http may not
# depend on fv_engine.
configure_file(version.h.in "${CMAKE_BINARY_DIR}/generated/http/version.h" @ONLY)
