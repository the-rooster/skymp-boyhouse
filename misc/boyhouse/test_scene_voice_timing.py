import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

from quest_catalog import MASTERS
from scene_voice_timing import descriptor, duration_ms, generate
from test_quest_catalog import field, record
from test_quest_dependencies import group


def fuz(decoded_bytes=2002):
    # Two synthetic xWMA packets. Only container metadata is interpreted.
    def chunk(tag, body):
        return tag + struct.pack("<I", len(body)) + body + b"\0" * (len(body) % 2)
    fmt = struct.pack("<HHIIHHH", 0x161, 1, 44100, 4000, 2, 16, 0)
    body = (b"XWMA" + chunk(b"fmt ", fmt) + chunk(b"dpds", struct.pack("<II", 0, decoded_bytes)) +
            chunk(b"data", b"abcd"))
    return b"FUZE" + struct.pack("<II", 1, 3) + b"lip" + b"RIFF" + struct.pack("<I", len(body)) + body


def bsa(entries):
    folders = {}
    for folder, name, payload in entries:
        folders.setdefault(folder, []).append((name, payload))
    names = b"".join(name.encode() + b"\0" for entries in folders.values() for name, _ in entries)
    count = sum(map(len, folders.values()))
    folder_bytes = sum(len(folder) + 1 for folder in folders)
    start = 36 + len(folders) * 25 + folder_bytes + count * 16 + len(names)
    header = struct.pack("<4s8I", b"BSA\0", 105, 36, 3, len(folders), count, folder_bytes, len(names), 0)
    table = bytearray()
    blocks = bytearray()
    payloads = bytearray()
    for folder, members in folders.items():
        table += struct.pack("<QIIQ", 0, len(members), 0, 0)
        blocks += bytes([len(folder) + 1]) + folder.encode() + b"\0"
        for _, payload in members:
            blocks += struct.pack("<QII", 0, len(payload), start + len(payloads))
            payloads += payload
    return header + table + blocks + names + payloads


def info(raw, extra=b"", flags=0):
    response = bytes(12) + b"\x01" + bytes(11)
    return record(b"INFO", raw, field(b"ENAM", bytes(4)) + field(b"TRDT", response) + extra, flags)


def topic(raw, infos, count=1):
    return (record(b"DIAL", raw, field(b"SNAM", b"SCEN") + field(b"TIFC", struct.pack("<I", count))) +
            group(infos, 7, raw))


def scene(raw, *ids):
    action = lambda typ, data: field(b"ANAM", struct.pack("<H", typ)) + field(b"DATA", struct.pack("<I", data)) + field(b"ANAM", b"")
    return record(b"SCEN", raw, b"".join(action(0, topic) for topic in ids) + action(1, 0x999))


class VoiceTimingTests(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.data = Path(tmp.name)
        self.archive = self.data / "Skyrim - Voices_en0.bsa"
        for i, name in enumerate(MASTERS):
            (self.data / name).write_bytes(record(b"TES4", body=field(b"MAST", b"Skyrim.esm\0") if i else b""))
        self.append(MASTERS[0], topic(0x100, info(0x200) + info(0x201), 2) + scene(0x101, 0x190, 0x100))
        self.append(MASTERS[1], scene(0x101, 0x100, 0x100))
        self.append(MASTERS[2], topic(0x01000100, info(0x01000200)) + scene(0x01000101, 0x01000100))
        self.entries = [
            (r"sound\voice\skyrim.esm\voiceone", "quest_00000200_1.fuz", fuz(100)),
            (r"sound\voice\skyrim.esm\voicetwo", "quest_00000200_1.fuz", fuz(10000)),
            (r"sound\voice\skyrim.esm\voiceone", "quest_00000201_1.fuz", fuz(4000)),
            (r"sound\voice\dawnguard.esm\voiceone", "quest_00000200_1.fuz", fuz(2000)),
        ]
        self.archive.write_bytes(bsa(self.entries))

    def append(self, name, records):
        with (self.data / name).open("ab") as stream:
            stream.write(records)

    def test_bulk_winning_scene_actions_stable_dlc_ids_and_max_across_voices_and_infos(self):
        index, skipped = generate(self.data, [], True)
        self.assertEqual(skipped, {})
        self.assertEqual(index["topics"], {"100:Skyrim.esm": 114, "100:Dawnguard.esm": 23})
        self.assertEqual(index["language"], "en")
        self.assertEqual(index["sources"][-1], {"file": self.archive.name,
                         "sha256": hashlib.sha256(self.archive.read_bytes()).hexdigest()})
        self.assertEqual(len(index["sources"]), 6)

    def test_incomplete_children_shared_multi_override_or_missing_audio_skip_entire_topic(self):
        self.append(MASTERS[0], topic(0x300, info(0x301, field(b"DNAM", bytes(4)))) +
                    topic(0x310, info(0x311, field(b"TRDT", bytes(24)))) +
                    topic(0x320, info(0x321), 2) + topic(0x330, info(0x331, field(b"DNAM", bytes(4)))))
        self.append(MASTERS[1], group(info(0x301), 7, 0x300))
        self.archive.write_bytes(bsa(self.entries[:2] + self.entries[3:]))
        desired = ["100:Skyrim.esm", "300:Skyrim.esm", "310:Skyrim.esm", "330:Skyrim.esm", "320:Skyrim.esm"]
        index, skipped = generate(self.data, desired)
        self.assertFalse(index["topics"])
        self.assertEqual(set(skipped), set(desired))
        self.assertEqual(skipped[desired[0]], "missing-voice-resource")
        self.assertEqual(skipped[desired[-1]], "missing-or-inconsistent-topic-children")

    def test_corrupt_alternate_voice_cannot_be_ignored(self):
        self.archive.write_bytes(bsa(self.entries + [(r"sound\voice\skyrim.esm\voicethree",
                                                      "quest_00000201_1.fuz", b"bad")]))
        index, skipped = generate(self.data, ["100:Skyrim.esm"])
        self.assertFalse(index["topics"])
        self.assertEqual(skipped, {"100:Skyrim.esm": "unsupported-voice-resource"})

    def test_reject_unknown_xwma_formats_counts_and_truncation(self):
        v = fuz(2)
        self.assertEqual(duration_ms(v), 1)
        for bad in (b"", v[:-1], v.replace(b"XWMA", b"WAVE"), v.replace(b"dpds", b"junk"), fuz(1), fuz(0)):
            with self.assertRaises(ValueError):
                duration_ms(bad)

    def test_cli_combines_topic_file_and_repeated_topics(self):
        file = self.data / "selected.txt"
        file.write_text("000100:skyrim.esm\n")
        output = self.data / "index.json"
        cmd = [sys.executable, str(Path(__file__).with_name("scene_voice_timing.py")),
               "--data-dir", str(self.data), "--output", str(output), "--topic-file", str(file),
               "--topic", "100:Dawnguard.esm", "--topic", "100:dawnguard.esm"]
        subprocess.run(cmd, check=True, capture_output=True)
        self.assertEqual(json.loads(output.read_text())["topics"], {"100:Skyrim.esm": 114, "100:Dawnguard.esm": 23})
        self.assertRaises(ValueError, descriptor, "../bad")
