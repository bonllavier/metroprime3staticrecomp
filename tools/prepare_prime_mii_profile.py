#!/usr/bin/env python3
"""Create one local RFL profile from the player's pinned DOL, without overwriting NAND.

Format research: koopthekoopa/RFL db7c33ae2989b6103a07e666f2f2f9bbd1d2f926,
RFLi_Database.h, RFLi_HiddenDatabase.h, RFL_Format.c, RFL_Database.c.
No reference implementation or bundled character resources are distributed here.
"""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
from pathlib import Path
import secrets
import struct

ROOT = Path(__file__).resolve().parents[2]
DOL_SHA256 = "6b550f221602074747a2e61b0aa064203fd493f6865dfb3b1a912682065e6104"
TEMPLATE_SHA256 = "bf05116ce4d38f9ad10d186e852e8748a8f26a62784794bc2e9b047a836b0351"
HEADER_SIZE = 0x1F1E0
HIDDEN_OFFSET = 0x1D00
DATABASE_SIZE = HEADER_SIZE + 10000 * 64


def crc16(data: bytes | bytearray) -> int:
    """RFL's augmented, MSB-first polynomial 0x1021, initial remainder zero."""
    remainder = 0
    for value in data:
        for bit in range(7, -1, -1):
            remainder = ((remainder << 1) ^ (0x1021 if remainder & 0x8000 else 0)
                         ^ ((value >> bit) & 1)) & 0xFFFF
    return remainder


def text_field(name: str) -> bytes:
    # Deliberately keep the first profile within the library's unambiguous names.
    if not 1 <= len(name) <= 10 or any(not (32 <= ord(c) <= 126) for c in name):
        raise ValueError("Profile name must contain 1..10 printable ASCII characters")
    if not name.strip():
        raise ValueError("Profile name cannot be blank")
    return name.encode("utf-16-be").ljust(20, b"\0")


def own_template(dol_path: Path) -> bytes:
    dol = dol_path.read_bytes()
    if hashlib.sha256(dol).hexdigest() != DOL_SHA256:
        raise ValueError("Expected the original RM3E01 disc 0 revision 0 DOL")
    address = 0x8058A5A8
    for index in range(18):
        offset = struct.unpack_from(">I", dol, index * 4)[0]
        base = struct.unpack_from(">I", dol, 0x48 + index * 4)[0]
        length = struct.unpack_from(">I", dol, 0x90 + index * 4)[0]
        if base <= address and address + 74 <= base + length:
            data = dol[offset + address - base:offset + address - base + 74]
            if hashlib.sha256(data).hexdigest() != TEMPLATE_SHA256:
                raise ValueError("Original default Mii preimage differs")
            return data
    raise ValueError("Default Mii is outside the original DOL sections")


def make_database(template: bytes, name: str, create_id: bytes) -> bytes:
    if len(template) != 74 or hashlib.sha256(template).hexdigest() != TEMPLATE_SHA256:
        raise ValueError("Only the verified original default appearance is supported")
    if len(create_id) != 8 or create_id[0] & 0xF0 != 0x80 or not any(create_id[4:]):
        raise ValueError("Expected a normal, persistent, non-temporary local identity")
    profile = bytearray(template)
    profile[2:22] = text_field(name)
    profile[24:32] = create_id
    profile[54:74] = text_field("PC")
    data = bytearray(DATABASE_SIZE)
    data[:4] = b"RNOD"
    data[4:78] = profile
    struct.pack_into(">I", data, 0x1CEC, 0x80000000)  # isolated database
    data[HIDDEN_OFFSET:HIDDEN_OFFSET + 8] = b"RNHD\xff\xff\xff\xff"
    for index in range(10000):
        # Empty linked-list node: sex=0, signed next/prev=-1 (15-bit fields).
        struct.pack_into(">HH", data, HIDDEN_OFFSET + 8 + index * 12 + 8,
                         0x7FFF, 0x7FFF)
    struct.pack_into(">H", data, HEADER_SIZE - 2, crc16(data[:HEADER_SIZE]))
    validate_database(bytes(data))
    return bytes(data)


def validate_database(data: bytes) -> dict:
    if len(data) != DATABASE_SIZE:
        raise ValueError("Incorrect RFL database size")
    if data[:4] != b"RNOD" or data[HIDDEN_OFFSET:HIDDEN_OFFSET + 4] != b"RNHD":
        raise ValueError("Incorrect database identifiers")
    if data[HIDDEN_OFFSET + 4:HIDDEN_OFFSET + 8] != b"\xff" * 4:
        raise ValueError("Expected an empty hidden-character list")
    if crc16(data[:HEADER_SIZE]) != 0:
        raise ValueError("Invalid database header CRC")
    occupied = [i for i in range(100) if any(data[4 + i * 74 + 24:4 + i * 74 + 32])]
    if occupied != [0]:
        raise ValueError("Expected exactly one profile in slot zero")
    if any(data[78:0x1CEC]) or any(data[HEADER_SIZE:]):
        raise ValueError("Unexpected extra official or hidden characters")
    for index in range(10000):
        entry = data[HIDDEN_OFFSET + 8 + index * 12:HIDDEN_OFFSET + 20 + index * 12]
        if entry != b"\0" * 8 + b"\x7f\xff\x7f\xff":
            raise ValueError("Unexpected hidden-database linkage")
    profile = data[4:78]
    text = profile[2:22].decode("utf-16-be").rstrip("\0")
    text_field(text)
    if profile[24] & 0xF0 != 0x80 or not any(profile[28:32]):
        raise ValueError("Invalid persistent profile identity")
    # This tool deliberately supports only its generated appearance/settings.
    # General-purpose import would require validating every original bitfield.
    expected = bytearray(own_template(ROOT / "recomp/input/RM3E01_00/main.dol"))
    expected[2:22] = text_field(text)
    expected[24:32] = profile[24:32]
    expected[54:74] = text_field("PC")
    if profile != expected:
        raise ValueError("Profile settings differ from the supported original template")
    return {"profile_count": 1, "name": text, "create_id": profile[24:32].hex(),
            "header_crc_residue": 0, "size": len(data),
            "sha256": hashlib.sha256(data).hexdigest()}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--name", default="Jugador")
    parser.add_argument("--dol", type=Path, default=ROOT / "recomp/input/RM3E01_00/main.dol")
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    if args.check:
        print(json.dumps(validate_database(args.output.read_bytes()), indent=2))
        return
    if args.output.exists():
        raise FileExistsError("Existing Mii database preserved; use --check to inspect it")
    # Normal ID: elapsed four-second units since 2006-06-17 and a local random
    # console component. This is private PC identity, not a claimed physical Wii.
    epoch = dt.datetime(2006, 6, 17, tzinfo=dt.timezone.utc)
    ticks = int((dt.datetime.now(dt.timezone.utc) - epoch).total_seconds() / 4)
    create_id = struct.pack(">I", 0x80000000 | (ticks & 0x0FFFFFFF)) + secrets.token_bytes(4)
    data = make_database(own_template(args.dol), args.name, create_id)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    # Exclusive creation also protects against races with another setup process.
    with args.output.open("xb") as target:
        target.write(data)
    actual = args.output.read_bytes()
    if actual != data:
        raise IOError("Database read-back differs")
    result = validate_database(actual)
    result.update({"output": str(args.output.resolve()), "original_library_acceptance": "pending",
                   "save_reload_verified": False})
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
