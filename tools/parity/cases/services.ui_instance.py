"""Curated inputs for services.ui_instance._data_key (fv_ui_shell instanceKey): the first 16 hex
digits of the SHA-256 of normcase(realpath(abspath(data_dir))). A relative path resolves against
the working directory, so its wrapper runs in the isolation scratch folder and returns, instead
of a key that would name the checkout's own folder, what the replay can check anywhere: the path
relative to the working directory that the folder normalized to, and whether its key is the key
of the same folder spelled from the working directory."""

import os


def _relative(module, relative):
    import ow_isolation

    previous = os.getcwd()
    os.chdir(ow_isolation.SCRATCH)
    try:
        here = os.getcwd()
        resolved = os.path.normcase(os.path.realpath(os.path.abspath(relative)))
        base = os.path.normcase(os.path.realpath(here))
        absolute_key = module._data_key(os.path.join(here, relative))
        return [os.path.relpath(resolved, base), module._data_key(relative) == absolute_key]
    finally:
        os.chdir(previous)


WRAPPERS = {"_data_key_relative": _relative}

CASES = {
    "_data_key": [
        ["C:\\Data"],
        ["c:/data/"],
        ["C:\\Data\\"],
        ["C:\\DATA"],
        ["C:\\Users\\Some One\\AppData\\Local\\FioriVoce"],
        ["C:\\Users\\Zoë\\AppData\\Local\\Ünïcödé Daten"],
        ["C:\\Data\\sub\\..\\other"],
    ],
    "_data_key_relative": [
        ["fv-data"],
        ["nested\\data dir"],
        ["Mixed/Case\\..\\Folder"],
    ],
}
