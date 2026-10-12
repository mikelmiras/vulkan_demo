#!/usr/bin/env python3
"""Turns the addresses in a crash report (core/crash.h) into functions, files and lines.

    python3 tools/symbolicate.py <name>-crash.txt [--symbols DIR]...

A report lists its stack (addresses) and the modules loaded when it crashed, each with its address range and build
id (Linux: the GNU build id; macOS: the image's UUID). For each frame this finds the module, then the symbols for
that exact build:

  - Linux: a debug file kept by tools/symbols.py (DIR/.build-id/ab/cdef....debug), else a file of the same name in
    DIR, else the module itself where the report says it was; any of them only if its build id matches. Resolved
    with addr2line (binutils) or llvm-symbolizer.
  - macOS: DIR/<name>.dSYM (dsymutil's, from tools/symbols.py), else the module itself; resolved with atos.
  - Windows reports name each frame "module+0xRVA" when there were no symbols on the player's machine: resolved
    with llvm-symbolizer from DIR/<module> (and its .pdb next to it).

Prints the report with " -> function at file:line" after each frame it could resolve.
"""

import argparse
import os
import re
import shutil
import struct
import subprocess
import sys

MODULE = re.compile(r"^\s+0x([0-9a-f]+)-0x([0-9a-f]+) (\S+) (.+)$")
FRAME = re.compile(r"^#\d+ ")


def elf_build_id(path):
    """The GNU build id of an ELF file (hex), or None"""
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError:
        return None
    if data[:4] != b"\x7fELF":
        return None
    is64 = data[4] == 2
    end = "<" if data[5] == 1 else ">"
    if is64:
        shoff, = struct.unpack_from(end + "Q", data, 0x28)
        shentsize, shnum = struct.unpack_from(end + "HH", data, 0x3A)
    else:
        shoff, = struct.unpack_from(end + "I", data, 0x20)
        shentsize, shnum = struct.unpack_from(end + "HH", data, 0x2E)
    for i in range(shnum):
        at = shoff + i * shentsize
        if is64:
            sh_type, = struct.unpack_from(end + "I", data, at + 4)
            offset, size = struct.unpack_from(end + "QQ", data, at + 24)
        else:
            sh_type, = struct.unpack_from(end + "I", data, at + 4)
            offset, size = struct.unpack_from(end + "II", data, at + 16)
        if sh_type != 7:   # SHT_NOTE
            continue
        p = offset
        while p + 12 <= offset + size:
            namesz, descsz, ntype = struct.unpack_from(end + "III", data, p)
            name = data[p + 12:p + 12 + namesz]
            desc_at = p + 12 + ((namesz + 3) & ~3)
            if ntype == 3 and name[:3] == b"GNU":   # NT_GNU_BUILD_ID
                return data[desc_at:desc_at + descsz].hex()
            p = desc_at + ((descsz + 3) & ~3)
    return None


def elf_is_executable(path):
    """True for a fixed-address (ET_EXEC) program: its addresses aren't relative to where it was loaded"""
    try:
        with open(path, "rb") as f:
            head = f.read(18)
    except OSError:
        return False
    if head[:4] != b"\x7fELF":
        return False
    e_type, = struct.unpack_from("<H" if head[5] == 1 else ">H", head, 16)
    return e_type == 2


def find_linux_symbols(module_path, build_id, dirs):
    name = os.path.basename(module_path)
    candidates = []
    if build_id and build_id != "-":
        for d in dirs:
            candidates.append(os.path.join(d, ".build-id", build_id[:2], build_id[2:] + ".debug"))
    for d in dirs:
        candidates.append(os.path.join(d, name))
        candidates.append(os.path.join(d, name + ".debug"))
    candidates.append(module_path)
    for c in candidates:
        if os.path.isfile(c) and (build_id in (None, "-") or elf_build_id(c) == build_id):
            return c
    return None


