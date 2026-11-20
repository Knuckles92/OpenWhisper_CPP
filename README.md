# OpenWhisper_CPP

OpenWhisper_CPP (FioriVoce) is an experimental C++ and Qt port of the
[OpenWhisper](https://github.com/Knuckles92/OpenWhisper) desktop app. It explores
the full OpenWhisper workflow, from dictation and file transcription to meetings
and walkthroughs. The goal is to find out where a native build might improve
startup time, responsiveness, or package size, and whether people want to use
and help shape a C++ version.

## Status

This is a source preview. Windows is the primary desktop target. The port
supports microphone dictation, file transcription, global hotkeys, history,
walkthrough capture, meeting transcription and review, and remote speech
hosts. Local transcription uses NVIDIA Parakeet via
[NeMo-Speech.cpp](https://github.com/NVIDIA/NeMo-Speech.cpp). The runtime and
model weights are distributed separately.

The original Python OpenWhisper remains the behavior reference. The
[parity report](docs/port/parity-report.md) and
[documented differences](docs/port/differences.md) show where the port stands.
Linux builds are available from source, with desktop integration still in
progress. See [open items](docs/port/open-items.md) for platform status and
release qualification.

If you try it, tell us which workflows matter to you and where you notice a
difference from the original. For performance or size comparisons, include
your platform, hardware, model, and whether the package includes the runtime
and model weights. [Share feedback on GitHub](https://github.com/Knuckles92/OpenWhisper_CPP/issues).

## Build

You need CMake 3.25 or later and a C++23 compiler (Visual Studio 2022 or later,
GCC 13+, or Clang 17+). The speech runtime is loaded when the program runs, not
linked, so the engine, the CLI and their tests need nothing else. The tests
fetch Catch2 when CMake configures.

```bash
cmake --preset default
```

```bash
cmake --build --preset release
```

```bash
ctest --preset release
```

That builds the engine, `fiorivoce-cli` and their tests into `build/bin`
(`build/bin/RelWithDebInfo` with Visual Studio).

The rest of the tree (every other library, the desktop app and its tests) needs
the third-party libraries in `vcpkg.json` and, for the app, Qt 6.8 or later
(Widgets, Svg, Network). Set the `Qt6_ROOT` environment variable to the Qt
folder, such as `D:\Qt\6.10.3\msvc2022_64`, then build with the vcpkg presets:

```bash
cmake --workflow --preset windows-debug
```

[BUILDING.md](BUILDING.md) covers machine setup, vcpkg, other presets, and the
module layout.

## Run

Local transcription uses two components that are distributed separately.

- **A NeMo-Speech.cpp release**, unpacked. Get the archive for your platform
  from the [v0.1.0 release](https://github.com/NVIDIA/NeMo-Speech.cpp/releases/tag/v0.1.0);
  the CUDA build runs on CPU as well.
- **A Parakeet model** in GGUF form, such as `parakeet-tdt-0.6b-v3.q8_0.gguf`
  from [nvidia/parakeet-tdt-0.6b-v3](https://huggingface.co/nvidia/parakeet-tdt-0.6b-v3).

If OpenWhisper is installed with Parakeet, you already have both. On Windows
they are in `%LOCALAPPDATA%\OpenWhisper\components\asr-nvidia-cuda` (or
`asr-nvidia-cpu`) and `%LOCALAPPDATA%\OpenWhisper\speech-models\parakeet-v3`.

```bash
fiorivoce-cli transcribe recording.wav --runtime <runtime folder> --model <model.gguf>
```

Add `--gpu 0` to decode on the first GPU, `--timestamps` for one timed segment
per line, and `--language en` to skip language detection. `FIORIVOCE_RUNTIME_DIR`
and `FIORIVOCE_MODEL` stand in for `--runtime` and `--model`. The transcript goes
to standard output; timing and the runtime's own log go to standard error.

The input is a WAV file (PCM or float, any sample rate or channel count).

In the desktop app, open Settings to choose the voice model and compute device.
Use Record for dictation, Upload for existing audio, or Meeting Mode for a meeting.

### AI cleanup for dictation

In **Settings > AI cleanup**, open the model provider dropdown and choose **Installed agent…**
to see whether Claude Code, Codex, and OpenCode are installed and signed in, and select one.
Apply the runner, confirm the named destination, and enable AI cleanup. To return to an
endpoint, select **API / local model** and apply it.
While enabled, live dictation you start on this computer can send transcripts and cleanup
instructions through the chosen CLI to its signed-in AI provider, including when you use a
cleanup profile. You can choose one of the models the CLI lists or use the CLI's default.
Turning off cleanup cancels an active cleanup request. Selecting a model endpoint clears the agent approval.
If the CLI configuration requires a private provider that the cleanup session cannot preserve,
FioriVoce keeps the original dictation.

## Tests

Most tests need nothing. The ones that decode real audio are skipped unless
these are set:

| Variable | Value |
| --- | --- |
| `FIORIVOCE_TEST_RUNTIME_DIR` | An unpacked NeMo-Speech.cpp release |
| `FIORIVOCE_TEST_MODEL` | A Parakeet `.gguf` file |
| `FIORIVOCE_TEST_WAV` | A recording with speech in it |

Three more are optional, each read-only and each enabling a few more tests:

| Variable | Value |
| --- | --- |
| `FIORIVOCE_TEST_PDFIUM` | A `pdfium.dll` (pypdfium2's, for example): PDF text and metadata tests |
| `FIORIVOCE_TEST_ONNXRUNTIME` | An `onnxruntime.dll` of ONNX Runtime 1.29: speaker embedding tests |
| `FIORIVOCE_TEST_SPEAKER_MODEL` | The WeSpeaker model `voxceleb_resnet34_LM.onnx`: the same tests |

## Layout

| Path | Contents |
| --- | --- |
| `src/engine` | The speech engine. Plain C++, no Qt. |
| `src/cli` | `fiorivoce-cli`. |
| `src/app` | `fiorivoce`, the Qt 6 Widgets desktop app. |
| `tests` | Catch2 tests for the core modules, desktop features and release tools. CTest selects the offscreen Qt platform. |
| `third_party/nemo_speech` | The NeMo-Speech.cpp C header (Apache-2.0). |
| `cmake` | CMake helpers. |

## License

[MIT](LICENSE). See the [third-party notices](THIRD_PARTY_NOTICES.md) for
bundled components and assets.
