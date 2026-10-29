#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Recover the running kernel's Module.symvers from its raw Image + kallsyms.

The Quest 3S stock kernel is built with CONFIG_MODVERSIONS but does not ship
Module.symvers.  Every exported symbol is still described inside the Image:
    __ksymtab[_gpl]  : struct kernel_symbol { int value, name, namespace; }
                       (PREL32 offsets, CONFIG_HAVE_ARCH_PREL32_RELOCATIONS)
    __kcrctab[_gpl]  : u32 crc per entry, same order.
kallsyms (read as root with kptr_restrict=0) gives the runtime addresses of the
table bounds and of _text; with a raw (uncompressed) Image, file offset ==
vaddr - _text, independent of KASLR.

Usage:
    stock-symvers.py --boot boot_a.img|--image Image --kallsyms kallsyms.txt -o Module.symvers
"""
import argparse
import struct
import sys

TABLES = (
    ("EXPORT_SYMBOL", "__start___ksymtab", "__stop___ksymtab", "__start___kcrctab"),
    ("EXPORT_SYMBOL_GPL", "__start___ksymtab_gpl", "__stop___ksymtab_gpl", "__start___kcrctab_gpl"),
)


def kernel_from_boot(data):
    if data[:8] != b"ANDROID!":
        raise SystemExit("not an Android boot image")
    kernel_size = struct.unpack_from("<I", data, 8)[0]
    version = struct.unpack_from("<I", data, 40)[0]
    if version < 3:
        raise SystemExit("boot image header v%d unsupported (need v3/v4)" % version)
    return data[4096:4096 + kernel_size]


def load_kallsyms(path):
    syms = {}
    with open(path, encoding="ascii", errors="replace") as f:
        for line in f:
            p = line.split()
            # Only vmlinux symbols (module symbols carry a 4th "[mod]" column).
            if len(p) == 3:
                syms.setdefault(p[2], int(p[0], 16))
    return syms


def cstr(img, off):
    end = img.index(b"\0", off)
    return img[off:end].decode()


def extract(img, syms):
    if img[56:60] != b"ARMd":
        raise SystemExit("raw arm64 Image expected (compressed kernels unsupported)")
    text = syms.get("_text")
    if not text or syms.get("_text") == 0:
        raise SystemExit("kallsyms has no usable _text (read it as root with kptr_restrict=0)")
    out = []
    for kind, start, stop, crcs in TABLES:
        a, b, c = (syms[n] - text for n in (start, stop, crcs))
        if (b - a) % 12:
            raise SystemExit("%s size is not a multiple of 12 (no PREL32 ksymtab?)" % start)
        for i in range((b - a) // 12):
            e = a + 12 * i
            _, name_off, ns_off = struct.unpack_from("<iii", img, e)
            name = cstr(img, e + 4 + name_off)
            ns = cstr(img, e + 8 + ns_off) if ns_off else ""
            crc = struct.unpack_from("<I", img, c + 4 * i)[0]
            out.append((name, crc, kind, ns))
    return sorted(out)


def main():
    ap = argparse.ArgumentParser()
    src = ap.add_argument_group("kernel").add_mutually_exclusive_group(required=True)
    src.add_argument("--boot")
    src.add_argument("--image")
    ap.add_argument("--kallsyms", required=True)
    ap.add_argument("-o", "--output", required=True)
    args = ap.parse_args()
    data = open(args.boot or args.image, "rb").read()
    img = kernel_from_boot(data) if args.boot else data
    rows = extract(img, load_kallsyms(args.kallsyms))
    with open(args.output, "w", newline="\n") as f:
        for name, crc, kind, ns in rows:
            f.write("0x%08x\t%s\tvmlinux\t%s\t%s\n" % (crc, name, kind, ns))
    print("%d exports written to %s" % (len(rows), args.output), file=sys.stderr)


if __name__ == "__main__":
    main()
