"""Consistency tests of the keyboard icon table (runtime/src/hle/input/prime_key_icons.cpp).

Always run: structure, naming rules of Aurora's texture replacement files, action ids against the
key binding table, agreement with recomp/config/RM3E01_00/keyboard_icon_textures.json, XXH64
vectors. Only with the extracted disc (set PRIME_ICONS_FILES, or pass --disc): every table entry is
recomputed from the game's own TEXR data and must produce exactly the same file name.

    python -I recomp/tools/prime_icons/test_icon_table.py [--disc]
"""
import json
import os
import re
import struct
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(HERE))
RUNTIME = ROOT / "recomp/third_party/WiiCompiled/runtime"
ICON_CPP = RUNTIME / "src/hle/input/prime_key_icons.cpp"
KPAD_CPP = RUNTIME / "src/hle/input/kpad.cpp"
JSON_TABLE = ROOT / "recomp/config/RM3E01_00/keyboard_icon_textures.json"
FILE_RE = re.compile(r"^tex1_(\d+)x(\d+)_([0-9a-f]{16})(?:_([0-9a-f]{16}))?_(\d+)$")
GX_FORMATS = {0, 1, 2, 3, 4, 5, 6, 8, 9, 10, 14}  # I4 I8 IA4 IA8 RGB565 RGB5A3 RGBA8 C4 C8 CMPR (+ C14X2 slot)
FIXED_LAYOUTS = {"stick", "dpad"}


def parse_table():
    text = ICON_CPP.read_text(encoding="utf-8")
    rows = []
    for m in re.finditer(r'TextureSpec\{"([^"]+)",\s*(\d+),\s*(\d+),\s*(nullptr|"[^"]*"),\s*(nullptr|"[^"]*"),\s*(nullptr|"[^"]*"),\s*"([^"]*)"\}', text):
        rows.append({"file": m.group(1), "w": int(m.group(2)), "h": int(m.group(3)),
                     "a": None if m.group(4) == "nullptr" else m.group(4).strip('"'),
                     "b": None if m.group(5) == "nullptr" else m.group(5).strip('"'),
                     "fixed": None if m.group(6) == "nullptr" else m.group(6).strip('"'),
                     "note": m.group(7)})
    return rows


def action_ids():
    text = KPAD_CPP.read_text(encoding="utf-8")
    return set(re.findall(r'PrimeActionDefinition\{\{"([a-z0-9_]+)",', text))


