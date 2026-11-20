"""Report the explicit tolerances of reference screenshots."""
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def render(root: Path) -> str:
    lines = ["## Screenshot tolerances", "", "| Reference | Maximum differing pixels | Maximum channel delta | Reason |",
             "|---|---:|---:|---|"]
    for path in sorted((root / "tests/reference").rglob("*.tolerance.json")):
        entry = json.loads(path.read_text(encoding="utf-8"))
        reason = entry["reason"].replace("|", "\\|").replace("\n", " ")
        lines.append(f"| {path.relative_to(root).as_posix()} | {entry['max_differing_pixels']} "
                     f"| {entry['max_channel_delta']} | {reason} |")
    return "\n".join(lines) + "\n"


if __name__ == "__main__":
    print(render(ROOT), end="")
