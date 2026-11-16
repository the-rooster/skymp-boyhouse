#!/usr/bin/env python3
"""Build locally supplied SKSE sources with Caprica; never install into the game.

The upstream build concatenates vanilla and modified PSC files. Caprica 0.3.0
needs explicit Native declarations for SKSE classes outside its built-in list.
That declaration is a compiler check, not a Skyrim PEX object flag. Two void
wrappers in Armor.psc also need a call followed by return instead of returning
the void expression. Only staging copies are changed.
"""

import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess

from quest_formats import Cursor, pex


CAPRICA_030_SHA256 = "389fef19a803c36342fa7f733c96b6e27ef10fcd0b171af10427e29cbe153ec6"


def normalize_object_size(data, allow_caprica_030=False):
    """Correct only the known 0.3.0 writer defect on single-object Skyrim PEX.

    PexWriter.h::endObject directly stores a host-endian length excluding its
    four-byte field. Skyrim needs a big-endian length including that field.
    Everything after the field still goes through our complete strict parser.
    """
    c = Cursor(data, ">")
    if (c.number("I"), c.number("B"), c.number("B"), c.number("H")) != (0xFA57C0DE, 3, 2, 1):
        raise ValueError("Expected Skyrim 3.2 PEX")
    c.take(8)
    for _ in range(3):
        c.string()
    for _ in range(c.number("H")):
        c.string()
    debug = c.number("B")
    if debug not in (0, 1):
        raise ValueError("Invalid PEX debug flag")
    if debug:
        c.take(8)
        for _ in range(c.number("H")):
            c.take(7)  # object, state, function string indexes; function type
            c.take(2 * c.number("H"))
    c.take(3 * c.number("H"))  # user flag string indexes and bit numbers
    if c.number("H") != 1:
        raise ValueError("SKSE source compilation must emit one PEX object")
    c.take(2)  # object name
    offset, expected = c.pos, len(data) - c.pos
    stored = c.take(4)
    if struct.unpack(">I", stored)[0] == expected:
        return data, False
    if not allow_caprica_030 or struct.unpack("<I", stored)[0] != expected - 4:
        raise ValueError("Unexpected PEX object size; refusing to repair it")
    return data[:offset] + struct.pack(">I", expected) + data[offset + 4:], True


def prepare_script(text, name):
    declaration = re.search(r"(?im)^\s*scriptname\s+([^;\r\n]+)", text)
    if not declaration or declaration[1].split()[0].lower() != name.lower():
        raise ValueError(f"Missing or mismatched Scriptname: {name}")
    if "native" not in declaration[1].lower().split():
        text = text[:declaration.end()] + " Native" + text[declaration.end():]
    if name.lower() == "armor":
        for call in ("SetArmorRating(armorRating)", "ModArmorRating(modBy)"):
            text = re.sub(r"(?im)^(\s*)return\s+" + re.escape(call) + r"\s*$",
                          lambda m: m[1] + call + "\n" + m[1] + "return", text)
    return text


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True,
                        help="Local SKSE source checkout (contains scripts/)")
    parser.add_argument("--compiler", type=Path, required=True,
                        help="Caprica.exe supporting --game=skyrim")
    parser.add_argument("--output", type=Path, required=True,
                        help="New staging directory; existing paths are refused")
    parser.add_argument("--platform-script", type=Path,
                        help="Also compile this checkout's TESModPlatform.psc")
    args = parser.parse_args()
    source, compiler, output = (p.resolve() for p in
                                (args.source, args.compiler, args.output))
    vanilla, modified = source / "scripts/vanilla", source / "scripts/modified"
    scripts = sorted(modified.glob("*.psc"))
    if not compiler.is_file() or not vanilla.is_dir() or not scripts:
        raise ValueError("Compiler or SKSE source files are missing")
    flags = Path(__file__).resolve().parents[2] / "cmake/TESV_Papyrus_Flags.flg"
    inputs = []
    prepared = {}
    for path in scripts:
        original = vanilla / path.name
        text = original.read_text(encoding="utf-8-sig") + "\n\n" if original.exists() else ""
        if original.exists():
            inputs.append({"path": str(original), "sha256": digest(original)})
        inputs.append({"path": str(path), "sha256": digest(path)})
        prepared[path.name] = prepare_script(text + path.read_text(encoding="utf-8-sig"), path.stem)
    if args.platform_script:
        path = args.platform_script.resolve()
        if path.stem.lower() != "tesmodplatform":
            raise ValueError("--platform-script must name TESModPlatform.psc")
        inputs.append({"path": str(path), "sha256": digest(path)})
        prepared[path.name] = prepare_script(path.read_text(encoding="utf-8-sig"), path.stem)
    # Refuse reuse: a failed compiler must never leave old successful PEX outputs
    # looking like products of this build. A failed build has no manifest.
    output.mkdir(parents=True, exist_ok=False)
    merged, compiled = output / "merged", output / "pex"
    merged.mkdir()
    compiled.mkdir()
    for name, text in prepared.items():
        (merged / name).write_text(text, encoding="utf-8")
    command = [str(compiler), "--game=skyrim", "--ignorecwd", "-i",
               f"{merged};{vanilla}", "-f", str(flags), "-o", str(compiled), str(merged)]
    with (output / "compiler.log").open("w", encoding="utf-8") as log:
        subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
    actual = {p.stem.lower(): p for p in compiled.glob("*.pex")}
    expected = {Path(name).stem.lower() for name in prepared}
    if actual.keys() != expected:
        raise ValueError("Compiler output does not match the input script set")
    corrected = []
    compiler_hash = digest(compiler)
    for name, path in actual.items():
        data, changed = normalize_object_size(path.read_bytes(), compiler_hash == CAPRICA_030_SHA256)
        if pex(data).keys() != {name}:
            raise ValueError(f"Unexpected compiled object: {path.name}")
        if changed:
            path.write_bytes(data)
            corrected.append(path.name)
    manifest = {
        "schemaVersion": 1,
        "compiler": {"path": str(compiler), "sha256": compiler_hash},
        "correctedCaprica030ObjectSizes": corrected,
        "flagsSha256": digest(flags),
        "inputs": inputs,
        "outputs": [{"path": p.relative_to(output).as_posix(), "sha256": digest(p)}
                    for p in sorted(actual.values())],
        "validation": "All outputs parsed as Skyrim PEX; gameplay not verified",
    }
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"Built and parsed {len(actual)} Skyrim PEX files in {output}")


if __name__ == "__main__":
    main()
