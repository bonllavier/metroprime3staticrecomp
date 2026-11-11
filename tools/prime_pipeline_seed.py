#!/usr/bin/env python3
"""Accumulate the RM3E01 Aurora pipeline seed shipped beside the executable.

Aurora stores each pipeline it builds as a portable recipe (GX TEV/blend/depth state,
no GPU binary, no game asset bytes) in the user's UserData/Cache/pipeline_cache.db and
merges initial_pipeline_cache.db into it at startup, compiling every row in the
background. This tool unions the rows a play session recorded into the repository seed,
so a fresh install on any GPU/driver/backend prewarms every pipeline already seen.

    python -B recomp/tools/prime_pipeline_seed.py merge [--cache <pipeline_cache.db>]
    python -B recomp/tools/prime_pipeline_seed.py stats

Rows are never dropped: union on (type, hash), keeping the earliest first_frame_used.
Only the newest config_version per shader type is kept, matching what Aurora writes after
its own migrations. Compiled blobs (dawn_cache.db) are GPU/driver specific and never read.
"""
from __future__ import annotations

import argparse
import shutil
import sqlite3
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SEED = ROOT / "recomp/third_party/WiiCompiled/runtime/assets/pipeline/RM3E01_00/initial_pipeline_cache.db"
DEFAULT_CACHE = ROOT / "recomp/build/RM3E01_00_runtime_link_probe/UserData/Cache/pipeline_cache.db"
# Mario Kart's seed, which earlier RM3E01 builds shipped by mistake: Aurora merged it into
# every user cache and migrated its GX rows from config v19 to v20. Those rows are not Prime's.
FOREIGN_SEED = ROOT / "recomp/third_party/WiiCompiled/runtime/assets/pipeline/initial_pipeline_cache.db"
SEED_ID = "rm3e01-00"
SCHEMA = 1  # Aurora PipelineCacheSchema
COLUMNS = ("type", "hash", "config_version", "config_size", "config", "first_frame_used")


def create_seed(path: Path) -> sqlite3.Connection:
    path.parent.mkdir(parents=True, exist_ok=True)
    db = sqlite3.connect(path)
    db.executescript(f"""
        CREATE TABLE IF NOT EXISTS aurora_schema(value INTEGER);
        CREATE TABLE IF NOT EXISTS pipeline_cache (
          type INTEGER NOT NULL,
          hash INTEGER NOT NULL,
          config_version INTEGER NOT NULL,
          config_size INTEGER NOT NULL,
          config BLOB NOT NULL,
          first_frame_used INTEGER NOT NULL,
          PRIMARY KEY (type, hash)
        );
        CREATE INDEX IF NOT EXISTS pipeline_cache_load_order_idx
          ON pipeline_cache(type, config_version, first_frame_used);
        CREATE TABLE IF NOT EXISTS pipeline_seed_metadata(seed_id TEXT PRIMARY KEY NOT NULL, version INTEGER NOT NULL);
    """)
    if db.execute("SELECT COUNT(*) FROM aurora_schema").fetchone()[0] == 0:
        db.execute("INSERT INTO aurora_schema VALUES (?)", (SCHEMA,))
    return db


def check_source(db: sqlite3.Connection, path: Path) -> None:
    schema = [row[0] for row in db.execute("SELECT value FROM aurora_schema")]
    if schema != [SCHEMA]:
        raise SystemExit(f"{path}: Aurora pipeline cache schema {schema}, expected [{SCHEMA}]")
    columns = tuple(row[1] for row in db.execute("PRAGMA table_info(pipeline_cache)"))
    if columns != COLUMNS:
        raise SystemExit(f"{path}: unexpected pipeline_cache columns {columns}")


def counts(db: sqlite3.Connection) -> dict:
    return {f"type{t}/v{v}": n for t, v, n in
            db.execute("SELECT type, config_version, COUNT(*) FROM pipeline_cache GROUP BY type, config_version")}


def foreign_rows() -> tuple[set, set]:
    """Clear-row hashes and GX v19 recipes (version field and the v20 dstAlpha word removed)."""
    if not FOREIGN_SEED.exists():
        return set(), set()
    db = sqlite3.connect(f"file:{FOREIGN_SEED}?mode=ro", uri=True)
    clear = {h for (h,) in db.execute("SELECT hash FROM pipeline_cache WHERE type = 0")}
    gx = {bytes(c[4:]) for (c,) in db.execute("SELECT config FROM pipeline_cache WHERE type = 1 AND config_version = 19")}
    return clear, gx


