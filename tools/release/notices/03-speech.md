## Speech runtime and models (not part of the setup)

The setup does not contain a speech runtime or a speech model. OpenWhisper_CPP loads a runtime and a
model from files you install or download, at run time and by path. Their licences are not
relicensed by OpenWhisper_CPP; read the notices that come with each download before you redistribute
it.

| Artifact | Upstream project / model card | Licence |
| --- | --- | --- |
| Speech runtime | [NVIDIA NeMo-Speech.cpp](https://github.com/NVIDIA/NeMo-Speech.cpp) | Apache-2.0; bundled third-party components retain their own notices, kept in the runtime archive under `nemo-speech/share/licenses` (the Linux CUDA archive includes NVIDIA's CUDA runtime and cuBLAS libraries, under NVIDIA's terms) |
| Parakeet TDT 0.6B v3 weights | [NVIDIA Parakeet TDT 0.6B v3](https://huggingface.co/nvidia/parakeet-tdt-0.6b-v3) | CC-BY-4.0 |
| Orukeet v0.1 weights | [Oruk AI Orukeet](https://huggingface.co/oruk/orukeet), adapted from NVIDIA Parakeet; [weight licence and attribution](https://huggingface.co/oruk/orukeet/blob/main/NOTICE.md) | CC-BY-SA-4.0; retain the attribution and the ShareAlike terms when you redistribute the weights |

Attribution (CC-BY-4.0): "Parakeet TDT 0.6B v3" is by NVIDIA Corporation, licensed under
Creative Commons Attribution 4.0 International
(<https://creativecommons.org/licenses/by/4.0/>). OpenWhisper_CPP downloads the GGUF form of the
weights, quantized to 8 bits, and uses them for inference without retraining. NVIDIA does not
endorse OpenWhisper_CPP.

The exact versions, URLs, sizes and SHA-256 digests of what OpenWhisper_CPP downloads are in its model
and component catalog. Pins establish the identity of an artifact, not a security certification.
