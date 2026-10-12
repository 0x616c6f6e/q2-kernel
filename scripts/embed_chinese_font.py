"""Embed the checked-in font without network access or nonstandard build dependencies."""

import hashlib
import json
from pathlib import Path
import sys


def embed(source, output):
    """Validate the asset then generate a read-only C array in the build directory."""
    metadata = json.loads((source.parent / "manifest.json").read_text(encoding="utf8"))
    data = source.read_bytes()
    if hashlib.sha256(data).hexdigest() != metadata["sha256"]:
        raise ValueError("CJK font checksum mismatch")
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("w", encoding="ascii", newline="\n") as stream:
        stream.write("/* Generated from Q2 CJK Regular; SIL OFL 1.1, see assets/fonts/OFL.txt. */\n")
        stream.write("#include <stddef.h>\nconst unsigned char q2_chinese_ttf[] = {\n")
        for offset in range(0, len(data), 24):
            stream.write("    " + ",".join(str(byte) for byte in data[offset : offset + 24]) + ",\n")
        stream.write("};\nconst size_t q2_chinese_ttf_size = sizeof(q2_chinese_ttf);\n")


if __name__ == "__main__":
    embed(Path(sys.argv[1]), Path(sys.argv[2]))
