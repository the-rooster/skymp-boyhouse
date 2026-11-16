import struct
import unittest

from build_skse_scripts import normalize_object_size, prepare_script
from quest_formats import pex


def make_pex():
    # One empty object; the official length includes its own four-byte field.
    header = struct.pack(">IBBH", 0xFA57C0DE, 3, 2, 1) + bytes(14)
    strings = struct.pack(">HH", 2, 7) + b"fixture" + bytes(2)
    body = struct.pack(">IHHIHHHH", 20, 1, 1, 0, 1, 0, 0, 0)
    return header + strings + struct.pack(">BHHH", 0, 0, 1, 0) + body


class ScriptBuildTests(unittest.TestCase):
    def test_official_size_is_unchanged(self):
        data = make_pex()
        self.assertEqual(normalize_object_size(data), (data, False))

    def test_caprica_size_requires_known_compiler(self):
        data = make_pex()
        # This empty single object has 20 bytes including its length field.
        size = 20
        offset = len(data) - size
        self.assertEqual(struct.unpack_from(">I", data, offset)[0], size)
        broken = data[:offset] + struct.pack("<I", size - 4) + data[offset + 4:]
        with self.assertRaisesRegex(ValueError, "refusing to repair"):
            normalize_object_size(broken)
        repaired, changed = normalize_object_size(broken, True)
        self.assertTrue(changed)
        self.assertEqual(repaired, data)
        self.assertEqual(pex(repaired).keys(), {"fixture"})

    def test_unknown_corruption_is_not_repaired(self):
        data = make_pex()
        with self.assertRaises(ValueError):
            normalize_object_size(data[:-1], True)
        with self.assertRaises(ValueError):
            normalize_object_size(data[:20], True)
        with self.assertRaises(ValueError):
            normalize_object_size(b"\0" * len(data), True)

    def test_native_declaration_is_idempotent(self):
        original = "Scriptname SKSE Hidden\nint Function GetVersion() global native\n"
        prepared = prepare_script(original, "skse")
        self.assertIn("Scriptname SKSE Hidden Native\n", prepared)
        self.assertEqual(prepare_script(prepared, "skse"), prepared)
        with self.assertRaises(ValueError):
            prepare_script(original, "wrong_name")

    def test_void_wrapper_keeps_call_and_return(self):
        original = "Scriptname Armor extends Form Hidden\nFunction SetAR(int armorRating)\n\treturn SetArmorRating(armorRating)\nEndFunction\n"
        result = prepare_script(original, "Armor")
        self.assertIn("\tSetArmorRating(armorRating)\n\treturn\nEndFunction", result)


if __name__ == "__main__":
    unittest.main()
