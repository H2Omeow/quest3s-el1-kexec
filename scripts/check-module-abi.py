#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Check .ko files against a kernel's Module.symvers and vermagic.

For each module: every __versions entry must name a symbol exported by the
kernel (or by another module given with --provider) with the identical CRC,
and .modinfo vermagic must equal --vermagic when given.  The kernel's
check_modstruct_layout() also requires module_layout, so a module lacking it is
reported too.  Exit status 1 if any module fails.

    check-module-abi.py --symvers Module.symvers [--vermagic STR]
                        [--provider other.ko|DIR ...] [--quiet] mod.ko|DIR ...
"""
import argparse
import os
import struct
import sys


def sections(path):
    d = open(path, "rb").read()
    if d[:4] != b"\x7fELF" or d[4] != 2 or d[5] != 1:
        raise ValueError("not an ELF64 little-endian file")
    shoff = struct.unpack_from("<Q", d, 0x28)[0]
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", d, 0x3A)
    sh = [struct.unpack_from("<IIQQQQIIQQ", d, shoff + i * shentsize) for i in range(shnum)]
    so = sh[shstrndx][4]
    res = {}
    for s in sh:
        name = d[so + s[0]:d.index(b"\0", so + s[0])].decode()
        res[name] = d[s[4]:s[4] + s[5]] if s[1] != 8 else b""
    return res


def versions(secs):
    v = secs.get("__versions", b"")
    for i in range(0, len(v), 64):
        crc = struct.unpack_from("<Q", v, i)[0] & 0xFFFFFFFF
        yield v[i + 8:i + 64].split(b"\0")[0].decode(), crc


def modinfo(secs):
    info = {}
    for item in secs.get(".modinfo", b"").split(b"\0"):
        if b"=" in item:
            k, _, val = item.decode(errors="replace").partition("=")
            info.setdefault(k, val)
    return info


def load_symvers(path, table):
    with open(path) as f:
        for line in f:
            p = line.rstrip("\n").split("\t")
            if len(p) >= 2:
                table[p[1]] = int(p[0], 16)


def expand(paths):
    out = []
    for p in paths:
        if os.path.isdir(p):
            for root, _, files in os.walk(p):
                out.extend(os.path.join(root, f) for f in sorted(files) if f.endswith(".ko"))
        else:
            out.append(p)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--symvers", required=True, action="append")
    ap.add_argument("--vermagic")
    ap.add_argument("--provider", action="append", default=[])
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("modules", nargs="+")
    args = ap.parse_args()
    args.modules = expand(args.modules)
    args.provider = expand(args.provider)

    exported = {}
    for p in args.symvers:
        load_symvers(p, exported)
    # Symbols exported by companion modules: their own __versions CRCs are
    # not available, so a provider only proves presence, not CRC.
    provided = set()
    for p in args.provider:
        s = sections(p)
        for sec in ("__ksymtab_strings",):
            provided.update(x.decode() for x in s.get(sec, b"").split(b"\0") if x)

    failed = 0
    for path in args.modules:
        try:
            secs = sections(path)
        except (OSError, ValueError) as e:
            print("%s: %s" % (path, e))
            failed += 1
            continue
        info = modinfo(secs)
        problems = []
        if args.vermagic is not None and info.get("vermagic") != args.vermagic:
            problems.append("vermagic %r != %r" % (info.get("vermagic"), args.vermagic))
        names = set()
        ok = 0
        for name, crc in versions(secs):
            names.add(name)
            if name in exported:
                if exported[name] != crc:
                    problems.append("CRC %s module 0x%08x kernel 0x%08x" % (name, crc, exported[name]))
                else:
                    ok += 1
            elif name not in provided:
                problems.append("unresolved %s" % name)
        if "module_layout" not in names:
            problems.append("no module_layout version (modversions build expected)")
        if problems:
            failed += 1
            print("%s: FAIL (%d ok)" % (path, ok))
            for p in problems[:40]:
                print("    " + p)
            if len(problems) > 40:
                print("    ... %d more" % (len(problems) - 40))
        elif not args.quiet:
            print("%s: ok (%d symbols, vermagic %s)" % (path, ok, info.get("vermagic")))
    print("%d/%d modules passed" % (len(args.modules) - failed, len(args.modules)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
