import io
import json
from pathlib import Path
import struct
import tempfile
import unittest
import zlib

from quest_catalog import MASTERS
from quest_dependencies import (ScriptGraph, analyze, capability_status, fingerprint,
                                load_records, load_scripts, read_registry,
                                stable_form, summarize_record)
from quest_formats import bsa_scripts, inflate, pex, plugin_records, vmad
from test_quest_catalog import field, record


def string(text, endian="<"):
    data = text.encode()
    return struct.pack(endian + "H", len(data)) + data


def group(body, kind=0, label=0):
    return struct.pack("<4sIIiII", b"GRUP", len(body) + 24, label, kind, 0, 0) + body


def vmad_object(form, alias=0xffff, fmt=2):
    return struct.pack("<IHH", form, alias, 0) if fmt == 1 else struct.pack("<HHI", 0, alias, form)


def vmad_script(name, props=(), status=0):
    return string(name) + struct.pack("<BH", status, len(props)) + b"".join(
        string(key) + struct.pack("<BB", kind, pstatus) + data
        for key, kind, pstatus, data in props)


def make_vmad(scripts=(), fmt=2, tail=b""):
    return struct.pack("<HHH", 5, fmt, len(scripts)) + b"".join(scripts) + tail


def make_pex(name="fixture", parent="", variables=None, funcs=None, props=None):
    strings = []

    def s(value):
        if value not in strings:
            strings.append(value)
        return struct.pack(">H", strings.index(value))

    def v(value):
        kind, data = value
        return bytes([kind]) + (s(data) if kind in (1, 2) else
                               struct.pack(">i", data) if kind == 3 else
                               struct.pack(">f", data) if kind == 4 else
                               bytes([data]) if kind == 5 else b"")

    def f(fn):
        code = b"".join(bytes([op]) + b"".join(v(a) for a in args)
                        for op, args in fn.get("code", []))
        locals_ = fn.get("types", {})
        return (s("none") + s("") + struct.pack(">IBHH", 0, fn.get("flags", 0), 0, len(locals_))
                + b"".join(s(k) + s(t) for k, t in locals_.items())
                + struct.pack(">H", len(fn.get("code", []))) + code)

    body = s(parent) + s("") + struct.pack(">I", 0) + s("")
    variables, funcs, props = variables or {}, funcs or [], props or {}
    body += struct.pack(">H", len(variables))
    for key, typ in variables.items():
        body += s(key) + s(typ) + struct.pack(">I", 0) + v((0, None))
    body += struct.pack(">H", len(props))
    for key, typ in props.items():
        body += s(key) + s(typ) + s("") + struct.pack(">IB", 0, 7) + s("::" + key + "_var")
    body += struct.pack(">H", 1) + s("") + struct.pack(">H", len(funcs))
    for fn in funcs:
        body += s(fn["name"]) + f(fn)
    object_name = s(name)
    obj = object_name + struct.pack(">I", len(body) + 4) + body
    return (struct.pack(">IBBHQ", 0xFA57C0DE, 3, 2, 1, 0)
            + string("fixture.psc", ">") + string("", ">") + string("", ">")
            + struct.pack(">H", len(strings)) + b"".join(string(x, ">") for x in strings)
            + b"\0" + struct.pack(">HH", 0, 1) + obj)


def make_bsa(payload, version=105, compressed=False, toggle=False, embedded=False):
    flags = 3 | (4 if compressed else 0) | (0x100 if embedded else 0)
    stored = payload
    if compressed != toggle:
        assert version == 104
        stored = struct.pack("<I", len(payload)) + zlib.compress(payload)
    if embedded:
        embedded_name = b"scripts\\fixture.pex"
        stored = bytes([len(embedded_name)]) + embedded_name + stored
    folder, filename = b"scripts\0", b"fixture.pex\0"
    stride = 24 if version == 105 else 16
    data_offset = 36 + stride + 1 + len(folder) + 16 + len(filename)
    header = struct.pack("<4s8I", b"BSA\0", version, 36, flags, 1, 1, len(folder), len(filename), 0)
    directory = struct.pack("<QIIQ", 0, 1, 0, 0) if version == 105 else struct.pack("<QII", 0, 1, 0)
    entry = struct.pack("<QII", 0, len(stored) | (0x40000000 if toggle else 0), data_offset)
    return header + directory + bytes([len(folder)]) + folder + entry + filename + stored


