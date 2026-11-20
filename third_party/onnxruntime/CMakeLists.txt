# Headers only. OpenWhisper_CPP loads onnxruntime.dll at run time by its full path (src/diarize), so
# nothing here is linked.
add_library(onnxruntime_headers INTERFACE)
target_include_directories(onnxruntime_headers SYSTEM INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")
