"""Check portable boundaries, documented public APIs, capability inventory and vendor file hashes."""

from pathlib import Path
import hashlib
import json
import re

ROOT = Path(__file__).resolve().parents[1]


def check():
    """Raise on a violated architecture contract; this is not a full C static analyzer."""
    count = 0
    for folder in ["src", "include", "components"]:
        for path in (ROOT / folder).rglob("*"):
            if path.suffix not in {".c", ".h"}:
                continue
            source = path.read_text(encoding="utf8")
            assert not re.search(r'#\s*include\s*[<"](?:esp_|freertos/|driver/|soc/)', source), path
            assert "../firmware" not in source, path
            count += 1
    api_count = 0
    for path in (ROOT / "include/q2").glob("*.h"):
        source = path.read_text(encoding="utf8")
        for match in re.finditer(r"^q2_result_t\s+(q2_\w+)\s*\(", source, re.M):
            prefix = source[: match.start()].rstrip()
            assert prefix.endswith("*/"), f"Missing public API comment: {match[1]}"
            api_count += 1
        # Every public function declaration in this profile has a unified result.
        assert not re.search(r"^(?:void|bool|int|size_t)\s+q2_\w+\s*\(", source, re.M), path
    inventory = json.loads((ROOT / "ai/capabilities.json").read_text(encoding="utf8"))
    declared = {
        (component["module"], method, component["capability"])
        for component in inventory["components"]
        for method in component["methods"]
    }
    actual = set()
    for path in (ROOT / "components").rglob("*.c"):
        source = path.read_text(encoding="utf8")
        actual.update(
            re.findall(r'\{\s*"([a-z_]+)",\s*"([a-z_]+)",\s*"([a-z_.]+)",\s*[A-Za-z_]\w*\s*\}', source)
        )
    assert declared == actual, f"Capability drift: {declared ^ actual}"
    lock = json.loads((ROOT / "third_party/files.sha256.json").read_text(encoding="utf8"))
    for name, digest in lock.items():
        assert hashlib.sha256((ROOT / "third_party" / name).read_bytes()).hexdigest() == digest, name
    installed = {
        p.relative_to(ROOT / "third_party").as_posix()
        for name in ["lua", "cjson", "littlefs", "fatfs", "sqlite", "tinyusb", "lvgl"]
        for p in (ROOT / "third_party" / name).rglob("*")
        if p.is_file()
    }
    assert installed == set(lock), "Vendor file added/removed without lock update"
    fonts = ROOT / "assets/fonts"
    font_lock = json.loads((fonts / "manifest.json").read_text(encoding="utf8"))
    assert hashlib.sha256((fonts / "q2-cjk-regular.ttf").read_bytes()).hexdigest() == font_lock["sha256"]
    assert hashlib.sha256((fonts / "OFL.txt").read_bytes()).hexdigest() == font_lock["license_sha256"]
    print(
        f"PASS: {count} portable files, {api_count} documented APIs, {len(actual)} component methods, "
        f"{len(lock)} vendor checksums"
    )


if __name__ == "__main__":
    check()
