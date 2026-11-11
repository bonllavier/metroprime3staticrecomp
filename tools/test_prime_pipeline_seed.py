"""Unit tests of prime_pipeline_seed.py's row filter: real recipes from the shipped seed pass, an
ubershader key (reserved flag bits set) and a recipe without a position attribute are refused."""
import sqlite3
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import prime_pipeline_seed as seed  # noqa: E402

SEED = Path(__file__).resolve().parents[1] / "third_party/WiiCompiled/runtime/assets/pipeline/RM3E01_00/initial_pipeline_cache.db"


class SeedFilter(unittest.TestCase):
    def rows(self):
        db = sqlite3.connect(f"file:{SEED}?mode=ro", uri=True)
        try:
            return [bytes(c) for (c,) in db.execute("SELECT config FROM pipeline_cache WHERE type = 1 AND config_version = 22 LIMIT 200")]
        finally:
            db.close()

    def test_every_shipped_recipe_is_valid(self):
        rows = self.rows()
        self.assertGreater(len(rows), 100)
        for config in rows:
            self.assertTrue(seed.is_valid_gx_config(1, config))

    def test_ubershader_keys_are_refused(self):
        config = bytearray(self.rows()[0])
        config[seed.GX_FLAGS_OFFSET] |= 0x20  # pad1 = 1, what lib/gx/uber.hpp encode_uber_pipeline writes
        self.assertFalse(seed.is_valid_gx_config(1, bytes(config)))

    def test_recipes_without_position_are_refused(self):
        config = bytearray(self.rows()[0])
        config[seed.GX_POS_ATTR_TYPE_OFFSET] = 0
        self.assertFalse(seed.is_valid_gx_config(1, bytes(config)))

    def test_clear_pipelines_are_never_filtered(self):
        self.assertTrue(seed.is_valid_gx_config(0, b"\0" * 8))


if __name__ == "__main__":
    unittest.main(verbosity=2)