def is_foreign(shader_type: int, hash_value: int, config: bytes, foreign: tuple[set, set]) -> bool:
    if shader_type == 0:
        return False  # clear pipelines are game-independent; Prime uses the same ones
    # Aurora's v19 -> v20 migration inserts ShaderConfig.dstAlpha, the 4 bytes before the
    # 36-byte PipelineConfig tail, and rewrites the leading version word.
    insert_at = len(config) - 40
    return shader_type == 1 and bytes(config[4:insert_at] + config[insert_at + 4:]) in foreign[1]


# Byte offset of ShaderConfig.attrs[GX_VA_POS].attrType inside a v22 PipelineConfig
# (version u32, msaaSamples u32, then fogType, vtxStride, flags, numTexGens, zTexture u32,
# and 8-byte AttrConfig entries; GX_VA_POS is attribute 9). A GX draw always has a position.
GX_POS_ATTR_TYPE_OFFSET = 8 + 8 + 9 * 8
# ShaderConfig flags byte (lineMode:2, dualTex:1, fogRangeAdjust:1, perPixelLighting:1, pad1:3). pad1 is
# zero in every real recipe; an ubershader key sets it (lib/gx/uber.hpp) and is never a seed row.
GX_FLAGS_OFFSET = 8 + 2


def is_valid_gx_config(shader_type: int, config: bytes) -> bool:
    if shader_type != 1:
        return True
    if len(config) <= GX_POS_ATTR_TYPE_OFFSET or config[GX_POS_ATTR_TYPE_OFFSET] == 0:
        return False
    return (config[GX_FLAGS_OFFSET] >> 5) & 7 == 0


def merge(cache: Path) -> None:
    if not cache.exists():
        print(f"no pipeline cache at {cache}; seed unchanged")
        return
    source = sqlite3.connect(f"file:{cache}?mode=ro", uri=True)
    check_source(source, cache)
    newest = dict(source.execute("SELECT type, MAX(config_version) FROM pipeline_cache GROUP BY type"))
    foreign = foreign_rows()
    seed = create_seed(SEED)
    excluded = 0
    invalid = 0
    before = seed.execute("SELECT COUNT(*) FROM pipeline_cache").fetchone()[0]
    with seed:
        # A config version bump makes older rows unloadable once Aurora has migrated the cache.
        for shader_type, version in newest.items():
            seed.execute("DELETE FROM pipeline_cache WHERE type = ? AND config_version < ?", (shader_type, version))
        for row in source.execute(f"SELECT {', '.join(COLUMNS)} FROM pipeline_cache"):
            shader_type, _, version, size, config, _ = row
            if version != newest[shader_type] or size != len(config):
                continue
            if is_foreign(shader_type, row[1], config, foreign):
                excluded += 1
                continue
            if not is_valid_gx_config(shader_type, config):
                invalid += 1
                continue
            seed.execute(f"""INSERT INTO pipeline_cache ({', '.join(COLUMNS)}) VALUES (?, ?, ?, ?, ?, ?)
                ON CONFLICT(type, hash) DO UPDATE SET
                  first_frame_used = MIN(first_frame_used, excluded.first_frame_used)""", row)
        after = seed.execute("SELECT COUNT(*) FROM pipeline_cache").fetchone()[0]
        version = (seed.execute("SELECT version FROM pipeline_seed_metadata WHERE seed_id = ?", (SEED_ID,)).fetchone()
                   or (0,))[0]
        seed.execute("DELETE FROM pipeline_seed_metadata")
        seed.execute("INSERT INTO pipeline_seed_metadata VALUES (?, ?)", (SEED_ID, version + (after != before)))
    seed.execute("VACUUM")
    print(f"seed {SEED}\n  rows {before} -> {after} (+{after - before}) {counts(seed)} excluded_mkw={excluded}"
          f" invalid={invalid}"
          f"\n  size {SEED.stat().st_size / 2**20:.1f} MiB")
    seed.close()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", choices=("merge", "stats"))
    parser.add_argument("--cache", type=Path, default=DEFAULT_CACHE)
    parser.add_argument("--install", type=Path, help="also copy the seed into this directory (the exe folder)")
    args = parser.parse_args()
    if args.command == "merge":
        merge(args.cache)
    elif SEED.exists():
        seed = sqlite3.connect(f"file:{SEED}?mode=ro", uri=True)
        print(SEED, counts(seed), f"{SEED.stat().st_size / 2**20:.1f} MiB")
    else:
        print(f"no seed yet at {SEED}")
    if args.install and SEED.exists():
        shutil.copy2(SEED, args.install / "initial_pipeline_cache.db")


if __name__ == "__main__":
    sys.exit(main())
