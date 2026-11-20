"""Curated inputs for services/component_catalog.py (M2.5): the NVIDIA speech-runtime rows, as
the dataclass's fields. FioriVoce rewrites the fields that describe OpenWhisper's worker process
and portable Python (tests/models/test_component_details.cpp lists them)."""

import dataclasses


def _details(module, component_id):
    return dataclasses.asdict(module.get_component_details(component_id))


WRAPPERS = {"get_component_details": _details}

CASES = {
    "get_component_details": [
        ["asr-nvidia-cpu"],
        ["asr-nvidia-cuda"],
        ["asr-nvidia-vulkan"],
        ["asr-nvidia-metal"],
        ["not-a-component"],
    ],
}
