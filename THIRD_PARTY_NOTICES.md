# Third-party notices

This port draws on [OpenWhisper](https://github.com/Knuckles92/OpenWhisper),
Copyright (c) 2025 Knuckles92, under the [MIT License](LICENSE).

| Component | License and attribution |
| --- | --- |
| NeMo-Speech.cpp API header | Apache-2.0. See the [license](third_party/nemo_speech/LICENSE) and [notice](third_party/nemo_speech/NOTICE). |
| ONNX Runtime API headers | [MIT](third_party/onnxruntime/LICENSE). |
| Tabler icons | [MIT](src/ui_theme/resources/assets/tabler/LICENSE.txt). |
| Qt | See the [Qt packaging notices](tools/release/notices/01-qt.md) and [license texts](packaging/windows/qt-licenses/README.md). |
| Meeting dashboard | See the acknowledgments below and the [dashboard component list](tools/release/notices/04-dashboard.md). |
| Word template parts from python-docx | [MIT](tools/release/notices/05-python-docx.md). |

The dashboard bundle includes React, React DOM and Scheduler, copyright (c)
Facebook, Inc. and its affiliates, and a Vite helper, copyright (c) 2019-present,
VoidZero Inc. and Vite contributors. These components use the MIT terms reproduced
in [LICENSE](LICENSE) with their respective copyright notices above. The
[DM Sans](webui/dist/licenses/DM-Sans-OFL.txt) and
[Instrument Serif](webui/dist/licenses/Instrument-Serif-OFL.txt) font licenses
are included with the dashboard.

Speech runtimes and model weights are separately distributed under their own
licenses. See the [speech notices](tools/release/notices/03-speech.md). The
Windows packaging tools collect the licenses of dependencies used in a build.
