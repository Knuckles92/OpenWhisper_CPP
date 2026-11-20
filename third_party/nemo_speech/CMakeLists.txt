# Headers only. FioriVoce loads the NeMo-Speech.cpp runtime at run time (see
# src/engine/nemo_api.h), so nothing here is linked.
add_library(nemo_speech_headers INTERFACE)
target_include_directories(nemo_speech_headers SYSTEM INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")
