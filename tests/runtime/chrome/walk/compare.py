"""A throwaway comparison of two walks for darionxt/mitto#207, never merged.

    python compare.py A.json B.json          what differs between two walks
    python compare.py --summary A.json ...   the size, cost and oddities of each walk

Nodes are matched by the path at which the walk first found them, and a reference to another node is written as that
node's path, so two walks compare although their nodes are numbered differently.
"""

import argparse
import json
import pathlib
from collections import Counter
from typing import Any


def load(path: pathlib.Path) -> dict[str, Any]:
    walk = json.loads(path.read_text(encoding="utf-8"))
    if "error" in walk:
        raise SystemExit(f"{path}: the walk failed: {walk['error']}")
    paths = [node["p"] for node in walk["nodes"]]

    def resolve(value: Any) -> Any:
        if isinstance(value, list) and value and value[0] == "o":
            return ["o", paths[value[1]]]
        return value

    by_path = {}
    for node in walk["nodes"]:
        own = {}
        order = []
        for entry in node.get("own", []):
            name, attributes, *values = entry
            own[name] = [attributes, *(resolve(v) for v in values)]
            order.append(name)
        reads = {name: resolve(value) for name, value in node.get("rd", [])}
        by_path[node["p"]] = {
            "kind": node["k"],
            "tag": node.get("t"),
            "proto": resolve(node.get("pr")),
            "function": node.get("f"),
            "own": own,
            "order": order,
            "reads": reads,
        }
    walk["by_path"] = by_path
    return walk


def short(value: Any, limit: int = 160) -> str:
    text = json.dumps(value, ensure_ascii=False)
    return text if len(text) <= limit else text[:limit] + "..."


def compare(a: dict[str, Any], b: dict[str, Any], limit: int) -> list[str]:
    lines = []
    pa, pb = a["by_path"], b["by_path"]
    ga, gb = pa["globalThis"], pb["globalThis"]
    only_a = [k for k in ga["order"] if k not in gb["own"]]
    only_b = [k for k in gb["order"] if k not in ga["own"]]
    lines.append(f"globals: {len(ga['order'])} against {len(gb['order'])}")
    if only_a:
        lines.append(f"  only in A ({len(only_a)}): {' '.join(only_a)}")
    if only_b:
        lines.append(f"  only in B ({len(only_b)}): {' '.join(only_b)}")
    common_a = [k for k in ga["order"] if k in gb["own"]]
    common_b = [k for k in gb["order"] if k in ga["own"]]
    if common_a != common_b:
        lines.append("  the globals they share come in another order")

    def parent_shared(path: str) -> bool:
        parent = path.rsplit(".", 1)[0]
        return parent in pa and parent in pb

    missing_b = [p for p in pa if p not in pb and parent_shared(p)]
    missing_a = [p for p in pb if p not in pa and parent_shared(p)]
    lines.append(f"nodes: {len(pa)} against {len(pb)}; the first of a missing subtree: {len(missing_b)} only in A, {len(missing_a)} only in B")
    for path in missing_b[:limit]:
        lines.append(f"  only in A: {path}")
    for path in missing_a[:limit]:
        lines.append(f"  only in B: {path}")

    members = []
    values = []
    reads = []
    for path, na in pa.items():
        nb = pb.get(path)
        if nb is None:
            continue
        if na["tag"] != nb["tag"] or na["proto"] != nb["proto"] or na["function"] != nb["function"]:
            members.append(f"  {path}: tag, prototype or function {short([na['tag'], na['proto'], na['function']])} -> {short([nb['tag'], nb['proto'], nb['function']])}")
        gone = [k for k in na["order"] if k not in nb["own"]]
        new = [k for k in nb["order"] if k not in na["own"]]
        if gone or new:
            members.append(f"  {path}: -{gone} +{new}")
        shared_a = [k for k in na["order"] if k in nb["own"]]
        shared_b = [k for k in nb["order"] if k in na["own"]]
        if shared_a != shared_b:
            members.append(f"  {path}: own properties in another order")
        for key in shared_a:
            if na["own"][key] != nb["own"][key]:
                values.append(f"  {path}.{key}: {short(na['own'][key])} -> {short(nb['own'][key])}")
        for key, value in na["reads"].items():
            if key in nb["reads"] and nb["reads"][key] != value:
                reads.append(f"  {path}.{key}: {short(value)} -> {short(nb['reads'][key])}")
        for key in na["reads"].keys() ^ nb["reads"].keys():
            reads.append(f"  {path}.{key}: read in only one")
    for title, items in (("members", members), ("own values and attributes", values), ("getter values", reads)):
        lines.append(f"{title}: {len(items)} differences")
        lines.extend(items[:limit])
        if len(items) > limit:
            lines.append(f"  ... {len(items) - limit} more")

    calls = []
    for name in sorted(a["calls"].keys() | b["calls"].keys()):
        ca, cb = a["calls"].get(name, {}), b["calls"].get(name, {})
        va = ca.get("v", ca.get("e"))
        vb = cb.get("v", cb.get("e"))
        if va != vb and isinstance(va, list) and isinstance(vb, list):
            gone = [x for x in va if x not in vb]
            new = [x for x in vb if x not in va]
            moved = " (same items in another order)" if not gone and not new else ""
            calls.append(f"  {name}: -{short(gone, 400)} +{short(new, 400)}{moved}")
        elif va != vb:
            calls.append(f"  {name}: {short(va, 400)} -> {short(vb, 400)}")
    lines.append(f"calls: {len(calls)} differences")
    lines.extend(calls)
    for key in ("reports", "throwing", "promises"):
        if a[key] != b[key]:
            lines.append(f"{key}: {short(a[key], 600)} -> {short(b[key], 600)}")
    return lines


def summary(path: pathlib.Path, walk: dict[str, Any]) -> list[str]:
    size = path.stat().st_size
    kinds = Counter(node["k"] for node in walk["nodes"])
    own = sum(len(node.get("own", [])) for node in walk["nodes"])
    read_count = sum(len(node.get("rd", [])) for node in walk["nodes"])
    lines = [
        f"{path.name}: {size} bytes, {len(walk['nodes'])} nodes {dict(kinds)}, {own} own properties, {read_count} getters read",
        f"  walk {walk['meta']['walkMs']} ms, with the calls {walk['meta']['totalMs']} ms, url {walk['meta']['url']}",
        f"  globals {len(walk['by_path']['globalThis']['order'])}",
        f"  reports {walk['reports']}",
        f"  slow getters {sorted(walk['slow'], key=lambda item: -item[1])[:15]}",
        f"  throwing getters {walk['throwing']}",
        f"  promises {walk['promises']}",
    ]
    deviations = []
    for node in walk["nodes"]:
        if node.get("f") and len(node["f"]) > 2:
            deviations.append(f"{node['p']}: {node['f'][2][:100]}")
        for entry in node.get("own", []):
            for value in entry[2:]:
                if isinstance(value, list) and value and value[0] == "F" and len(value) > 3:
                    deviations.append(f"{node['p']}.{entry[0]}: {value[3][:100]}")
    lines.append(f"  source text that is not [native code]: {deviations}")
    return lines


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("walks", nargs="+", type=pathlib.Path)
    parser.add_argument("--summary", action="store_true")
    parser.add_argument("--limit", type=int, default=60)
    args = parser.parse_args()
    if args.summary:
        for path in args.walks:
            print("\n".join(summary(path, load(path))))
        return
    if len(args.walks) != 2:
        raise SystemExit("Give two walks.")
    print("\n".join(compare(load(args.walks[0]), load(args.walks[1]), args.limit)))


if __name__ == "__main__":
    main()
