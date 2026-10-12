"""Prepare the pinned LXGW WenKai Lite Regular asset (fonttools==4.60.1)."""

import argparse
import hashlib
import json
from pathlib import Path

import fontTools
from fontTools import subset
from fontTools.ttLib import TTFont

ROOT = Path(__file__).resolve().parents[1]
VERSION = "v1.522"
COMMIT = "067a6d508532cc0f8bc79e899ec3b06ff4dee38a"
SOURCE_URL = (
    f"https://raw.githubusercontent.com/lxgw/LxgwWenKai-Lite/{COMMIT}/"
    "fonts/TTF/LXGWWenKaiLite-Regular.ttf"
)
LICENSE_URL = f"https://raw.githubusercontent.com/lxgw/LxgwWenKai-Lite/{COMMIT}/OFL.txt"
SOURCE_SHA256 = "140c99ba4e28e817cec49bf82a0c5fcdc4fe633fb9dfda16d0ee8d59a8545f15"
LICENSE_SHA256 = "c38b1994a5e48ac30ac7d1da7d0409fd8fd8127dfe28a13d6e787d5b1ef34a5e"


def prepare(source, license_path):
    """Preserve every upstream Unicode mapping and remove unused shaping/hint data."""
    if fontTools.__version__ != "4.60.1":
        raise ValueError("Regeneration requires fonttools==4.60.1")
    if hashlib.sha256(source.read_bytes()).hexdigest() != SOURCE_SHA256:
        raise ValueError("Wrong upstream font")
    license_data = license_path.read_bytes()
    if hashlib.sha256(license_data).hexdigest() != LICENSE_SHA256:
        raise ValueError("Wrong upstream license")
    font = TTFont(source, recalcTimestamp=False)
    codepoints = set(font.getBestCmap())
    options = subset.Options()
    options.layout_features = []
    options.hinting = False
    worker = subset.Subsetter(options=options)
    # Song names are dynamic: do not restrict the font to today's UI strings or BMP.
    worker.populate(unicodes=codepoints)
    worker.subset(font)
    if set(font.getBestCmap()) != codepoints:
        raise ValueError("Unicode coverage changed")
    # Use a distinct derivative name; preserve upstream copyright/license records.
    for record in font["name"].names:
        if record.nameID in (1, 3, 4, 6, 16, 17):
            text = "Regular" if record.nameID == 17 else "Q2 CJK Regular"
            record.string = text.encode(record.getEncoding())
    output = ROOT / "assets/fonts/q2-cjk-regular.ttf"
    font.save(output)
    font.close()
    data = output.read_bytes()
    (output.parent / "OFL.txt").write_bytes(license_data)
    manifest = {
        "source_family": "LXGW WenKai Lite Regular",
        "source_version": VERSION,
        "source_commit": COMMIT,
        "source_url": SOURCE_URL,
        "source_sha256": SOURCE_SHA256,
        "license": "OFL-1.1",
        "license_url": LICENSE_URL,
        "license_sha256": LICENSE_SHA256,
        "fonttools": fontTools.__version__,
        "weight": 400,
        "coverage": "All upstream Unicode cmap entries; no common-character/BMP filter",
        "codepoints": len(codepoints),
        "bytes": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
    }
    (output.parent / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf8")
    print(f"Generated {len(codepoints)} codepoints, {len(data)} bytes")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("--license", dest="license_path", type=Path, required=True)
    args = parser.parse_args()
    prepare(args.source, args.license_path)