REGISTRY = {"nativeSubsets": ["quest.getstage", "globalvariable.getvalue"],
            "conditionSubsets": ["58", "74"]}
ORIGINS = ["Skyrim.esm"]


def summary(typ="QUST", form=42, values=(), groups=(), flags=0):
    return summarize_record(typ, flags, form, values, groups, ORIGINS, "Skyrim.esm")


class FormatTests(unittest.TestCase):
    def test_vmad_aliases_and_object_array_in_both_layouts(self):
        for fmt in (1, 2):
            script = vmad_script("Root", [
                ("Targets", 11, 0, struct.pack("<I", 2) + vmad_object(42, 3, fmt) + vmad_object(43, fmt=fmt)),
                ("Removed", 1, 3, vmad_object(99, fmt=fmt)),
            ])
            tail = (struct.pack("<BH", 2, 1) + string("private source")
                    + struct.pack("<IIB", 10, 2, 1) + string("Root") + string("Fragment_0")
                    + struct.pack("<H", 1) + vmad_object(42, 7, fmt)
                    + struct.pack("<HHH", 5, fmt, 1) + vmad_script("AliasScript"))
            data = make_vmad([script], fmt, tail)
            result = vmad(data)
            self.assertEqual(result["scripts"][0]["properties"][0]["references"],
                             [{"form": 42, "alias": 3}, {"form": 43, "alias": 65535}])
            self.assertEqual(len(result["scripts"][0]["properties"]), 1)
            self.assertEqual(result["aliases"][0]["scripts"][0]["name"], "aliasscript")
            self.assertEqual(result["fragments"][0]["stage"], 10)
            for end in range(len(data)):
                if end == len(make_vmad([script], fmt)):
                    continue  # A VMAD without its optional fragment tail is valid.
                with self.assertRaises(ValueError):
                    vmad(data[:end])
            with self.assertRaises(ValueError):
                vmad(data + b"\0")

    def test_optional_vmad_tail_and_removed_script(self):
        self.assertEqual(vmad(make_vmad([vmad_script("removed", status=3)]))["scripts"], [])
        self.assertEqual(vmad(make_vmad())["fragments"], [])

    def test_scene_phase_and_info_fragments(self):
        base = struct.pack("<BB", 2, 3) + string("source")
        fragment = b"\1" + string("fragment") + string("run")
        result = vmad(make_vmad(tail=base + fragment * 2), "INFO")
        self.assertEqual(len(result["fragments"]), 2)
        tail = base + fragment * 2 + struct.pack("<HBI", 1, 1, 23) + fragment
        self.assertEqual(vmad(make_vmad(tail=tail), "SCEN")["fragments"][2]["phase"], 23)
        with self.assertRaises(ValueError):
            vmad(make_vmad(tail=base + fragment), "INFO")

    def test_pex_calls_keep_types_and_discard_literal_content(self):
        data = make_pex(variables={"target": "quest"}, funcs=[
            {"name": "run", "code": [
                (23, [(1, "GetStage"), (1, "target"), (1, "result"), (3, 1), (2, "PRIVATE DIALOGUE")]),
                (25, [(1, "utility"), (1, "wait"), (1, "none"), (3, 1), (4, 0.1)]),
                (24, [(1, "run"), (1, "none"), (3, 0)]),
                (28, [(1, "target"), (1, "self"), (1, "result")]),
            ]}])
        obj = pex(data)["fixture"]
        self.assertEqual(obj["types"]["target"], "quest")
        self.assertEqual([c["op"] for c in obj["functions"][0]["calls"]], [23, 25, 24, 28])
        self.assertNotIn("private dialogue", json.dumps(obj))
        for end in range(len(data)):
            with self.assertRaises(ValueError):
                pex(data[:end])
        with self.assertRaises(ValueError):
            pex(data + b"\0")

    def test_pex_rejects_bad_call_count_opcode_and_header(self):
        for code in [
            [(23, [(1, "run"), (1, "self"), (1, "result"), (3, -1)])],
            [(23, [(1, "run"), (1, "self"), (1, "result"), (2, "two")])],
            [(255, [])],
        ]:
            with self.assertRaises(ValueError):
                pex(make_pex(funcs=[{"name": "run", "code": code}]))
        data = make_pex()
        with self.assertRaises(ValueError):
            pex(b"BAD!" + data[4:])

    def test_bsa_versions_compression_toggle_and_embedded_names(self):
        payload = make_pex()
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "scripts.bsa"
            for version, compressed, toggle in [(104, False, False), (105, False, False),
                                                 (104, True, False), (104, False, True),
                                                 (104, True, True)]:
                for embedded in (False, True):
                    path.write_bytes(make_bsa(payload, version, compressed, toggle, embedded))
                    self.assertEqual(list(bsa_scripts(path)), [("fixture", payload)])
            data = make_bsa(payload)
            for end in (0, 35, 40, 65, len(data) - 1):
                path.write_bytes(data[:end])
                with self.assertRaises(ValueError):
                    list(bsa_scripts(path))

    def test_bounded_inflate_and_trailing_stream(self):
        compressed = zlib.compress(b"abc")
        self.assertEqual(inflate(compressed, 3), b"abc")
        for data, expected in [(compressed, 2), (compressed, 4), (compressed + b"tail", 3),
                                (compressed[:-1], 3), (compressed, 100000000)]:
            with self.assertRaises(ValueError):
                inflate(data, expected)

    def test_plugin_groups_and_compressed_info(self):
        body = field(b"VMAD", make_vmad())
        compressed = struct.pack("<I", len(body)) + zlib.compress(body)
        data = group(record(b"INFO", 99, compressed, 0x40000), 7, 100)
        parsed = list(plugin_records(io.BytesIO(data), len(data)))
        self.assertEqual(parsed[0][4], ((7, 100),))
        self.assertEqual(parsed[0][3], [(b"VMAD", make_vmad())])
        with self.assertRaises(ValueError):
            list(plugin_records(io.BytesIO(data[:-1]), len(data) - 1))


