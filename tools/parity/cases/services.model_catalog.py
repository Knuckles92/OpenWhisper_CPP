"""Curated inputs for services/model_catalog.py (M2.5): every built-in speech model's profile and
format_download_mb. get_model_details returns the dataclass's fields with download_size and
compact_tags beside them; FioriVoce describes the Parakeet models only."""

import dataclasses


def _details(module, name):
    details = module.get_model_details(name)
    record = dataclasses.asdict(details)
    record["download_size"] = details.download_size
    record["compact_tags"] = details.compact_tags
    return record


WRAPPERS = {"get_model_details": _details}

CASES = {
    "get_model_details": [["parakeet-v3"], ["orukeet-v0.1"], ["auto"], ["unknown-model"]],
    "format_download_mb": [
        [0], [0.4], [0.5], [1.5], [2.5], [76], [145], [714], [714.456], [999], [999.4], [999.5],
        [1000], [1049], [1050], [1149.99], [1510], [1530], [1620], [3090], [12345],
    ],
}
