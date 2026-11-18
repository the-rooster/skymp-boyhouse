"""Read dependency metadata, never execute or export owned game scripts.

Format references: libespm/QuestScriptData.cpp, papyrus-vm/Reader.cpp,
TES5Edit/Core/wbDefinitionsTES5.pas, Ryan-rsm-McKenzie/bsa/src/bsa/tes4.cpp.
Unknown formats fail explicitly; these readers are analysis tools, not a VM.
"""
import mmap
import struct
import zlib


MAX_BLOB = 64 * 1024 * 1024


class Cursor:
    def __init__(self, data, endian="<"):
        if len(data) > MAX_BLOB:
            raise ValueError("Metadata buffer exceeds 64 MiB")
        self.data, self.pos, self.endian = data, 0, endian

    def take(self, size):
        if size < 0 or self.pos + size > len(self.data):
            raise ValueError(f"Truncated metadata at byte {self.pos}")
        result = self.data[self.pos:self.pos + size]
        self.pos += size
        return result

    def number(self, fmt):
        return struct.unpack(self.endian + fmt, self.take(struct.calcsize(fmt)))[0]

    def string(self):
        return self.take(self.number("H")).decode("utf-8", errors="strict")

    def remaining(self):
        return len(self.data) - self.pos

    def end(self):
        if self.remaining():
            raise ValueError(f"Unexpected trailing metadata at byte {self.pos}")


def inflate(data, expected):
    if expected > MAX_BLOB:
        raise ValueError("Decompressed record exceeds 64 MiB")
    decoder = zlib.decompressobj()
    result = decoder.decompress(data, expected + 1)
    if (len(result) != expected or not decoder.eof
            or decoder.unused_data or decoder.unconsumed_tail):
        raise ValueError("Invalid compressed record size or stream")
    return result


def plugin_records(stream, end, groups=(), *, extra_types=()):
    """All record headers; selected bodies only. INFO retains its parent group."""
    from quest_catalog import fields
    detailed = {b"TES4", b"QUST", b"DIAL", b"INFO", b"SCEN", b"PACK",
                b"SMQN", b"SMBN", b"SMEN"}
    detailed.update(extra_types)
    while stream.tell() < end:
        start = stream.tell()
        header = stream.read(24)
        if len(header) != 24:
            raise ValueError("Truncated plugin header")
        tag, size, flags, form = struct.unpack_from("<4sIII", header)
        limit = start + size if tag == b"GRUP" else start + 24 + size
        if limit < start + 24 or limit > end:
            raise ValueError("Invalid plugin record boundary")
        if tag == b"GRUP":
            if len(groups) > 32:
                raise ValueError("Plugin group depth exceeded")
            label, kind = struct.unpack_from("<Ii", header, 8)
            yield from plugin_records(stream, limit, groups + ((kind, label),),
                                      extra_types=extra_types)
        else:
            values = []
            if tag in detailed:
                if size > MAX_BLOB:
                    raise ValueError("Plugin record exceeds 64 MiB")
                body = stream.read(size)
                if len(body) != size:
                    raise ValueError("Truncated plugin record")
                if flags & 0x40000:
                    c = Cursor(body)
                    expected = c.number("I")
                    body = inflate(c.take(c.remaining()), expected)
                values = list(fields(body))
            else:
                stream.seek(limit)
            yield tag.decode("ascii"), flags, form, values, groups


