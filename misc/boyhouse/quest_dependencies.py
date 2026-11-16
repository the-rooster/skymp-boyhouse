"""Catalog-wide, conservative dependency analysis for the isolated campaign VM.

Static dependency discovery is not a playthrough or a claim of support. All
attached script functions/states are potential roots. Declared receiver types
are used; dynamic dispatch, runtime forms and native-engine dependencies remain
explicit gaps. Owned script bodies and dialogue text are never emitted.
"""
import argparse
from collections import Counter, defaultdict, deque
import hashlib
import json
from pathlib import Path
import struct

from quest_catalog import MASTERS
from quest_formats import bsa_scripts, pex, plugin_records, vmad

ROOT = Path(__file__).resolve().parents[2]
# Only known FormID-valued fields. ALCA is a signed alias + create flags, NOT a form.
ALIAS_FORMS = {b"ALFR", b"ALUA", b"ALRT", b"ALEQ", b"ALCO", b"ALFL",
               b"KNAM", b"ALSP", b"ALFC", b"ALPC", b"VTCK", b"ALDN"}
FILL_TYPES = {b"ALFR": "forced-reference", b"ALUA": "unique-actor",
              b"ALFA": "location-or-alias", b"ALEQ": "external-alias",
              b"ALCO": "created-reference", b"ALNA": "matching-reference",
              b"ALFE": "event", b"ALFL": "specific-location"}
FOLLOW_TYPES = {"PACK", "SCEN", "DIAL", "INFO", "SMQN", "SMBN", "SMEN"}
GLOBAL_GAPS = [
    "Static discovery cannot prove branch reachability, correct behavior or playability.",
    "Declared receiver types do not enumerate all virtual overrides or runtime object types.",
    "Runtime-created forms, computed IDs, native-engine side effects and dynamic events need traces.",
    "World-reference/asset record scripts and non-script relationships are not traversed yet.",
    "Cross-quest record links are reported separately, not expanded into every linked quest.",
    "Client projection, phase transitions, lifecycle, persistence and real-client validation remain gates.",
]


