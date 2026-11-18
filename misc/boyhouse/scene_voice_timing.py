"""Generate conservative English scene-topic timings from the five owned Skyrim masters and stock voice archives.

Use --all-scene-topics for bulk discovery, --topic 17f2b:Skyrim.esm for selected
topics, or --topic-file for a newline-delimited list of those descriptors. Each
indexed value is the longest encoded English response in milliseconds, rounded
up. No speech, subtitles or game files are written to the index.
"""
import argparse
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path
import re
import struct
import sys
import tempfile

from quest_catalog import MASTERS
from quest_formats import bsa_members, plugin_records

VOICE_KEY = re.compile(r".+_([0-9a-f]{8})_([0-9]+)\.(fuz|xwm|wav)\Z")
VOICE_ARCHIVE = re.compile(r"Skyrim - Voices_en([0-9]+)\.bsa\Z", re.I)
INFO_FIELDS = {b"EDID", b"ENAM", b"PNAM", b"CNAM", b"ANAM", b"TRDT", b"NAM1", b"NAM2", b"NAM3",
               b"CTDA", b"CIS1", b"CIS2"}


def descriptor(value):
    match = re.fullmatch(r"([0-9a-f]{1,6}):([^:]+)", value.strip(), re.I)
    if match:
        form = int(match[1], 16)
        for master in MASTERS:
            if form and match[2].lower() == master.lower():
                return f"{form:x}:{master}"
    raise ValueError(f"Expected a nonzero vanilla topic descriptor, got {value!r}")


def stable_id(raw, origins):
    if not raw or raw >> 24 >= len(origins):
        raise ValueError("Invalid plugin-local FormID")
    return f"{raw & 0xffffff:x}:{origins[raw >> 24]}"


def scene_topics(fields, origins):
    topics = set()
    kind = topic = None
    for tag, value in fields:
        if tag == b"ANAM":
            if kind is None and len(value) == 2:
                kind = int.from_bytes(value, "little")
            elif kind is not None and not value:
                if kind == 0 and topic:
                    topics.add(stable_id(topic, origins))
                kind = topic = None
            else:
                return set()
        elif kind == 0 and tag == b"DATA":
            if topic is not None or len(value) != 4:
                return set()
            topic = int.from_bytes(value, "little")
    return topics if kind is None else set()


def response_number(fields):
    # Shared INFO, continuations, sound overrides, legacy response formats and
    # unrecognized playback features cannot be timed from one local FUZ.
    if any(tag not in INFO_FIELDS for tag, _ in fields):
        return None
    flags = [v for tag, v in fields if tag == b"ENAM"]
    if len(flags) != 1 or len(flags[0]) != 4 or int.from_bytes(flags[0][:2], "little") & ~0x226:
        return None
    records = [v for tag, v in fields if tag == b"TRDT"]
    if len(records) != 1 or len(records[0]) != 24 or any(records[0][16:20]):
        return None
    return records[0][12] or None


def catalog(data):
    scenes, topics, infos = {}, {}, {}
    for name in MASTERS:
        path = data / name
        with path.open("rb") as stream:
            origins = None
            for tag, flags, raw, fields, groups in plugin_records(stream, path.stat().st_size):
                if tag == "TES4":
                    origins = [v.rstrip(b"\0").decode() for k, v in fields if k == b"MAST"] + [name]
                    if any(m not in MASTERS[:MASTERS.index(name)] for m in origins[:-1]):
                        raise ValueError(f"Unexpected master dependency in {name}")
                    continue
                if tag not in ("SCEN", "DIAL", "INFO"):
                    continue
                if origins is None:
                    raise ValueError(f"Missing header in {name}")
                key = stable_id(raw, origins)
                deleted = bool(flags & 0x20)
                if tag == "SCEN":
                    scenes[key] = set() if deleted else scene_topics(fields, origins)
                elif tag == "DIAL":
                    counts = [v for k, v in fields if k == b"TIFC"]
                    topics[key] = (int.from_bytes(counts[0], "little") if not deleted and
                                   len(counts) == 1 and len(counts[0]) == 4 and
                                   (b"SNAM", b"SCEN") in fields else 0)
                elif deleted:
                    infos.pop(key, None)
                else:
                    if not groups or groups[-1][0] != 7:
                        raise ValueError(f"INFO outside topic children in {name}")
                    parent = stable_id(groups[-1][1], origins)
                    # Overridden responses require resolving the override's voice
                    # assets/loose files. Omit these until provenance is known.
                    number = response_number(fields) if origins[raw >> 24] == name else None
                    infos[key] = (parent, number)
    return scenes, topics, infos


