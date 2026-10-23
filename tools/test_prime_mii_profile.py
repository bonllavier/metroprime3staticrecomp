import binascii
import struct
import unittest

from prepare_prime_mii_profile import (
    ROOT, HEADER_SIZE, crc16, make_database, own_template, text_field, validate_database,
)


class MiiProfileContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.template = own_template(ROOT / "recomp/input/RM3E01_00/main.dol")
        cls.identity = bytes.fromhex("8123456702030405")
        cls.database = make_database(cls.template, "Jugador", cls.identity)

    def test_independent_crc_and_readback(self):
        self.assertEqual(crc16(b"123456789\0\0"), 0x31C3)
        stored = struct.unpack_from(">H", self.database, HEADER_SIZE - 2)[0]
        self.assertEqual(stored, binascii.crc_hqx(self.database[:HEADER_SIZE - 2], 0))
        result = validate_database(self.database)
        self.assertEqual(result["name"], "Jugador")
        self.assertEqual(result["create_id"], self.identity.hex())
        self.assertEqual(result["profile_count"], 1)

    def test_header_corruption_rejected(self):
        for offset in [0, 5, 28, 0x1D00, 0x1D10, HEADER_SIZE - 1]:
            with self.subTest(offset=offset):
                data = bytearray(self.database)
                data[offset] ^= 1
                with self.assertRaises(ValueError):
                    validate_database(bytes(data))

    def test_structural_corruption_with_valid_crc_rejected(self):
        for offset in [4 + 74 + 24, 4 + 32, 0x1D04, 0x1D10, HEADER_SIZE]:
            with self.subTest(offset=offset):
                data = bytearray(self.database)
                data[offset] ^= 1
                struct.pack_into(">H", data, HEADER_SIZE - 2,
                                 binascii.crc_hqx(data[:HEADER_SIZE - 2], 0))
                with self.assertRaises(ValueError):
                    validate_database(bytes(data))

    def test_invalid_identity_and_name_rejected(self):
        for identity in [b"\0" * 8, bytes.fromhex("2123456702030405"),
                         bytes.fromhex("8123456700000000"), b"123"]:
            with self.assertRaises(ValueError):
                make_database(self.template, "Jugador", identity)
        for name in ["", " ", "x" * 11, "\0abc", "a\nb"]:
            with self.assertRaises(ValueError):
                text_field(name)
        self.assertEqual(len(text_field("0123456789")), 20)

    def test_truncated_or_extended_database_rejected(self):
        for data in [b"", self.database[:-1], self.database + b"\0"]:
            with self.assertRaises(ValueError):
                validate_database(data)


if __name__ == "__main__":
    unittest.main()
