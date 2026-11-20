# NeMo-Speech.cpp headers

`include/nemo_speech/asr.h` is the C ABI header from the
[NVIDIA NeMo-Speech.cpp](https://github.com/NVIDIA/NeMo-Speech.cpp) v0.1.0
release, copied unchanged. It is Apache-2.0 licensed; see `LICENSE` and
`NOTICE`.

Only the header is vendored. The runtime libraries are loaded at run time from
a NeMo-Speech.cpp release the user has installed, so the build needs no
binaries.

When moving to a newer runtime, replace the header with the one from that
release's `include/nemo_speech/` folder. The v1 ABI is append-only, so a newer
header still describes older runtimes; `src/engine/nemo_api.h` lists the
functions OpenWhisper_CPP binds.
