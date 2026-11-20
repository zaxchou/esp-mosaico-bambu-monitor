"""Build the CJK font subset for bambu_monitor (run once, output committed).

Produces:
  ui/fonts/DengBmb.ttf   DengXian subset: ASCII + GB2312 level-1 hanzi + extras
  ui/charset_cjk.txt     the same corpus, referenced by dynamic labels

Task names arrive from the printer at runtime, so the corpus covers the
common 3,755 GB2312 level-1 hanzi; that keeps one 16px atlas under ~1.2 MB,
which fits the 1.5 MiB ui_apps partition next to the small UI.
"""
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
UI = ROOT / "ui"
SRC_FONT = "C:/Windows/Fonts/Deng.ttf"

chars = {chr(c) for c in range(32, 127)}
# GB2312 level-1 (rows 0xB0..0xD7) — the common 3,755 hanzi.
for row in range(0xB0, 0xD8):
    for col in range(0xA1, 0xFF):
        try:
            chars.add(bytes([row, col]).decode("gb2312"))
        except UnicodeDecodeError:
            pass
# UI copy and units that fall outside the level-1 block.
chars.update("°·—…℃！？：（）、。，；×÷")

corpus = "".join(sorted(chars))
(UI / "charset_cjk.txt").write_text(corpus, encoding="utf-8")

with tempfile.NamedTemporaryFile("w", encoding="utf-8", suffix=".txt",
                                 delete=False) as fp:
    fp.write(corpus)
    charset_path = fp.name
try:
    subprocess.run(
        [sys.executable, "-m", "fontTools.subset", SRC_FONT,
         "--text-file=" + charset_path, "--output-file=" + str(UI / "fonts/DengBmb.ttf"),
         "--layout-features=", "--no-hinting", "--desubroutinize"],
        check=True)
finally:
    import os
    os.unlink(charset_path)
print("DengBmb.ttf:", (UI / "fonts/DengBmb.ttf").stat().st_size, "bytes")
print("corpus chars:", len(corpus))
