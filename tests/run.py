#!/usr/bin/env python3
"""End-to-end tests for dirwhale. Creates a temp fixture, runs the binary, checks output."""
from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CANDIDATES = [ROOT / "dirwhale.exe", ROOT / "dirwhale"]
BIN = next((p for p in CANDIDATES if p.exists()), None)
if BIN is None:
    sys.exit("dirwhale binary not found; build it first (make / gcc)")

PASS = 0
FAIL = 0


def run(args, **kw):
    return subprocess.run(
        [str(BIN), *args],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        **kw,
    )


def check(name, cond, detail=""):
    global PASS, FAIL
    if cond:
        PASS += 1
        print(f"  PASS  {name}")
    else:
        FAIL += 1
        print(f"  FAIL  {name}{': ' + detail if detail else ''}")


def write_bytes(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def find_child(node, name):
    for c in node.get("children") or []:
        if c["name"] == name:
            return c
    return None


def main() -> int:
    print(f"binary: {BIN}")

    r = run(["--version"])
    check("version rc=0", r.returncode == 0, r.stderr)
    check("version text", "dirwhale 0.2.0" in r.stdout, r.stdout)

    r = run(["-h"])
    check("help rc=0", r.returncode == 0)
    check("help mentions quiet", "--quiet" in r.stderr)
    check("help mentions version", "--version" in r.stderr)

    r = run(["-d"])
    check("missing -d arg rc=1", r.returncode == 1)
    r = run(["-d", "abc"])
    check("bad -d rc=1", r.returncode == 1)
    r = run(["-z"])
    check("unknown option rc=1", r.returncode == 1)
    r = run(["nosuch-dirwhale-path"])
    check("missing path rc=1", r.returncode == 1)
    r = run([".", "."])
    check("two paths rc=1", r.returncode == 1)

    with tempfile.TemporaryDirectory() as td:
        base = Path(td)
        tree = base / "tree"
        write_bytes(tree / "a.txt", b"hello world")          # 11
        write_bytes(tree / "b.log", b"a" * 20)               # 20
        write_bytes(tree / "sub" / "big.bin", b"x" * 5000)   # 5000
        write_bytes(tree / "sub" / "deep" / "tiny.c", b"x")  # 1
        write_bytes(tree / "node_modules" / "x.js", b"js")   # 2
        write_bytes(tree / "中文.txt", b"cn")                 # 2
        write_bytes(tree / "emoji-🐳.txt", b"ew")             # 2
        (tree / "empty").mkdir()
        expected = 11 + 20 + 5000 + 1 + 2 + 2 + 2            # 5038

        r = run(["-q", str(tree)])
        out = r.stdout
        check("scan rc=0", r.returncode == 0, r.stderr)
        check("total files", ", 7 files]" in out, out.splitlines()[0] if out else "")
        check("total size line", out.startswith(str(tree)) or str(tree) in out.splitlines()[0], out[:200])
        check("lists sub/", "sub/" in out, out)
        check("lists empty/", "empty/" in out, out)
        check("chinese name", "中文.txt" in out, out)
        check("emoji name", "emoji-🐳.txt" in out, out)
        check("quiet hides scanning", "scanning " not in r.stderr, r.stderr)
        r_piped = run([str(tree)])
        check("piped stderr has no progress", "scanning " not in r_piped.stderr, r_piped.stderr)

        r = run(["-q", "-d", "0", str(tree)])
        body = [ln for ln in r.stdout.splitlines() if ln.startswith(" ") or ln.startswith("|") or ln.startswith("`")]
        check("depth 0 has no children", body == [], r.stdout)

        r = run(["-q", "-d", "2", "-n", "1", str(tree)])
        check("top-1 shows sub", "sub/" in r.stdout, r.stdout)
        check("top-1 hidden at root", "more entries" in r.stdout, r.stdout)
        check("top-1 hidden at sub", "more entry" in r.stdout, r.stdout)

        r = run(["-q", "-e", "node_modules", "-e", "*.bin", str(tree)])
        check("exclude node_modules", "node_modules" not in r.stdout, r.stdout)
        check("exclude *.bin", "big.bin" not in r.stdout, r.stdout)
        r = run(["-q", "-e", "*", str(tree)])
        check("exclude star is not CRT-globbed", r.returncode == 0 and ", 0 files]" in r.stdout, r.stdout + r.stderr)

        r = run(["-q", "-t", str(tree)])
        check("types header", "file types:" in r.stdout)
        check("types has bin", "bin" in r.stdout and "4.88 KiB" in r.stdout, r.stdout)

        json_path = base / "out.json"
        r = run(["-q", "-d", "0", "-j", str(json_path), str(tree)])
        check("json scan rc=0", r.returncode == 0, r.stderr)
        check("json written msg", "JSON written" in r.stdout)
        data = json.loads(json_path.read_text(encoding="utf-8"))
        check("json size", data["size"] == expected, f"{data['size']} != {expected}")
        check("json files", data["files"] == 7, str(data["files"]))
        check("json is_dir", data["is_dir"] is True)
        check("json keeps tree at depth 0", find_child(data, "sub") is not None)
        sub = find_child(data, "sub")
        check("json nested children", sub is not None and find_child(sub, "big.bin") is not None)
        check("json chinese", find_child(data, "中文.txt") is not None)
        check("json emoji", find_child(data, "emoji-🐳.txt") is not None)
        # children should be size-desc
        sizes = [c["size"] for c in data["children"]]
        check("json children sorted by size", sizes == sorted(sizes, reverse=True), str(sizes))

        r = run(["-q", str(tree / "a.txt")])
        check("single file rc=0", r.returncode == 0, r.stderr)
        check("single file size", "11 B" in r.stdout and "1 file]" in r.stdout, r.stdout)

        # many files -> top-N + hidden
        many = tree / "many"
        many.mkdir()
        for i in range(20):
            write_bytes(many / f"f{i:02d}.dat", b"m" * (100 - i))
        r = run(["-q", "-n", "3", str(many)])
        check("many top-3 hidden", "and 17 more entries" in r.stdout, r.stdout)

        if os.name == "nt":
            junc = tree / "junc"
            # junction pointing at empty/ should not recurse into a loop if pointed at tree
            rc = subprocess.run(
                ["cmd", "/c", "mklink", "/J", str(junc), str(tree / "empty")],
                capture_output=True, text=True,
            )
            if rc.returncode == 0:
                r = run(["-q", "-d", "2", str(tree)])
                check("junction listed", "junc/" in r.stdout or "junc" in r.stdout, r.stdout)
                # scanning the tree must finish (no infinite recursion)
                check("junction no hang rc=0", r.returncode == 0)
            else:
                print("  SKIP  junction (mklink failed — need privilege?)")

    print(f"\n{PASS} passed, {FAIL} failed")
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
