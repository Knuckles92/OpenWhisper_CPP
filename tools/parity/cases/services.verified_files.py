"""Curated inputs for services/verified_files.py (M1.7).

`free_space_shortfall` runs with shutil.disk_usage reporting the given free bytes for a scratch
folder. `free_space_message` is the text components._check_free_space raises for a shortfall.
"""

import collections
import shutil

import ow_isolation

_Usage = collections.namedtuple("_Usage", "total used free")


def _free_space_shortfall(module, required, free, margin):
    folder = ow_isolation.SCRATCH / "verified-files-cases" / "space"
    original = module.shutil.disk_usage
    module.shutil.disk_usage = lambda path: _Usage(free * 2, free, free)
    try:
        if margin is None:
            return module.free_space_shortfall(str(folder), required)
        return module.free_space_shortfall(str(folder), required, margin)
    finally:
        module.shutil.disk_usage = original
        shutil.rmtree(folder.parent, ignore_errors=True)


def _free_space_message(module, needed, free):
    from services import components

    original = components.free_space_shortfall
    components.free_space_shortfall = lambda path, required: (needed, free)
    try:
        components._check_free_space(1)
    except components.ComponentError as exc:
        return str(exc)
    finally:
        components.free_space_shortfall = original
    return None


CASES = {
    "free_space_shortfall": [
        [0, 0, None],
        [-5, 0, None],
        [1, 0, None],
        [1, 1, None],
        [100, 113, None],
        [100, 114, None],
        [100, 115, None],
        [1000, 1149, None],
        [1000, 1150, None],
        [7, 8, None],
        [7, 9, None],
        [1_500_000_000, 1_000_000_000, None],
        [1_500_000_000, 1_725_000_000, None],
        [640_000_000, 735_999_999, None],
        [640_000_000, 736_000_000, None],
        [100, 100, 1.0],
        [100, 99, 1.0],
        [100, 199, 2.0],
        [100, 200, 2.0],
        [3, 3, 1.5],
        [3, 4, 1.5],
        [10_000_000_000, 11_499_999_999, None],
        [10_000_000_000, 11_500_000_000, None],
    ],
    "free_space_message": [
        [114, 113],
        [1_725_000_000, 1_000_000_000],
        [736_000_000, 1_000],
        [2_000, 999],
        [999_999_999, 500_000],
        [11_500_000_000, 0],
    ],
}

WRAPPERS = {
    "free_space_shortfall": _free_space_shortfall,
    "free_space_message": _free_space_message,
}

PINS = {
    "free_space_note": "free_space_shortfall(<scratch folder>, required[, margin]) with "
                       "shutil.disk_usage(...).free = the second argument; free_space_message "
                       "is components._check_free_space's ComponentError text for a "
                       "(needed, free) shortfall",
}
