"""Curated inputs for services/gpu_info.py (M1.11): NvidiaGpu.short_name and format_gb."""


def _short_name(module, name):
    return module.NvidiaGpu(name, 0).short_name


CASES = {
    "short_name": [
        ["NVIDIA GeForce GTX 1050 Ti"],
        ["NVIDIA GeForce RTX 2060"],
        ["NVIDIA RTX A4000"],
        ["Quadro RTX 4000"],
        ["NVIDIA Quadro P2000"],
        ["GeForce GTX 980"],
        ["GeForce NVIDIA GTX"],
        ["Tesla T4"],
        ["NVIDIA "],
        ["NVIDIA GeForce "],
        [""],
        ["NVIDIA TITAN V"],
    ],
    "format_gb": [[0], [1], [512], [1024], [4096], [6144], [6195], [11264], [24576], [1536],
                  [1075], [1126]],
}

WRAPPERS = {"short_name": _short_name}