def find_macos_symbols(module_path, dirs):
    name = os.path.basename(module_path)
    for d in dirs:
        dwarf = os.path.join(d, name + ".dSYM", "Contents", "Resources", "DWARF", name)
        if os.path.isfile(dwarf):
            return dwarf
    return module_path if os.path.isfile(module_path) else None


def resolve_linux(symbols, address):
    if shutil.which("addr2line"):
        out = subprocess.run(["addr2line", "-f", "-C", "-i", "-e", symbols, hex(address)], capture_output=True,
                             text=True).stdout.split("\n")
        frames = []
        for i in range(0, len(out) - 1, 2):   # (function, file:line pairs; inlined ones first)
            if out[i] and out[i] != "??":
                where = out[i + 1] if i + 1 < len(out) else ""
                frames.append(out[i] + ("" if where.startswith("??") else " at " + where))
        return " <- ".join(frames) or None
    if shutil.which("llvm-symbolizer"):
        out = subprocess.run(["llvm-symbolizer", "--obj=" + symbols, "-C", hex(address)], capture_output=True,
                             text=True).stdout.strip().split("\n")
        return (out[0] + " at " + out[1]) if len(out) >= 2 and out[0] != "??" else None
    return None


def resolve_macos(symbols, load, address):
    if not shutil.which("atos"):
        return None
    out = subprocess.run(["atos", "-o", symbols, "-l", hex(load), hex(address)], capture_output=True,
                         text=True).stdout.strip()
    return out if out and not out.startswith("0x") else None


def resolve_windows(module, rva, dirs):
    if not shutil.which("llvm-symbolizer"):
        return None
    for d in dirs:
        obj = os.path.join(d, module)
        if os.path.isfile(obj):
            out = subprocess.run(["llvm-symbolizer", "--obj=" + obj, "--relative-address", "-C", hex(rva)],
                                 capture_output=True, text=True).stdout.strip().split("\n")
            if len(out) >= 2 and out[0] != "??":
                return out[0] + " at " + out[1]
    return None


def main():
    parser = argparse.ArgumentParser(description="Turns a crash report's addresses into functions and lines")
    parser.add_argument("report")
    parser.add_argument("--symbols", action="append", default=[], help="a folder of kept symbols (repeatable)")
    args = parser.parse_args()
    with open(args.report, encoding="utf-8", errors="replace") as f:
        lines = f.read().split("\n")

    modules = []   # (start, end, id, path)
    in_modules = False
    for line in lines:
        if line.startswith("Modules:"):
            in_modules = True
            continue
        m = MODULE.match(line) if in_modules else None
        if m:
            modules.append((int(m.group(1), 16), int(m.group(2), 16), m.group(3), m.group(4)))
        elif in_modules and not line.startswith("  "):
            in_modules = False
    macos = any(p.endswith(".dylib") or "/Contents/MacOS/" in p for _, _, _, p in modules) or sys.platform == "darwin"

    resolved = 0
    out = []
    for line in lines:
        note = None
        if FRAME.match(line):
            win = re.search(r"#\d+ ([^\s!]+\.(?:exe|dll))\+0x([0-9a-fA-F]+)\s*$", line)
            bracket = re.search(r"\[0x([0-9a-f]+)\]", line)
            plain = re.findall(r"\b0x([0-9a-f]{9,16})\b", line)
            if win:
                note = resolve_windows(win.group(1), int(win.group(2), 16), args.symbols)
            elif bracket or plain:
                address = int(bracket.group(1) if bracket else plain[0], 16)
                for start, end, build_id, path in modules:
                    if start <= address < end:
                        if macos:
                            symbols = find_macos_symbols(path, args.symbols)
                            note = symbols and resolve_macos(symbols, start, address)
                        else:
                            symbols = find_linux_symbols(path, build_id, args.symbols)
                            offset = address if elf_is_executable(path) else address - start
                            note = symbols and resolve_linux(symbols, offset)
                        break
        if note:
            resolved += 1
            line += "\n      -> " + note
        out.append(line)
    print("\n".join(out))
    if resolved == 0:
        print("(nothing resolved: no Modules section, or no symbols for this build found)", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