class DependencyTests(unittest.TestCase):
    def test_load_order_owner_remap_and_deletion(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp)
            skyrim = record(b"TES4") + record(b"QUST", 42) + record(b"QUST", 43)
            skyrim += record(b"DIAL", 100, field(b"QNAM", struct.pack("<I", 42)))
            skyrim += group(record(b"INFO", 101, field(b"VMAD", make_vmad([vmad_script("talk")]))), 7, 100)
            (p / MASTERS[0]).write_bytes(skyrim)
            for name in MASTERS[1:]:
                (p / name).write_bytes(record(b"TES4", body=field(b"MAST", b"Skyrim.esm\0")))
            with (p / MASTERS[-1]).open("ab") as stream:
                stream.write(record(b"DIAL", 100, field(b"QNAM", struct.pack("<I", 43))))
                stream.write(record(b"QUST", 42, flags=0x20))
            index, sources = load_records(p)
            result = analyze(index, pex(make_pex("talk")), REGISTRY)
            rows = {q["id"]: q for q in result["quests"]}
            self.assertEqual(rows["Skyrim.esm:00002A"]["status"], "excluded-deleted")
            self.assertEqual(rows["Skyrim.esm:00002B"]["scriptRoots"], ["talk"])
            self.assertEqual(result["summary"]["certifiedPlayable"], 0)
            self.assertEqual(len(sources), 5)

    def test_master_mapping_and_bad_id(self):
        self.assertEqual(stable_form(0x01000042, ["Skyrim.esm", "Update.esm"]), "Update.esm:000042")
        self.assertIsNone(stable_form(0, ORIGINS))
        with self.assertRaises(ValueError):
            stable_form(0xff000042, ORIGINS)

    def test_alias_create_structure_is_not_a_form_id(self):
        q = summary(values=[(b"ALST", struct.pack("<I", 3)),
                            (b"ALCO", struct.pack("<I", 55)),
                            (b"ALCA", struct.pack("<hH", -1, 0x8000)), (b"ALED", b"")])
        self.assertEqual(q["refs"], {"Skyrim.esm:000037"})
        self.assertIn("alias.fill.created-reference", q["features"])
        for values in [[(b"ALST", struct.pack("<I", 3))], [(b"ALED", b"")]]:
            with self.assertRaises(ValueError):
                summary(values=values)

    def test_player_reference_is_campaign_binding_not_missing_asset(self):
        q = summary(values=[(b"VMAD", make_vmad([vmad_script("root", [
            ("Player", 1, 0, vmad_object(0x14))])]))])
        r = analyze({q["id"]: q}, pex(make_pex("root")), REGISTRY)
        self.assertEqual(r["quests"][0]["unknowns"], [])
        self.assertIn("world.campaign-player", [c["id"] for c in r["capabilities"]])

    def test_scene_parent_and_package_action_not_confused(self):
        q, scene, package = summary(), summary("SCEN", 100, [
            (b"PNAM", struct.pack("<I", 101)), (b"PNAM", struct.pack("<I", 42))]), summary("PACK", 101)
        result = analyze({r["id"]: r for r in (q, scene, package)}, {}, REGISTRY)
        caps = {c["id"] for c in result["quests"][0]["capabilities"]}
        self.assertTrue({"engine.scenes", "engine.packages"} <= caps)
        self.assertEqual(result["quests"][0]["recordCount"], 3)

    def test_missing_script_and_vmad_failure_remain_unknown(self):
        for value in [make_vmad([vmad_script("missing")]), b"bad"]:
            q = summary(values=[(b"VMAD", value)])
            result = analyze({q["id"]: q}, {}, REGISTRY)
            self.assertTrue(result["quests"][0]["unknowns"])
            self.assertFalse(result["quests"][0]["analysisComplete"])
            self.assertEqual(result["quests"][0]["status"], "untested")

    def test_condition_context_is_separate_from_function_support(self):
        condition = bytearray(32)
        struct.pack_into("<H", condition, 8, 58)
        struct.pack_into("<I", condition, 20, 5)
        q = summary(values=[(b"CTDA", bytes(condition))])
        result = analyze({q["id"]: q}, {}, REGISTRY)
        caps = {c["id"]: c["status"] for c in result["capabilities"]}
        self.assertEqual(caps["condition:58"], "implemented-subset")
        self.assertEqual(caps["condition.run-on:5"], "missing")
        self.assertEqual(caps["condition.non-stage-evaluation"], "missing")

    def test_transitive_calls_parent_cycles_and_native_subsets(self):
        objects = {}
        objects.update(pex(make_pex("root", "base", funcs=[{
            "name": "run", "types": {"q": "quest"}, "code": [
                (24, [(1, "work"), (1, "none"), (3, 0)]),
                (23, [(1, "getstage"), (1, "q"), (1, "result"), (3, 0)]),
                (23, [(1, "run"), (1, "self"), (1, "none"), (3, 0)]),
            ]}])))
        objects.update(pex(make_pex("base", funcs=[{"name": "work", "code": [
            (25, [(1, "utility"), (1, "wait"), (1, "none"), (3, 0)])]}])))
        objects.update(pex(make_pex("utility", funcs=[{"name": "wait", "flags": 3}])))
        traced = ScriptGraph(objects, REGISTRY).dependencies({"root"})
        self.assertEqual(set(traced["features"]), {"native:utility.wait", "native:quest.getstage"})
        self.assertEqual(traced["unknowns"], set())
        self.assertTrue(traced["dynamicSites"])
        self.assertEqual(capability_status("native:quest.getstage", REGISTRY), "implemented-subset")
        self.assertEqual(capability_status("native:utility.wait", REGISTRY), "missing")

    def test_unused_native_base_helpers_do_not_inflate_impact(self):
        objects = pex(make_pex("root", "quest", funcs=[{"name": "run"}]))
        objects.update(pex(make_pex("quest", funcs=[
            {"name": "getstage", "flags": 2},
            {"name": "unused", "code": [(25, [(1, "missing"), (1, "do"), (1, "none"), (3, 0)])]}
        ])))
        traced = ScriptGraph(objects, REGISTRY).dependencies({"root"})
        self.assertEqual(traced["unknowns"], set())
        self.assertEqual(traced["features"], {})

    def test_auto_property_does_not_create_fake_native(self):
        objects = pex(make_pex("root", props={"target": "quest"}, funcs=[{"name": "run", "code": [
            (28, [(1, "target"), (1, "self"), (1, "result")])]}]))
        traced = ScriptGraph(objects, REGISTRY).dependencies({"root"})
        self.assertFalse(traced["unknowns"])
        self.assertFalse(traced["features"])

    def test_unresolved_receiver_and_method_are_explicit(self):
        objects = pex(make_pex("root", funcs=[{"name": "run", "code": [
            (23, [(1, "run"), (1, "unknown"), (1, "none"), (3, 0)]),
            (23, [(1, "missing"), (1, "self"), (1, "none"), (3, 0)])]}]))
        traced = ScriptGraph(objects, REGISTRY).dependencies({"root"})
        self.assertTrue(any("unknown-receiver" in u for u in traced["unknowns"]))
        self.assertTrue(any("unresolved-call" in u for u in traced["unknowns"]))

    def test_impact_deduplicates_quests_and_retains_evidence(self):
        objects = pex(make_pex("root", funcs=[{"name": "run", "code": [
            (25, [(1, "utility"), (1, "wait"), (1, "none"), (3, 0)]),
            (25, [(1, "utility"), (1, "wait"), (1, "none"), (3, 0)])]}]))
        objects.update(pex(make_pex("utility", funcs=[{"name": "wait", "flags": 3}])))
        q = summary(values=[(b"VMAD", make_vmad([vmad_script("root")]))])
        result = analyze({q["id"]: q}, objects, REGISTRY)
        native = next(c for c in result["capabilities"] if c["id"] == "native:utility.wait")
        self.assertEqual(native["questCount"], 1)
        self.assertEqual(result["quests"][0]["capabilities"][0]["evidenceCount"], 2)

    def test_script_precedence_and_parse_failure_do_not_fall_back(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            loose = root / "loose"
            loose.mkdir()
            (loose / "fixture.pex").write_bytes(b"invalid")
            archive = root / "archive.bsa"
            archive.write_bytes(make_bsa(make_pex()))
            objects, sources, failures, overrides = load_scripts([archive], [loose])
            self.assertFalse(objects)
            self.assertIn("fixture", failures)
            self.assertEqual(overrides[0]["winner"], "loose")
            self.assertEqual(len(sources), 2)

    def test_loose_script_inputs_must_exist_and_have_unique_names(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            with self.assertRaisesRegex(ValueError, "Missing loose-script"):
                load_scripts([], [root / "missing"])
            (root / "Fixture.pex").write_bytes(make_pex())
            (root / "fixture.PEX").write_bytes(make_pex())
            # A case-insensitive filesystem cannot create both conflicting files.
            if len(list(root.iterdir())) == 2:
                with self.assertRaisesRegex(ValueError, "Ambiguous script"):
                    load_scripts([], [root])

    def test_capability_source_changes_require_review(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / "runtime.cpp"
            source.write_bytes(b"version1\n")
            registry = root / "caps.json"
            registry.write_text(json.dumps({"schemaVersion": 1,
                                           "sourceHashes": {"runtime.cpp": fingerprint(source)["sha256"]}}))
            read_registry(registry, root)
            source.write_bytes(b"version1\r\n")
            read_registry(registry, root)
            source.write_text("version2")
            with self.assertRaisesRegex(ValueError, "audit is stale"):
                read_registry(registry, root)

    def test_cross_quest_links_are_not_silently_expanded(self):
        first = summary(values=[(b"VMAD", make_vmad([vmad_script("first", [
            ("OtherQuest", 1, 0, vmad_object(43))])]))])
        second = summary(form=43, values=[(b"ALST", struct.pack("<I", 0)), (b"ALED", b"")])
        result = analyze({q["id"]: q for q in [first, second]}, pex(make_pex("first")), REGISTRY)
        first_row = result["quests"][0]
        self.assertEqual(first_row["questDependencies"], ["Skyrim.esm:00002B"])
        self.assertNotIn("alias.reference", {c["id"] for c in first_row["capabilities"]})
        self.assertEqual(result["capabilities"][0]["questCount"], 1)

    def test_recursive_inheritance_fails_explicitly(self):
        objects = pex(make_pex("first", "second"))
        objects.update(pex(make_pex("second", "first")))
        with self.assertRaisesRegex(ValueError, "inheritance cycle"):
            ScriptGraph(objects, REGISTRY).dependencies({"first"})

    def test_unindexed_record_ids_are_reported(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp)
            for name in MASTERS:
                (p / name).write_bytes(record(b"TES4"))
            with (p / MASTERS[0]).open("ab") as stream:
                stream.write(record(b"GMST", 0x01000042))
            _, sources = load_records(p)
            self.assertEqual(sources[0]["unindexedRecords"][0]["form"], "01000042")

    def test_checked_in_registry_matches_runtime(self):
        read_registry(Path(__file__).with_name("quest_capabilities.json"))


if __name__ == "__main__":
    unittest.main()
