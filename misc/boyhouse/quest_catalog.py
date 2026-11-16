"""Inventory QUST records from owned Skyrim masters, without exporting quest text.

Presence in this catalog is NOT a statement of multiplayer support. Load-order
overrides are resolved by original master + local FormID. Includes helper quests.
"""
import argparse
import hashlib
import json
import struct
import zlib
from pathlib import Path

MASTERS = ("Skyrim.esm", "Update.esm", "Dawnguard.esm", "HearthFires.esm", "Dragonborn.esm")

def fields(data):
    offset, extended = 0, None
    while offset < len(data):
        if offset + 6 > len(data):
            raise ValueError("Truncated field header")
        tag, size = struct.unpack_from("<4sH", data, offset)
        offset += 6
        if tag == b"XXXX":
            if size != 4 or offset + 4 > len(data) or extended is not None:
                raise ValueError("Invalid extended field size")
            extended = struct.unpack_from("<I", data, offset)[0]
            offset += 4
            continue
        length = size if extended is None else extended
        extended = None
        if offset + length > len(data):
            raise ValueError("Truncated field data")
        yield tag, data[offset:offset + length]
        offset += length
    if extended is not None:
        raise ValueError("Dangling extended field size")

def records(stream, end):
    while stream.tell() < end:
        start = stream.tell()
        header = stream.read(24)
        if len(header) != 24:
            raise ValueError("Truncated record header")
        tag, size, flags, form_id = struct.unpack_from("<4sIII", header)
        limit = start + size if tag == b"GRUP" else start + 24 + size
        if limit > end or limit < start + 24:
            raise ValueError("Invalid record/group boundary")
        if tag == b"GRUP":
            label = header[8:12]
            group_type = struct.unpack_from("<I", header, 12)[0]
            if group_type == 0 and label != b"QUST":
                stream.seek(limit)
            else:
                yield from records(stream, limit)
            continue
        if tag not in (b"TES4", b"QUST"):
            stream.seek(limit)
            continue
        payload = stream.read(size)
        if len(payload) != size:
            raise ValueError("Truncated record body")
        if flags & 0x40000:
            if len(payload) < 4:
                raise ValueError("Truncated compressed record")
            expected = struct.unpack_from("<I", payload)[0]
            payload = zlib.decompress(payload[4:])
            if len(payload) != expected:
                raise ValueError("Compressed record size mismatch")
        yield tag, flags, form_id, list(fields(payload))

def catalog(data_dir):
    entries, sources = {}, []
    for filename in MASTERS:
        path = data_dir / filename
        digest = hashlib.sha256()
        with path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
        sources.append({"file": filename, "bytes": path.stat().st_size, "sha256": digest.hexdigest()})
        masters, count = [], 0
        with path.open("rb") as stream:
            for tag, flags, form_id, values in records(stream, path.stat().st_size):
                if tag == b"TES4":
                    masters = [v.rstrip(b"\0").decode("utf-8") for k, v in values if k == b"MAST"]
                    continue
                local_index = form_id >> 24
                origins = masters + [filename]
                if local_index >= len(origins):
                    raise ValueError(f"Invalid master index in {filename}: {form_id:08X}")
                origin = origins[local_index]
                key = f"{origin}:{form_id & 0xffffff:06X}"
                editor_id = next((v.rstrip(b"\0").decode("utf-8") for k, v in values if k == b"EDID"), "")
                stages = sorted({struct.unpack_from("<H", v)[0] for k, v in values if k == b"INDX" and len(v) >= 2})
                objectives = sorted({struct.unpack_from("<H", v)[0] for k, v in values if k == b"QOBJ" and len(v) >= 2})
                entries[key] = {
                    "id": key, "editorId": editor_id, "winningPlugin": filename,
                    "deleted": bool(flags & 0x20), "stages": stages, "objectives": objectives,
                    "hasScriptData": any(k == b"VMAD" for k, _ in values),
                    "status": "untested", "evidence": [],
                }
                count += 1
        sources[-1]["questRecordCount"] = count
    quests = sorted(entries.values(), key=lambda q: q["id"])
    return {
        "schemaVersion": 1,
        "scope": "Five vanilla masters; includes internal/helper quests; not a playable quest count",
        "sources": sources,
        "summary": {"distinctQuestRecords": len(quests), "deletedRecords": sum(q["deleted"] for q in quests),
                    "supported": 0, "untested": len(quests)},
        "quests": quests,
    }

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    report = catalog(args.data_dir)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"output": str(args.output), **report["summary"]}))

if __name__ == "__main__":
    main()