class IconTable(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.rows = parse_table()

    def test_table_is_not_empty_and_parsed_completely(self):
        declared = ICON_CPP.read_text(encoding="utf-8").count("TextureSpec{\"tex1_")
        self.assertGreaterEqual(len(self.rows), 40)
        self.assertEqual(len(self.rows), declared, "a TextureSpec line did not match the parser (formatting changed?)")

    def test_names_follow_aurora_replacement_rules(self):
        for r in self.rows:
            m = FILE_RE.match(r["file"])
            self.assertIsNotNone(m, r["file"])
            self.assertEqual((int(m.group(1)), int(m.group(2))), (r["w"], r["h"]), r["file"])
            self.assertIn(int(m.group(5)), GX_FORMATS, r["file"])
            palette = int(m.group(5)) in (8, 9)
            self.assertEqual(m.group(4) is not None, palette, f"{r['file']}: palette formats carry a TLUT hash, others do not")

    def test_names_are_unique(self):
        names = [r["file"] for r in self.rows]
        self.assertEqual(len(names), len(set(names)))

    def test_every_row_says_what_to_draw(self):
        ids = action_ids()
        for r in self.rows:
            self.assertTrue(r["a"] or (r["fixed"] in FIXED_LAYOUTS), r["file"])
            for key in ("a", "b"):
                if r[key]:
                    self.assertIn(r[key], ids, f"{r['file']}: unknown action id {r[key]!r}")

    def test_every_button_the_game_shows_has_a_cap(self):
        used = {r["a"] for r in self.rows if r["a"]}
        for needed in ("a", "b", "z", "c", "plus", "minus", "one", "two", "stick_up", "dpad_down", "shake"):
            self.assertIn(needed, used, f"no texture draws the {needed} key cap")

    def test_default_config_json_knows_every_texture(self):
        known = {t["file"].removesuffix(".png") for t in json.loads(JSON_TABLE.read_text(encoding="utf-8"))}
        missing = [r["file"] for r in self.rows if r["file"] not in known]
        self.assertEqual(missing, [], "textures in the C++ table but not in keyboard_icon_textures.json")

    def test_sizes_are_plausible_for_replacement_pngs(self):
        for r in self.rows:
            self.assertTrue(8 <= r["w"] <= 256 and 8 <= r["h"] <= 256, r["file"])


class Xxh64(unittest.TestCase):
    def test_reference_vectors(self):
        from xxh64 import xxh64
        self.assertEqual(xxh64(b""), 0xEF46DB3751D8E999)
        self.assertEqual(xxh64(b"a"), 0xD24EC4F1A98C6E5B)
        self.assertEqual(xxh64(b"abc"), 0x44BC2CF5AD770999)

    def test_long_input_uses_the_four_lane_path(self):
        from xxh64 import xxh64
        data = bytes(range(256)) * 4
        self.assertNotEqual(xxh64(data), xxh64(data[:-1]))
        self.assertEqual(xxh64(data), xxh64(bytes(data)))


@unittest.skipUnless(os.environ.get("PRIME_ICONS_FILES") or "--disc" in sys.argv, "needs the extracted disc (PRIME_ICONS_FILES or --disc)")
class AgainstTheDisc(unittest.TestCase):
    def test_every_name_is_what_the_disc_texture_hashes_to(self):
        import glob
        from pak_probe import parse_pak, ROOT as FILES
        from lzo_txtr import read_resource_lzo
        from icon_probe import RETRO_TO_GX
        from xxh64 import xxh64
        gx_bpp = {0: 4, 1: 8, 2: 8, 3: 16, 4: 16, 5: 16, 6: 32, 8: 4, 9: 8, 10: 16, 14: 4}
        gx_block = {0: (8, 8), 1: (8, 4), 2: (8, 4), 3: (4, 4), 4: (4, 4), 5: (4, 4), 6: (4, 4), 8: (8, 8), 9: (8, 4), 10: (4, 4), 14: (8, 8)}
        entries = {t["file"].removesuffix(".png"): t for t in json.loads(JSON_TABLE.read_text(encoding="utf-8"))}
        paks = {}
        for path in sorted(glob.glob(FILES + "/*.pak")):
            paks[os.path.basename(path)] = parse_pak(path)
        checked = 0
        for r in parse_table():
            t = entries[r["file"]]
            f, _, _, res, data_off = paks[t["pak"]]
            data = read_resource_lzo(f, res, data_off, int(t["id"], 16))
            rfmt, w, h, _ = struct.unpack(">IHHI", data[:12])
            gx = RETRO_TO_GX[rfmt]
            p = 12
            if gx in (8, 9):
                _, pw, ph = struct.unpack(">IHH", data[p:p + 8])
                p += 8 + pw * ph * 2
            bw, bh = gx_block[gx]
            size = ((w + bw - 1) // bw * bw) * ((h + bh - 1) // bh * bh) * gx_bpp[gx] // 8
            name = f"tex1_{w}x{h}_{xxh64(data[p:p + size], 0):016x}_{gx}"
            if gx not in (8, 9):
                self.assertEqual(name, r["file"], t["id"])
                checked += 1
        self.assertGreater(checked, 30)


if __name__ == "__main__":
    if "--disc" in sys.argv:
        sys.argv.remove("--disc")
        os.environ.setdefault("PRIME_ICONS_FILES", str(ROOT / "recomp/.tools/disc-extraction-rm3e01/files"))
    unittest.main(verbosity=2)
