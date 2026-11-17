import io
import struct
import tempfile
import unittest
import zlib
from pathlib import Path
from quest_catalog import MASTERS, catalog, fields, records

def field(tag, data):
    return struct.pack("<4sH", tag, len(data)) + data

def record(tag, form=0, body=b"", flags=0):
    return struct.pack("<4sIIIIHH", tag, len(body), flags, form, 0, 44, 0) + body

def group(body):
    return struct.pack("<4sI4sIII", b"GRUP", len(body) + 24, b"QUST", 0, 0, 0) + body

class CatalogTests(unittest.TestCase):
    def test_compressed_quest_in_group(self):
        body = field(b"EDID", b"Fixture\0") + field(b"INDX", struct.pack("<HBB", 10, 0, 0))
        payload = struct.pack("<I", len(body)) + zlib.compress(body)
        data = group(record(b"QUST", 42, payload, 0x40000))
        result = list(records(io.BytesIO(data), len(data)))
        self.assertEqual(result[0][2], 42)
        self.assertEqual(result[0][3][0], (b"EDID", b"Fixture\0"))

    def test_extended_field_size(self):
        data = field(b"XXXX", struct.pack("<I", 3)) + struct.pack("<4sH", b"EDID", 0) + b"abc"
        self.assertEqual(list(fields(data)), [(b"EDID", b"abc")])

    def test_bad_boundaries_are_rejected(self):
        for data in [b"QUST", record(b"QUST", body=b"abc")[:-1], struct.pack("<4sI4sIII", b"GRUP", 0, b"QUST", 0, 0, 0)]:
            with self.assertRaises(ValueError): list(records(io.BytesIO(data), len(data)))
        with self.assertRaises(ValueError): list(fields(b"EDID\x03\x00a"))

    def test_override_resolution_and_no_automatic_support_claim(self):
        with tempfile.TemporaryDirectory() as tmp:
            for index, name in enumerate(MASTERS):
                header = record(b"TES4", body=field(b"MAST", b"Skyrim.esm\0") if index else b"")
                quest = record(b"QUST", 42, field(b"EDID", f"Version{index}".encode() + b"\0"))
                (Path(tmp) / name).write_bytes(header + group(quest))
            report = catalog(Path(tmp))
            self.assertEqual(report["summary"]["distinctQuestRecords"], 1)
            self.assertEqual(report["quests"][0]["winningPlugin"], "Dragonborn.esm")
            self.assertEqual(report["quests"][0]["status"], "untested")
            self.assertEqual(report["summary"]["supported"], 0)

if __name__ == "__main__": unittest.main()