def fingerprint(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return {"file": path.name, "bytes": path.stat().st_size, "sha256": digest.hexdigest()}


def stable_form(raw, origins):
    if raw == 0:
        return None
    index = raw >> 24
    if index >= len(origins):
        raise ValueError(f"Invalid master index for {raw:08X}")
    return f"{origins[index]}:{raw & 0xffffff:06X}"


def read_registry(path, root=ROOT):
    registry = json.loads(path.read_text())
    if registry.get("schemaVersion") != 1:
        raise ValueError("Unsupported capability registry")
    for rel, expected in registry["sourceHashes"].items():
        # Git's Windows checkout conversion must not invalidate a source audit.
        normalized = (root / rel).read_bytes().replace(b"\r\n", b"\n")
        if hashlib.sha256(normalized).hexdigest() != expected:
            raise ValueError(f"Capability audit is stale: review {rel} and update sourceHashes")
    return registry


def summarize_record(typ, flags, form, values, groups, origins, plugin):
    key = stable_form(form, origins)
    result = {"id": key, "type": typ, "deleted": bool(flags & 0x20),
              "plugin": plugin, "editorId": "", "roots": set(), "refs": set(),
              "features": defaultdict(set), "unknowns": set(), "owners": set(),
              "stages": set(), "objectives": set(), "bindings": []}
    stage, context, alias, fill, vmad_seen = None, "record", None, False, False

    def feature(name, where):
        result["features"][name].add(where)

    def ref(raw, where):
        target = stable_form(raw, origins)
        if target:
            result["refs"].add(target)
        return target

    def u32(data):
        if len(data) != 4:
            raise ValueError("Expected four-byte record field")
        return struct.unpack("<I", data)[0]

    for tag, data in values:
        where = f"{key}/{context}/{tag.decode('ascii')}"
        if tag == b"EDID":
            result["editorId"] = data.rstrip(b"\0").decode("utf-8")
        elif tag == b"INDX" and typ == "QUST":
            if len(data) < 2:
                raise ValueError("Truncated quest stage")
            stage = struct.unpack_from("<H", data)[0]
            result["stages"].add(stage)
            context = f"stage:{stage}"
        elif tag == b"QOBJ" and typ == "QUST":
            if len(data) != 2:
                raise ValueError("Invalid objective index")
            objective = struct.unpack("<H", data)[0]
            result["objectives"].add(objective)
            context = f"objective:{objective}"
        elif tag == b"ANAM" and typ == "QUST":
            context = "aliases"
        elif tag in (b"ALST", b"ALLS") and typ == "QUST":
            if alias is not None:
                raise ValueError("Nested quest alias")
            alias, fill = u32(data), False
            context = f"alias:{alias}"
            feature("alias.reference" if tag == b"ALST" else "alias.location", where)
        elif tag == b"ALED" and typ == "QUST":
            if alias is None or data:
                raise ValueError("Invalid quest alias end")
            if not fill:
                feature("alias.fill.matching-or-script", where)
            alias, context = None, "record"
        elif alias is not None and tag in FILL_TYPES:
            fill = True
            feature("alias.fill." + FILL_TYPES[tag], where)
            if tag in ALIAS_FORMS:
                ref(u32(data), where)
        elif alias is not None and tag in ALIAS_FORMS:
            ref(u32(data), where)
            if tag == b"ALPC":
                feature("engine.packages", where)
        elif tag == b"VMAD":
            if vmad_seen:
                raise ValueError("Duplicate VMAD")
            vmad_seen = True
            try:
                metadata = vmad(data, typ)
            except ValueError as exc:
                result["unknowns"].add(f"{key}: {exc}")
                continue
            scripts = list(metadata["scripts"])
            for binding in metadata["aliases"]:
                feature("alias.scripts", where)
                ref(binding["object"]["form"], where)
                scripts.extend(binding["scripts"])
            for fragment in metadata["fragments"]:
                result["roots"].add(fragment["script"])
                result["bindings"].append(fragment)
            for script in scripts:
                result["roots"].add(script["name"])
                for prop in script["properties"]:
                    for value in prop["references"]:
                        ref(value["form"], where)
                        if value["form"] and value["alias"] != 0xffff:
                            feature("alias.properties", f"{key}/{script['name']}.{prop['name']}")
        elif tag == b"CTDA":
            if len(data) != 32:
                raise ValueError("Invalid CTDA size")
            op = data[0]
            index = struct.unpack_from("<H", data, 8)[0]
            run_on, reference = struct.unpack_from("<II", data, 20)
            feature(f"condition:{index}", where)
            if op & 0x1e or op >> 5 > 5:
                feature("condition.flags-or-global-comparison", where)
            if run_on != 0 and not (run_on == 2 and stable_form(reference, origins) == "Skyrim.esm:000014"):
                feature(f"condition.run-on:{run_on}", where)
            if not context.startswith("stage:") or typ != "QUST":
                feature("condition.non-stage-evaluation", where)
            if index in (56, 58, 59, 74, 543) and not op & 0x0a:
                ref(struct.unpack_from("<I", data, 12)[0], where)
        elif tag == b"QSDT" and typ == "QUST":
            if len(data) != 1:
                raise ValueError("Invalid quest log flags")
            if data[0] & ~1:
                feature("quest.log-flags", where)
        elif typ == "DIAL" and tag == b"QNAM":
            owner = stable_form(u32(data), origins)
            if owner:
                result["owners"].add(owner)
        elif typ == "SCEN" and tag == b"PNAM":
            # Both package actions and the parent quest use PNAM; resolve by record type later.
            ref(u32(data), where)
        elif typ == "SCEN" and tag == b"DATA" and len(data) == 4:
            ref(u32(data), where)
        elif typ == "SMQN" and tag == b"NNAM":
            owner = stable_form(u32(data), origins)
            if owner:
                result["owners"].add(owner)
        elif typ in ("SMQN", "SMBN", "SMEN") and tag == b"PNAM":
            ref(u32(data), where)
    if alias is not None:
        raise ValueError("Unterminated quest alias")
    if typ == "INFO":
        parents = [label for kind, label in groups if kind == 7]
        if parents:
            result["topic"] = stable_form(parents[-1], origins)
        else:
            result["unknowns"].add(f"{key}: INFO has no parent topic group")
    for kind, cap in {"DIAL": "engine.dialogue", "INFO": "engine.dialogue",
                      "SCEN": "engine.scenes", "PACK": "engine.packages",
                      "SMQN": "engine.story-manager", "SMBN": "engine.story-manager",
                      "SMEN": "engine.story-manager"}.items():
        if typ == kind:
            feature(cap, key)
    return result


def load_records(data_dir):
    index, sources = {}, []
    for filename in MASTERS:
        path = data_dir / filename
        source = fingerprint(path)
        counts = Counter()
        unindexed = []
        origins = None
        with path.open("rb") as stream:
            for typ, flags, form, values, groups in plugin_records(stream, path.stat().st_size):
                if typ == "TES4":
                    origins = [v.rstrip(b"\0").decode("utf-8") for k, v in values if k == b"MAST"] + [filename]
                    continue
                if origins is None:
                    raise ValueError(f"{filename} has no TES4 header")
                counts[typ] += 1
                try:
                    key = stable_form(form, origins)
                except ValueError as exc:
                    unindexed.append({"type": typ, "form": f"{form:08X}", "reason": str(exc)})
                    continue
                if typ not in {"QUST", "DIAL", "INFO", "SCEN", "PACK", "SMQN", "SMBN", "SMEN"}:
                    if key:
                        index[key] = {"id": key, "type": typ, "deleted": bool(flags & 0x20)}
                    continue
                record = summarize_record(typ, flags, form, values, groups, origins, filename)
                if record["id"]:
                    index[record["id"]] = record
        source["recordCounts"] = dict(sorted(counts.items()))
        source["unindexedRecords"] = unindexed
        sources.append(source)
    return index, sources


class ScriptGraph:
    def __init__(self, objects, registry):
        self.objects, self.registry = objects, registry
        self.natives = set(registry["nativeSubsets"])

    def lineage(self, name):
        seen = set()
        while name:
            if name in seen:
                raise ValueError(f"Script inheritance cycle: {name}")
            seen.add(name)
            yield name
            obj = self.objects.get(name)
            if not obj:
                break
            name = obj["parent"]

    def resolve(self, cls, method, prop=False):
        for ancestor in self.lineage(cls):
            key = ancestor + "." + method
            if not prop and key in self.natives:
                return [("native", key)]
            obj = self.objects.get(ancestor)
            if not obj:
                return [("unknown", f"missing-script:{ancestor}")]
            matches = [i for i, f in enumerate(obj["functions"]) if f["name"] == method]
            if matches:
                return [("function", (ancestor, i)) for i in matches]
            if prop:
                pname, suffix = method[1:].split(":")
                definition = obj["properties"].get(pname)
                if definition and definition["flags"] & 4:
                    return []  # auto property; data binding remains a separate dependency
        return [("unknown", f"unresolved-call:{cls}.{method}")]

    def dependencies(self, roots):
        queue, visited, caps, unknowns, scripts = deque(), set(), defaultdict(set), set(), set()
        dynamic_sites = set()
        for root in sorted(roots):
            if root not in self.objects:
                unknowns.add(f"missing-script:{root}")
                continue
            # Every attached function/state can be reached by engine events or external calls.
            for ancestor in self.lineage(root):
                obj = self.objects.get(ancestor)
                if not obj:
                    unknowns.add(f"missing-script:{ancestor}")
                    break
                engine_base = any(f["flags"] & 2 for f in obj["functions"])
                queue.extend(("function", (ancestor, i), f"attached:{root}")
                             for i, f in enumerate(obj["functions"])
                             if not f["flags"] & 2 and
                             (ancestor == root or not engine_base or f["name"].startswith("on")))
        while queue:
            kind, target, evidence = queue.popleft()
            if kind == "unknown":
                unknowns.add(f"{target} at {evidence}")
                continue
            if kind == "native":
                caps["native:" + target].add(evidence)
                continue
            cls, i = target
            scripts.add(cls)
            obj = self.objects[cls]
            fn = obj["functions"][i]
            where = f"{cls}.{fn['name']}[{fn['state']}]"
            if fn["flags"] & 2:
                caps["native:" + cls + "." + fn["name"]].add(evidence)
                continue
            if target in visited:
                continue
            visited.add(target)
            if fn["name"].startswith("on"):
                caps["engine.events"].add(where)
            types = {}
            for ancestor in reversed(list(self.lineage(cls))):
                if ancestor in self.objects:
                    types.update(self.objects[ancestor]["types"])
            types.update(fn["types"])
            types["self"] = cls
            for call in fn["calls"]:
                op = call["op"]
                a, b = call["operands"]
                site = f"{where}:{call['instruction']}"
                if a[0] != 1:
                    unknowns.add(f"non-symbolic-call at {site}")
                    continue
                if op == 25:
                    if b[0] != 1:
                        unknowns.add(f"non-symbolic-static-call at {site}")
                        continue
                    receiver, method = a[1], b[1]
                elif op == 24:
                    receiver, method = obj["parent"], a[1]
                else:
                    receiver = types.get(b[1], "") if b[0] == 1 else ""
                    if b != (1, "self"):
                        dynamic_sites.add(site)
                    method = a[1]
                    if op in (28, 29):
                        method = f"@{method}:{'get' if op == 28 else 'set'}"
                if not receiver:
                    unknowns.add(f"unknown-receiver:{method} at {site}")
                    continue
                for next_kind, next_target in self.resolve(receiver, method, op in (28, 29)):
                    queue.append((next_kind, next_target, site))
        return {"features": caps, "unknowns": unknowns, "scripts": scripts, "dynamicSites": dynamic_sites}


def load_scripts(archives, loose_dirs=()):
    objects, sources, failures, winners, overrides = {}, [], {}, {}, []
    # Runtime precedence: loose files first, then archives in the supplied order.
    inputs = []
    for directory in loose_dirs:
        if not directory.is_dir():
            raise ValueError(f"Missing loose-script directory: {directory}")
        entries = sorted((p for p in directory.iterdir() if p.is_file() and p.suffix.lower() == ".pex"),
                         key=lambda p: (p.name.lower(), p.name))
        inputs.append((directory, ((p.stem.lower(), p.read_bytes()) for p in entries), "loose"))
    inputs.extend((path, bsa_scripts(path), "archive") for path in archives)
    for path, entries, kind in inputs:
        source = fingerprint(path) if kind == "archive" else {"file": path.name, "kind": "loose", "scripts": []}
        count, source_names = 0, set()
        for name, data in entries:
            if name in source_names:
                raise ValueError(f"Ambiguous script name in {path.name}: {name}")
            source_names.add(name)
            count += 1
            if kind == "loose":
                source["scripts"].append({"name": name, "sha256": hashlib.sha256(data).hexdigest()})
            if name in winners:
                overrides.append({"script": name, "winner": winners[name], "shadowed": path.name})
                continue
            winners[name] = path.name
            try:
                parsed = pex(data)
                if set(parsed) != {name}:
                    raise ValueError("PEX object name differs from filename or has multiple objects")
                objects.update(parsed)
            except ValueError as exc:
                failures[name] = str(exc)
        source["scriptCount"] = count
        sources.append(source)
    return objects, sources, failures, overrides


def capability_status(cap, registry):
    if cap.startswith("native:"):
        return "implemented-subset" if cap[7:] in registry["nativeSubsets"] else "missing"
    if cap.startswith("condition:"):
        return "implemented-subset" if cap[10:] in registry["conditionSubsets"] else "missing"
    return "missing"


def family(cap):
    if cap.startswith("native:"):
        cls, method = cap[7:].split(".", 1)
        if any(t in method for t in ("register", "unregister", "wait")):
            return "events-and-latent-execution"
        if cls in ("quest", "alias", "referencealias", "locationalias"):
            return "quests-and-aliases"
        if cls in ("actor", "objectreference"):
            return "actors-and-world-effects"
        return cls
    return cap.split(".")[0].split(":")[0]


def analyze(index, objects, registry, parse_failures=None):
    parse_failures = parse_failures or {}
    children = defaultdict(set)
    for rec in index.values():
        if rec["deleted"]:
            continue
        owners = set(rec.get("owners", ()))
        if rec["type"] == "SCEN":
            owners.update(k for k in rec["refs"] if index.get(k, {}).get("type") == "QUST")
        if rec["type"] == "INFO":
            topic = index.get(rec.get("topic"))
            if topic and not topic["deleted"]:
                owners.update(topic["owners"])
            else:
                rec["unknowns"].add(f"{rec['id']}: missing or deleted parent topic")
        for owner in owners:
            if index.get(owner, {}).get("type") == "QUST":
                children[owner].add(rec["id"])
    graph = ScriptGraph(objects, registry)
    impact, rows, unresolved = defaultdict(set), [], Counter()
    for key, quest in sorted(index.items()):
        if quest["type"] != "QUST":
            continue
        if quest["deleted"]:
            rows.append({"id": key, "editorId": quest["editorId"], "status": "excluded-deleted",
                         "capabilities": [], "unknowns": []})
            continue
        queue, seen = deque([key] + sorted(children[key])), set()
        features, roots, unknowns, quest_deps, referenced = defaultdict(set), set(), set(), set(), set()
        while queue:
            target = queue.popleft()
            if target in seen:
                continue
            seen.add(target)
            rec = index.get(target)
            if not rec or rec["deleted"]:
                unknowns.add(f"missing-or-deleted-record:{target}")
                continue
            roots.update(rec["roots"])
            unknowns.update(rec["unknowns"])
            for cap, evidence in rec["features"].items():
                features[cap].update(evidence)
            for reference in sorted(rec["refs"]):
                referenced.add(reference)
                if reference == "Skyrim.esm:000014":
                    features["world.campaign-player"].add(target + " -> PlayerRef")
                    continue
                linked = index.get(reference)
                if not linked or linked["deleted"]:
                    unknowns.add(f"missing-or-deleted-record:{reference} from {target}")
                    continue
                typ = linked["type"]
                if typ == "QUST":
                    if reference != key:
                        quest_deps.add(reference)
                elif typ in FOLLOW_TYPES:
                    queue.append(reference)
                elif typ == "GLOB":
                    features["state.campaign-globals"].add(target + " -> " + reference)
                elif typ in ("ACHR", "REFR", "NPC_", "PLYR"):
                    features["world.campaign-references"].add(target + " -> " + reference)
                else:
                    features["forms.asset-bindings"].add(target + " -> " + reference)
        traced = graph.dependencies(roots)
        for cap, evidence in traced["features"].items():
            features[cap].update(evidence)
        unknowns.update(traced["unknowns"])
        for script in roots | traced["scripts"]:
            if script in parse_failures:
                unknowns.add(f"unparsed-script:{script}: {parse_failures[script]}")
        caps = []
        for cap, evidence in sorted(features.items()):
            status = capability_status(cap, registry)
            if cap == "state.campaign-globals":
                status = "implemented-subset"
            caps.append({"id": cap, "status": status, "evidence": sorted(evidence)[:5],
                         "evidenceCount": len(evidence)})
            impact[cap].add(key)
        for issue in unknowns:
            unresolved[issue.split(":", 1)[0]] += 1
        rows.append({"id": key, "editorId": quest["editorId"], "winningPlugin": quest["plugin"],
                     "status": "untested", "analysisComplete": False,
                     "stages": sorted(quest["stages"]), "objectives": sorted(quest["objectives"]),
                     "recordCount": len(seen), "scriptRoots": sorted(roots),
                     "runtimeDispatch": {"siteCount": len(traced["dynamicSites"]),
                                         "examples": sorted(traced["dynamicSites"])[:10]},
                     "tracedScripts": sorted(traced["scripts"]),
                     "questDependencies": sorted(quest_deps), "referencedRecords": sorted(referenced),
                     "capabilities": caps, "unknowns": sorted(unknowns)})
    ranked = []
    for cap, quests in impact.items():
        status = capability_status(cap, registry)
        if cap == "state.campaign-globals":
            status = "implemented-subset"
        label = cap
        if cap.startswith("condition:"):
            number = int(cap[10:])
            label = f"{registry.get("conditionNames", {}).get(str(number), 'Condition')} ({number})"
        ranked.append({"id": cap, "label": label, "family": family(cap), "status": status,
                       "questCount": len(quests), "quests": sorted(quests)})
    ranked.sort(key=lambda r: (r["status"] != "missing", -r["questCount"], r["id"]))
    return {"schemaVersion": 1,
            "scope": "Five owned masters; includes helper quests; conservative potential dependencies",
            "analysisLimitations": GLOBAL_GAPS,
            "summary": {"questRecords": len(rows), "deletedQuestRecords": sum(q["status"] == "excluded-deleted" for q in rows),
                        "certifiedPlayable": 0, "capabilities": len(ranked),
                        "questsWithUnknowns": sum(bool(q["unknowns"]) for q in rows),
                        "questsWithRuntimeDispatch": sum(bool(q.get("runtimeDispatch", {}).get("siteCount")) for q in rows),
                        "unknownCategories": dict(sorted(unresolved.items()))},
            "capabilities": ranked, "quests": rows}


def markdown(report):
    summary = report["summary"]
    lines = ["# Quest dependency backlog", "",
             "Generated static analysis. Counts include internal/helper quests and potential "
             "dependencies in all attached functions/states, including unreachable code. "
             "Counts overlap and do not predict how many quests a feature makes playable.", "",
             f"- Quest records: **{summary['questRecords']}**.",
             f"- Parsed PEX objects: **{report['scripts']['parsedObjects']}**.",
             f"- Quests with explicit unresolved dependencies: **{summary['questsWithUnknowns']}**.",
             f"- Quests with runtime-dependent dispatch sites: **{summary['questsWithRuntimeDispatch']}**.",
             f"- Record IDs not indexed: **{sum(len(s['unindexedRecords']) for s in report['sources'])}** (listed in JSON).",
             "- Certified playable quests: **0**. Every active catalog entry remains untested.", "",
             "## Missing shared systems", "",
             "| Capability | Quest records affected |",
             "| --- | ---: |"]
    for row in report["capabilities"]:
        if row["status"] == "missing" and not row["id"].startswith(("native:", "condition:")):
            lines.append(f"| {row['label']} | {row['questCount']} |")
    lines += ["", "## Most widely referenced missing native APIs", "",
              "| API | Quest records affected |", "| --- | ---: |"]
    for row in [r for r in report["capabilities"] if r["id"].startswith("native:") and r["status"] == "missing"][:30]:
        lines.append(f"| {row['id'][7:]} | {row['questCount']} |")
    lines += ["", "## Most widely referenced missing condition functions", "",
              "| Condition | Quest records affected |", "| --- | ---: |"]
    for row in [r for r in report["capabilities"] if r["id"].startswith("condition:") and r["status"] == "missing"][:20]:
        lines.append(f"| {row['label']} | {row['questCount']} |")
    lines += ["", "## Limits and next steps", ""]
    lines.extend("- " + gap for gap in report["analysisLimitations"])
    lines += ["", "Implement shared capabilities, then rerun this report and the regression suites. "
              "Use runtime traces and real playthroughs to resolve unknowns; a dependency report "
              "must never automatically enable a quest adapter.", "", "## Reproducibility", "",
              f"Capability registry SHA-256: {report['capabilityRegistrySha256']}", ""]
    for source in report["sources"] + report["scripts"]["sources"]:
        if "sha256" in source:
            lines.append(f"- {source['file']}: {source['sha256']}")
    lines.append("")
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data-dir", type=Path, required=True)
    parser.add_argument("--archive", type=Path, action="append", required=True,
                        help="Script archives in first-wins runtime order; repeat as needed")
    parser.add_argument("--scripts-dir", type=Path, action="append", default=[],
                        help="Optional loose PEX directories, before all archives")
    parser.add_argument("--registry", type=Path, default=Path(__file__).with_name("quest_capabilities.json"))
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--markdown", type=Path)
    args = parser.parse_args()
    registry = read_registry(args.registry)
    index, sources = load_records(args.data_dir)
    objects, script_sources, failures, overrides = load_scripts(args.archive, args.scripts_dir)
    report = analyze(index, objects, registry, failures)
    report["sources"] = sources
    report["capabilityRegistrySha256"] = fingerprint(args.registry)["sha256"]
    report["analyzerSources"] = [fingerprint(Path(__file__)),
                                 fingerprint(Path(__file__).with_name("quest_formats.py")),
                                 fingerprint(Path(__file__).with_name("quest_catalog.py"))]
    report["scripts"] = {"parsedObjects": len(objects), "parseFailures": failures,
                         "sources": script_sources, "overrides": overrides}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    if args.markdown:
        args.markdown.parent.mkdir(parents=True, exist_ok=True)
        args.markdown.write_text(markdown(report), encoding="utf-8")
    print(json.dumps({"output": str(args.output), **report["summary"],
                      "parsedScripts": len(objects), "scriptParseFailures": len(failures)}))


if __name__ == "__main__":
    main()
