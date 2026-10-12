"""Standalone host build. Pass a C compiler explicitly; no ESP-IDF or sibling project required."""

from pathlib import Path
import argparse
import hashlib
import json
import os
import subprocess

ROOT = Path(__file__).resolve().parents[1]
EXCLUDED = {"lua.c", "luac.c", "linit.c", "liolib.c", "loslib.c", "loadlib.c", "ldblib.c", "lcorolib.c"}


def sources():
    """Return first-party and pinned vendor translation units without SDK paths."""
    own = sorted(ROOT.glob("src/**/*.c")) + sorted(ROOT.glob("components/**/*.c"))
    # Optional LVGL has a separate real-rendering suite in CMake; the minimal core stays independent.
    own = [p for p in own if p != ROOT / "components/display/lvgl.c"]
    vendor = sorted(p for p in (ROOT / "third_party/lua").glob("*.c") if p.name not in EXCLUDED)
    vendor += [
        ROOT / p
        for p in [
            "third_party/cjson/cJSON.c",
            "third_party/littlefs/lfs.c",
            "third_party/littlefs/lfs_util.c",
            "third_party/fatfs/ff.c",
            "third_party/fatfs/ffunicode.c",
            "third_party/sqlite/sqlite3.c",
            "third_party/tinyusb/src/tusb.c",
            "third_party/tinyusb/src/common/tusb_fifo.c",
            "third_party/tinyusb/src/device/usbd.c",
            "third_party/tinyusb/src/device/usbd_control.c",
            "third_party/tinyusb/src/class/cdc/cdc_device.c",
        ]
    ]
    return own, vendor


def build(compiler, extra_sources=None, output=None):
    """Build the integration executable and return its path and compiler environment."""
    out = ROOT / "build/host"
    out.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env["ZIG_GLOBAL_CACHE_DIR"] = str(out / "zig-cache")
    env["ZIG_LOCAL_CACHE_DIR"] = str(out / "zig-local")
    include = [
        "include",
        "src/core",
        "third_party/lua",
        "third_party/cjson",
        "third_party/littlefs",
        "third_party/fatfs",
        "third_party/sqlite",
        "third_party/tinyusb/src",
        "components/usb",
        "components/storage",
    ]
    definitions = [
        "LUAI_MAXCCALLS=32",
        "LUAI_MAXSTACK=2048",
        "CJSON_NESTING_LIMIT=16",
        "LFS_NO_DEBUG",
        "LFS_NO_WARN",
        "LFS_NO_ERROR",
        "SQLITE_OS_OTHER=1",
        "SQLITE_THREADSAFE=0",
        "SQLITE_OMIT_LOAD_EXTENSION=1",
        "SQLITE_TEMP_STORE=3",
        "SQLITE_DEFAULT_MEMSTATUS=0",
    ]
    base = compiler + ["-std=c11", "-O1", "-g", "-fsanitize=undefined"]
    base += ["-I" + str(ROOT / p) for p in include] + ["-D" + d for d in definitions]
    own, vendor = sources()
    own += list(extra_sources or [ROOT / "tests/test_kernel.c", ROOT / "tests/fake_dcd.c"])
    objects = []
    for i, path in enumerate(own + vendor):
        obj = out / (hashlib.sha256(str(path).encode()).hexdigest()[:16] + ".o")
        stamp = obj.with_suffix(".json")
        flags = ["-Wall", "-Wextra", "-Werror"] if path in own else ["-w"]
        cmd = base + flags + ["-c", str(path), "-o", str(obj)]
        # Headers are part of the cache key; vendor code is never formatted or locally rewritten by this builder.
        digest = hashlib.sha256(path.read_bytes() + json.dumps(cmd).encode() + HEADER_HASH).hexdigest()
        if not obj.exists() or not stamp.exists() or stamp.read_text() != digest:
            subprocess.run(cmd, check=True, env=env)
            stamp.write_text(digest)
        objects.append(str(obj))
    exe = Path(output) if output else out / ("q2_tests.exe" if os.name == "nt" else "q2_tests")
    exe.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        compiler + ["-fsanitize=undefined", *objects, "-o", str(exe)] + ([] if os.name == "nt" else ["-lm"]),
        check=True,
        env=env,
    )
    return exe, env


HEADER_HASH = hashlib.sha256(
    b"".join(p.read_bytes() for p in sorted(ROOT.rglob("*.h")) if "build" not in p.relative_to(ROOT).parts)
).digest()
if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--cc", default=os.environ.get("Q2_CC", "cc"), help="Compiler executable path (e.g. clang or zig)"
    )
    args = parser.parse_args()
    compiler = [args.cc] + (["cc"] if Path(args.cc).stem == "zig" else [])
    exe, env = build(compiler)
    result = subprocess.run([str(exe)], env=env, capture_output=True, text=True, timeout=90)
    print(result.stdout, end="")
    if result.stderr:
        print(result.stderr, end="")
    result.check_returncode()
    (ROOT / "build/host/verification.txt").write_text(result.stdout, encoding="utf8")