def vmad(data, record_type="QUST"):
    c = Cursor(data)
    version, objformat = c.number("H"), c.number("H")

    def header(v, f):
        if v not in (4, 5) or f not in (1, 2):
            raise ValueError("Unsupported VMAD version/object format")

    header(version, objformat)

    def obj(fmt):
        if fmt == 1:
            form, alias = c.number("I"), c.number("H")
            c.number("H")
        else:
            c.number("H")
            alias, form = c.number("H"), c.number("I")
        return {"form": form, "alias": alias}

    def value(kind, fmt):
        if kind == 0:
            return []
        if kind == 1:
            return [obj(fmt)]
        if kind == 2:
            c.string()
        elif kind in (3, 4):
            c.take(4)
        elif kind == 5:
            if c.number("B") > 1:
                raise ValueError("Invalid VMAD boolean")
        elif kind in range(11, 16):
            count = c.number("I")
            if count > c.remaining() or count > 1000000:
                raise ValueError("Invalid VMAD array length")
            refs = []
            for _ in range(count):
                refs.extend(value(kind - 10, fmt))
            return refs
        elif kind != 1:
            raise ValueError(f"Unsupported VMAD value {kind}")
        return []

    def scripts(fmt):
        result = []
        for _ in range(c.number("H")):
            name, status = c.string(), c.number("B")
            if status > 3:
                raise ValueError("Unsupported VMAD script flags")
            props = []
            for _ in range(c.number("H")):
                pname, kind, pstatus = c.string(), c.number("B"), c.number("B")
                if pstatus > 3 or (kind == 0 and pstatus != 3):
                    raise ValueError("Unsupported VMAD property flags/type")
                refs = value(kind, fmt)
                if pstatus != 3:
                    props.append({"name": pname, "type": kind, "references": refs})
            if not status & 2:
                result.append({"name": name.lower(), "properties": props})
        return result

    result = {"scripts": scripts(objformat), "fragments": [], "aliases": []}
    if not c.remaining():
        return result
    if c.number("B") != 2:
        raise ValueError("Unsupported VMAD fragment binding version")

    def fragment(**extra):
        c.number("B")
        return dict(script=c.string().lower(), function=c.string().lower(), **extra)

    if record_type == "QUST":
        count = c.number("H")
        c.string()  # Source filename is deliberately discarded.
        for _ in range(count):
            stage, log = c.number("I"), c.number("I")
            result["fragments"].append(fragment(stage=stage, log=log))
        for _ in range(c.number("H")):
            ref = obj(objformat)
            av, af = c.number("H"), c.number("H")
            header(av, af)
            result["aliases"].append({"object": ref, "scripts": scripts(af)})
    elif record_type in ("INFO", "PACK", "SCEN"):
        flags = c.number("B")
        if flags & ~(7 if record_type == "PACK" else 3):
            raise ValueError("Unsupported VMAD fragment flags")
        c.string()
        for bit in range(3):
            if flags & (1 << bit):
                result["fragments"].append(fragment(event=bit))
        if record_type == "SCEN":
            for _ in range(c.number("H")):
                flag, phase = c.number("B"), c.number("I")
                result["fragments"].append(fragment(phase=phase, flags=flag))
    else:
        raise ValueError(f"Unparsed VMAD fragment tail for {record_type}")
    c.end()
    return result


ARITY = (0, 3, 3, 3, 3, 3, 3, 3, 3, 3, 2, 2, 2, 2, 2, 3, 3, 3,
         3, 3, 1, 2, 2, -4, -3, -4, 1, 3, 3, 3, 2, 2, 3, 3, 4, 4)


def pex(data):
    """Decode declarations and call sites. Literal strings/debug text stay private."""
    c = Cursor(data, ">")
    signature, major, minor, game = (c.number("I"), c.number("B"),
                                     c.number("B"), c.number("H"))
    if signature != 0xFA57C0DE or major != 3 or minor > 2 or game != 1:
        raise ValueError("Unsupported Skyrim PEX header")
    c.take(8)
    for _ in range(3):
        c.string()
    strings = [c.string() for _ in range(c.number("H"))]

    def name():
        index = c.number("H")
        if index >= len(strings):
            raise ValueError("PEX string index outside table")
        return strings[index].lower()

    debug = c.number("B")
    if debug not in (0, 1):
        raise ValueError("Invalid PEX debug flag")
    if debug:
        c.take(8)
        for _ in range(c.number("H")):
            name(), name(), name(), c.number("B")
            c.take(2 * c.number("H"))
    for _ in range(c.number("H")):
        name(), c.number("B")

    def value():
        kind = c.number("B")
        if kind == 0:
            return kind, None
        if kind in (1, 2):
            return kind, name()
        if kind == 3:
            return kind, c.number("i")
        if kind == 4:
            return kind, c.number("f")
        if kind == 5:
            flag = c.number("B")
            if flag > 1:
                raise ValueError("Invalid PEX boolean")
            return kind, flag
        raise ValueError(f"Unsupported PEX value type {kind}")

    def function(fname, state=""):
        result = {"name": fname, "state": state, "return": name()}
        name(), c.number("I")
        result["flags"] = c.number("B")
        if result["flags"] & ~3:
            raise ValueError("Unknown PEX function flags")
        types = {}
        for _ in range(2):  # parameters and locals
            for _ in range(c.number("H")):
                key, typ = name(), name()
                types[key] = typ
        result["types"], result["calls"] = types, []
        for index in range(c.number("H")):
            op = c.number("B")
            if op >= len(ARITY):
                raise ValueError(f"Unsupported PEX opcode {op}")
            args = [value() for _ in range(abs(ARITY[op]))]
            if ARITY[op] < 0:
                kind, count = args[-1]
                if kind != 3 or count < 0 or count > 128:
                    raise ValueError("Invalid PEX call argument count")
                for _ in range(count):
                    value()
            if op in (23, 24, 25, 28, 29):
                # Only symbolic operands are retained, never arbitrary call arguments.
                result["calls"].append({"op": op, "instruction": index,
                                        "operands": args[:2]})
        return result

    result = {}
    for _ in range(c.number("H")):
        objname = name()
        size_start, size = c.pos, c.number("I")
        parent = name()
        name(), c.number("I"), name()
        types, funcs, props = {}, [], {}
        for _ in range(c.number("H")):
            key, typ = name(), name()
            types[key] = typ
            c.number("I"), value()
        for _ in range(c.number("H")):
            key, typ = name(), name()
            name(), c.number("I")
            flags = c.number("B")
            if flags & ~7:
                raise ValueError("Unknown PEX property flags")
            props[key] = {"type": typ, "flags": flags}
            if flags & 4:
                types[name()] = typ
            else:
                for bit, suffix in [(1, "get"), (2, "set")]:
                    if flags & bit:
                        funcs.append(function(f"@{key}:{suffix}"))
        for _ in range(c.number("H")):
            state = name()
            for _ in range(c.number("H")):
                funcs.append(function(name(), state))
        if c.pos != size_start + size:
            raise ValueError(f"PEX object size mismatch for {objname}")
        if objname in result:
            raise ValueError("Duplicate PEX object")
        result[objname] = {"parent": parent, "types": types,
                           "functions": funcs, "properties": props}
    c.end()
    return result