def duration_ms(fuz):
    """FUZE v1 holds a LIP block followed by RIFF xWMA (no audio decoding needed)."""
    if len(fuz) < 24 or fuz[:8] != b"FUZE\x01\0\0\0":
        raise ValueError("Unknown FUZ")
    start = 12 + int.from_bytes(fuz[8:12], "little")
    wave = memoryview(fuz)[start:]
    if len(wave) < 12 or wave[:4] != b"RIFF" or wave[8:12] != b"XWMA" or int.from_bytes(wave[4:8], "little") != len(wave) - 8:
        raise ValueError("Unknown or truncated xWMA")
    chunks = {}
    pos = 12
    while pos < len(wave):
        if len(wave) - pos < 8:
            raise ValueError("Truncated RIFF chunk")
        tag, size = struct.unpack_from("<4sI", wave, pos)
        pos += 8
        if tag in chunks or pos + size + (size & 1) > len(wave):
            raise ValueError("Invalid RIFF chunk")
        chunks[tag] = wave[pos:pos + size]
        pos += size + (size & 1)
    fmt, dpds, audio = (chunks.get(tag, b"") for tag in (b"fmt ", b"dpds", b"data"))
    if len(fmt) != 18 or not audio or not dpds or len(dpds) % 4:
        raise ValueError("Missing xWMA duration metadata")
    codec, channels, rate, _, block, bits, extra = struct.unpack("<HHIIHHH", fmt)
    if codec not in (0x161, 0x162) or not channels or not rate or not block or bits != 16 or extra:
        raise ValueError("Unsupported xWMA format")
    if len(dpds) // 4 != (len(audio) + block - 1) // block:
        raise ValueError("xWMA packet count mismatch")
    last = 0
    for count, in struct.iter_unpack("<I", dpds):
        if count < last or count % (2 * channels):
            raise ValueError("Invalid xWMA decoded byte count")
        last = count
    if not last:
        raise ValueError("Empty xWMA")
    denominator = rate * channels * 2
    return (last * 1000 + denominator - 1) // denominator


def voice_asset(folder, filename):
    parts = folder.split("\\")
    match = VOICE_KEY.fullmatch(filename)
    if len(parts) != 4 or parts[:2] != ["sound", "voice"] or not parts[3] or not match:
        return None
    form = int(match[1], 16)
    return (f"{form & 0xffffff:x}:{parts[2]}", int(match[2]), match[3], form >> 24)


def digest(path):
    with path.open("rb") as stream:
        checksum = hashlib.sha256()
        for block in iter(lambda: stream.read(4 << 20), b""):
            checksum.update(block)
        return {"file": path.name, "sha256": checksum.hexdigest()}


def generate(data, requested, all_scene_topics=False):
    scenes, counts, infos = catalog(data)
    requested = {descriptor(key) for key in requested}
    if all_scene_topics:
        for used in scenes.values():
            requested.update(used)
    children = defaultdict(list)
    expected = {}
    for info, (topic, number) in infos.items():
        if topic in requested:
            children[topic].append(info.lower())
            expected[info.lower()] = number
    archives = sorted((int(m[1]), p) for p in data.iterdir() if (m := VOICE_ARCHIVE.fullmatch(p.name)))
    if not archives or [i for i, _ in archives] != list(range(len(archives))):
        raise ValueError("Expected contiguous stock Skyrim - Voices_enN.bsa archives starting at zero")
    for directory in data.iterdir():
        if directory.name.lower() == "sound" and directory.is_dir():
            for voice in directory.iterdir():
                if voice.name.lower() == "voice" and voice.is_dir() and any(p.is_file() for p in voice.rglob("*")):
                    raise ValueError("Loose voice assets present; source precedence is unknown")
    durations, blocked = defaultdict(list), set()
    def selected(folder, name):
        asset = voice_asset(folder, name)
        return asset is not None and asset[0] in expected
    for _, archive in archives:
        for folder, filename, payload in bsa_members(archive, selected):
            info, number, extension, prefix = voice_asset(folder, filename)
            if extension != "fuz" or prefix or number != expected[info]:
                blocked.add(info)
                continue
            try:
                durations[info].append(duration_ms(payload))
            except ValueError:
                blocked.add(info)
    timed, skipped = {}, {}
    for topic in sorted(requested):
        rows = children[topic]
        if not rows or counts.get(topic) != len(rows):
            skipped[topic] = "missing-or-inconsistent-topic-children"
        elif any(expected[i] is None for i in rows):
            skipped[topic] = "unsupported-info-response"
        elif any(i in blocked for i in rows):
            skipped[topic] = "unsupported-voice-resource"
        elif any(not durations[i] for i in rows):
            skipped[topic] = "missing-voice-resource"
        else:
            timed[topic] = max(max(durations[i]) for i in rows)
    sources = [digest(data / name) for name in MASTERS] + [digest(path) for _, path in archives]
    return {"schemaVersion": 1, "language": "en", "sources": sources, "topics": timed}, skipped


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--topic", action="append", default=[])
    parser.add_argument("--topic-file", "--topic-list", type=Path, help="One hexadecimal topic:master descriptor per line")
    parser.add_argument("--all-scene-topics", action="store_true")
    args = parser.parse_args()
    try:
        topics = args.topic
        if args.topic_file:
            topics += [line.strip() for line in args.topic_file.read_text().splitlines() if line.strip()]
        if not topics and not args.all_scene_topics:
            parser.error("Specify --topic, --topic-file, or --all-scene-topics")
        index, skipped = generate(args.data_dir, topics, args.all_scene_topics)
        # A failed build must leave a previously usable server index intact.
        temporary = None
        try:
            with tempfile.NamedTemporaryFile(dir=args.output.parent, delete=False) as stream:
                temporary = Path(stream.name)
                stream.write((json.dumps(index, separators=(",", ":")) + "\n").encode())
            temporary.replace(args.output)
        finally:
            if temporary is not None:
                temporary.unlink(missing_ok=True)
        print(f"Indexed {len(index['topics'])} topics. Skipped {len(skipped)}: {dict(Counter(skipped.values()))}", file=sys.stderr)
    except (OSError, ValueError) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()
