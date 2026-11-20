# ONNX Runtime C API headers

`include/` holds the C API headers of [ONNX Runtime](https://github.com/microsoft/onnxruntime)
1.29.0, the version OpenWhisper pins, copied unchanged from the tag `v1.29.0`, folder
`include/onnxruntime/core/session/`. They are MIT licensed; see `LICENSE`, copied from the same
tag.

| File | SHA-256 as published |
|---|---|
| `include/onnxruntime_c_api.h` | `acc0cf4b3f28d39339c76770d76164bb7a0637dc89f5fde764b4017b632f6743` |
| `include/onnxruntime_error_code.h` | `5ce3b054e798eced8d14f5b86e98692fd33470463f96194ce0700a2d53dd8721` |
| `include/onnxruntime_ep_c_api.h` | `e6c986c9e98583f8113b2c6bc3864814883b806d501cf24da4d239c45753e235` |
| `LICENSE` | `2f07c72751aed99790b8a4869cf2311df85a860b22ded05fa22803587a48922c` |

`onnxruntime_c_api.h` is the header FioriVoce uses (`ORT_API_VERSION` 29). The other two are
there only because it includes them.

Only headers are vendored. `onnxruntime.dll` is loaded at run time by its full path, never
linked and never unloaded, so the build needs no binaries. CMake target: `onnxruntime_headers`.

When moving to another runtime version, replace all three headers and `LICENSE` with the ones
from that release's tag and update the table. The API is append-only by version, so a newer
header still describes an older runtime as long as the code asks for an API version that the
runtime has.