def bsa_scripts(path):
    """Yield PEX bytes in the scripts folder; never extract files to disk.

    Skyrim BSA 104/105 with filename tables. SSE compressed entries require the
    optional lz4 package; unsupported compression fails rather than omitting files.
    """
    for _, filename, payload in bsa_members(path, lambda folder, name: folder == "scripts" and name.endswith(".pex")):
        yield filename[:-4], payload


def bsa_members(path, select):
    """Read selected members without loading a multi-gigabyte voice archive into memory."""
    with path.open("rb") as stream, mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as data:
        yield from _bsa_members(data, select)


def _bsa_members(data, select):
    if len(data) < 36:
        raise ValueError("Truncated BSA header")
    magic, version, offset, flags, folders, files, dir_bytes, name_bytes, _ = (
        struct.unpack_from("<4s8I", data))
    if magic != b"BSA\0" or version not in (104, 105) or offset != 36:
        raise ValueError("Unsupported BSA header")
    if flags & 3 != 3 or flags & (0x40 | 0x200):
        raise ValueError("BSA needs PC filename tables and supported compression")
    stride = 24 if version == 105 else 16
    table_end = offset + folders * stride
    if table_end > len(data) or files > len(data) // 16:
        raise ValueError("Invalid BSA table size")
    counts = [struct.unpack_from("<I", data, offset + i * stride + 8)[0]
              for i in range(folders)]
    if sum(counts) != files:
        raise ValueError("BSA file count mismatch")
    names_start = table_end + folders + dir_bytes + files * 16
    names_end = names_start + name_bytes
    if names_end > len(data):
        raise ValueError("Truncated BSA names")
    names = data[names_start:names_end].split(b"\0")
    if not names or names.pop() != b"" or len(names) != files:
        raise ValueError("BSA filename count mismatch")
    pos, name_index, seen = table_end, 0, set()
    for count in counts:
        if pos >= names_start:
            raise ValueError("Truncated BSA directory")
        length = data[pos]
        pos += 1
        folder = data[pos:pos + length]
        if not folder.endswith(b"\0"):
            raise ValueError("Invalid BSA directory name")
        folder = folder[:-1].decode("utf-8").lower()
        pos += length
        for _ in range(count):
            if pos + 16 > names_start:
                raise ValueError("Truncated BSA file entry")
            _, size_flags, start = struct.unpack_from("<QII", data, pos)
            pos += 16
            filename = names[name_index].decode("utf-8").lower()
            name_index += 1
            if not select(folder, filename):
                continue
            if "/" in filename or "\\" in filename:
                raise ValueError("Unexpected nested BSA filename")
            member = (folder, filename)
            if member in seen:
                raise ValueError("Duplicate BSA member name")
            seen.add(member)
            size = size_flags & 0x3fffffff
            if start & 0x80000000 or start < names_end or start + size > len(data):
                raise ValueError("BSA member data outside archive")
            payload = data[start:start + size]
            if flags & 0x100:
                if not payload or 1 + payload[0] > len(payload):
                    raise ValueError("Invalid embedded BSA filename")
                payload = payload[1 + payload[0]:]
            if bool(flags & 4) != bool(size_flags & 0x40000000):
                if len(payload) < 4:
                    raise ValueError("Truncated compressed BSA entry")
                expected = struct.unpack_from("<I", payload)[0]
                if expected > MAX_BLOB:
                    raise ValueError("BSA member exceeds 64 MiB")
                if version == 104:
                    payload = inflate(payload[4:], expected)
                else:
                    try:
                        import lz4.frame
                    except ImportError as exc:
                        raise ValueError("Compressed SSE BSA requires lz4.frame") from exc
                    # Incremental limit prevents decompression exceeding the declared size.
                    decoder = lz4.frame.LZ4FrameDecompressor()
                    payload = decoder.decompress(payload[4:], max_length=expected + 1)
                    if not decoder.eof or decoder.unused_data or len(payload) != expected:
                        raise ValueError("Invalid compressed SSE BSA entry")
            if len(payload) > MAX_BLOB:
                raise ValueError("BSA member exceeds 64 MiB")
            yield folder, filename, payload
    if pos != names_start:
        raise ValueError("BSA directory table length mismatch")
