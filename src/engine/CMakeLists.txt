# The speech engine: Parakeet through the NeMo-Speech.cpp C ABI. No Qt here, so
# the CLI and the tests build with a compiler alone. The runtime is loaded by
# path, so only its header is used.
fv_module(KIND core
          PRIVATE_THIRD_PARTY nemo_speech_headers ${CMAKE_DL_LIBS})

configure_file(version.h.in "${CMAKE_BINARY_DIR}/generated/engine/version.h" @ONLY)

# The name src/app and src/cli link by.
add_library(FioriVoce::engine ALIAS fv_engine)
