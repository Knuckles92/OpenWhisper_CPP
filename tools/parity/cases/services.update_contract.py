"""Curated inputs for services/update_contract.py (M3.2, update/update_contract.h).

setup_asset_name carries OpenWhisper's prefix; the C++ replay compares it rebranded (the release
identity's setup_prefix is FioriVoce's).
"""

VERSIONS = [
    "2.1.1", "v2.1.1", "V2.1.1", "  v2.1.1  ", "v 2.1.1", "vv2.1.1", "2.6.01", "v2.6.01",
    "2.5.2-rc.1", "2.5.3", "v2.5.3", "2.5", "2.5.3.1", "", "   ", "v", "x2.1.1", "2.1.1 ", " 2.1.1　",
    "٢.٩.٠", "02.003.0004", "2.1.1\n", "2.1.1\nx", "1_0.2.3", "+1.2.3", "-1.2.3",
    "2.1.a",
]

CASES = {
    "normalize_version": [[v] for v in VERSIONS],
    "parse_strict_version": [[v] for v in VERSIONS],
    "is_newer_version": [
        ["2.6.09", "2.6.10"],
        ["2.6.10", "2.6.09"],
        ["2.1.1", "v2.1.1"],
        ["2.1.1", "2.2.0"],
        ["2.2.0", "2.1.1"],
        ["2.6.01", "2.6.1"],
        ["0.1.0", "0.1.1"],
        ["0.1.0", "1.0.0"],
        ["1.0.0", "0.9.99"],
        ["2.5.2", "2.5.3-rc.1"],
        ["2.5.2-rc.1", "2.5.3"],
    ],
    "setup_asset_name": [[v] for v in ("2.1.1", "v2.6.01", " V0.1.1 ", "2.9.0-rc.1", "")],
}
