"""Render UTF-8 labels/buttons at both UI sizes using the real x86/Lua/LVGL runner."""

import argparse
import json
from pathlib import Path
import subprocess
import tempfile
from PIL import Image, ImageChops

SCRIPT = """local q = require("q2")
return {on_start=function()
    assert(q.ui.background(0x101c19))
    local title = assert(q.ui.label(24, 18, "中文字体 / Chinese"))
    assert(q.ui.style(title, 20, 0xf0aa70))
    assert(q.ui.label(24, 60, "简体中文、繁體中文，标点：《音乐》"))
    local chars = {"中", "文", "音", "乐", "繁", "體", "龘"}
    for i=1,#chars do
        local small = assert(q.ui.label(24+(i-1)*48, 108, chars[i]))
        assert(q.ui.style(small, 20, 0xf1e8d5))
        local big = assert(q.ui.label(24+(i-1)*48, 152, chars[i]))
        assert(q.ui.style(big, 40, 0xf1e8d5))
    end
    assert(q.ui.label(24, 224, "歌名：晴天  ·  歌手：周杰伦"))
    assert(q.ui.label(24, 260, "歌词：晚风吹过街道，音乐陪我慢慢回家"))
    assert(q.ui.button(24, 308, "系统设置", 180))
    assert(q.ui.button(230, 308, "返回主页", 180))
end}
"""


def check(runner, output):
    """Capture actual glyphs and reject identical tofu rectangles across Chinese letters."""
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="q2-chinese-") as directory:
        work = Path(directory)
        manifest = {
            "format": 2,
            "id": "2d7f3e18-b368-43e7-9698-eb29015fc1f8",
            "name": "Chinese font test",
            "version": "1.0.0",
            "entry": "main.lua",
            "type": "app",
            "api": 2,
            "capabilities": {"required": ["display.ui"], "optional": []},
            "resources": {"lua_heap_kib": 64},
        }
        (work / "manifest.json").write_text(json.dumps(manifest), encoding="utf8")
        (work / "main.lua").write_text(SCRIPT, encoding="utf8")
        result = subprocess.run(
            [
                str(runner),
                str(work / "manifest.json"),
                str(work / "main.lua"),
                "--headless",
                "--frames",
                "12",
                "--width",
                "502",
                "--height",
                "410",
                "--grant",
                "display.ui",
                "--capture",
                str(work / "frame.bmp"),
            ],
            capture_output=True,
            text=True,
            timeout=20,
        )
        assert result.returncode == 0 and "failed result=" not in result.stdout, result.stdout + result.stderr
        frame = Image.open(work / "frame.bmp").convert("RGB")
        # The product runner reserves a 32-pixel status bar. Glyph boxes remain inside their row.
        for y, height in [(140, 26), (184, 49)]:
            shapes = []
            for index in range(7):
                x = 24 + index * 48
                tile = frame.crop((x, y, x + 44, y + height))
                background = Image.new("RGB", tile.size, (16, 28, 25))
                assert ImageChops.difference(tile, background).getbbox(), "Invisible glyph"
                shapes.append(tile.tobytes())
            assert len(set(shapes)) == 7, "Chinese glyphs collapsed to identical placeholders"
        frame.save(output / "chinese-font.png")
        (output / "chinese-font.log").write_text(result.stdout + result.stderr, encoding="utf8")
        print("PASS: UTF-8 labels/buttons, mixed scripts, distinct 20/40px Chinese glyphs")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runner", required=True, type=Path)
    parser.add_argument("--artifacts", required=True, type=Path)
    args = parser.parse_args()
    check(args.runner.resolve(), args.artifacts.resolve())
