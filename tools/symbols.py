#!/usr/bin/env python3
"""Keeps a build's debug symbols aside, so what ships is small and crash reports can still be read
(tools/symbolicate.py).

    python3 tools/symbols.py split --out SYMBOLS_DIR FILE...

  - Linux (ELF): the debug info goes to SYMBOLS_DIR/.build-id/ab/cdef....debug (keyed by the GNU build id, the
    layout gdb and debuginfod use), the file is stripped of it (its symbol table stays: crash stacks still name
    functions) and points at the debug file (.gnu_debuglink). Needs objcopy and strip (binutils).
  - macOS (Mach-O): dsymutil makes SYMBOLS_DIR/<name>.dSYM, then strip -S. Needs the Xcode command line tools.
  - Windows (PE): the .pdb next to the file is moved to SYMBOLS_DIR with a copy of the file itself
    (llvm-symbolizer reads both).

Files that aren't programs or libraries are left alone. Prints what it kept.
"""

import argparse
import os
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from symbolicate import elf_build_id   # noqa: E402


def kind(path):
    with open(path, "rb") as f:
        head = f.read(4)
    if head == b"\x7fELF":
        return "elf"
    if head in (b"\xcf\xfa\xed\xfe", b"\xce\xfa\xed\xfe", b"\xca\xfe\xba\xbe"):
        return "macho"
    if head[:2] == b"MZ":
        return "pe"
    return None


def run(cmd):
    subprocess.run(cmd, check=True)


def split_elf(path, out):
    build_id = elf_build_id(path)
    if not build_id:
        print(f"{path}: no build id (link with -Wl,--build-id): left as it is", file=sys.stderr)
        return False
    debug = os.path.join(out, ".build-id", build_id[:2], build_id[2:] + ".debug")
    os.makedirs(os.path.dirname(debug), exist_ok=True)
    run(["objcopy", "--only-keep-debug", path, debug])
    run(["strip", "--strip-debug", path])
    run(["objcopy", "--remove-section=.gnu_debuglink", path])
    run(["objcopy", "--add-gnu-debuglink=" + debug, path])
    print(f"{path}: {debug}")
    return True


def split_macho(path, out):
    dsym = os.path.join(out, os.path.basename(path) + ".dSYM")
    run(["dsymutil", path, "-o", dsym])
    run(["strip", "-S", path])
    print(f"{path}: {dsym}")
    return True


def split_pe(path, out):
    pdb = os.path.splitext(path)[0] + ".pdb"
    if not os.path.isfile(pdb):
        print(f"{path}: no .pdb next to it (build with /Zi and link with /DEBUG)", file=sys.stderr)
        return False
    os.makedirs(out, exist_ok=True)
    shutil.move(pdb, os.path.join(out, os.path.basename(pdb)))
    shutil.copy2(path, os.path.join(out, os.path.basename(path)))
    print(f"{path}: {os.path.join(out, os.path.basename(pdb))}")
    return True


def main():
    parser = argparse.ArgumentParser(description="Keeps debug symbols aside and strips what ships")
    sub = parser.add_subparsers(dest="command", required=True)
    split = sub.add_parser("split", help="move FILEs' debug info into the symbols folder")
    split.add_argument("--out", required=True, help="the symbols folder")
    split.add_argument("files", nargs="+")
    args = parser.parse_args()
    os.makedirs(args.out, exist_ok=True)
    kept = 0
    for f in args.files:
        if not os.path.isfile(f) or os.path.islink(f):
            continue
        k = kind(f)
        if k == "elf":
            kept += split_elf(f, args.out)
        elif k == "macho":
            kept += split_macho(f, args.out)
        elif k == "pe":
            kept += split_pe(f, args.out)
    print(f"{kept} file(s): symbols in {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
