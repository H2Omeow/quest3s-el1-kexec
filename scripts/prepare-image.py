#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Export the ARM64 Linux Image, rejecting unexplained allocated ELF sections.

Requires pyelftools and llvm-objcopy. Never changes the source kernel build.
ARM64 5.10 unwinds frame pointers; top-level DWARF .eh_frame after _end is not
part of its runtime image. All remaining allocated file bytes are compared
against the exported binary before publishing it.
"""
import argparse
import hashlib
import os
from pathlib import Path
import struct
import subprocess
import tempfile

from elftools.elf.elffile import ELFFile


def export_image(source, destination, objcopy):
    if source.resolve() == destination.resolve():
        raise ValueError("input and output must differ")
    with source.open("rb") as stream:
        elf = ELFFile(stream)
        if elf.elfclass != 64 or not elf.little_endian or elf["e_machine"] != "EM_AARCH64":
            raise ValueError("expected a little-endian AArch64 vmlinux")
        symbols = elf.get_section_by_name(".symtab")
        if symbols is None:
            raise ValueError("vmlinux needs its symbol table")
        bounds = {}
        for name in ("_text", "_end"):
            matches = symbols.get_symbol_by_name(name)
            if not matches or len(matches) != 1:
                raise ValueError(f"missing or ambiguous {name}")
            bounds[name] = matches[0]["st_value"]
        start, end = bounds["_text"], bounds["_end"]
        if not start or end <= start:
            raise ValueError("invalid _text.._end extent")
        removed = []
        retained = []
        excluded = {".note", ".note.gnu.build-id", ".comment"}
        for section in elf.iter_sections():
            addr, size, flags = (section[key] for key in ("sh_addr", "sh_size", "sh_flags"))
            if not flags & 2 or not size:
                continue
            if section.name in (".eh_frame", ".eh_frame_hdr"):
                if addr < end or flags & (1 | 4):
                    raise ValueError("unwind section is inside runtime image or writable/executable")
                removed.append((section.name, size))
                excluded.add(section.name)
                continue
            if addr < start or addr + size > end:
                raise ValueError(f"unexplained allocated section outside Image: {section.name}")
            if section.name not in excluded and section["sh_type"] != "SHT_NOBITS":
                retained.append((section.name, addr - start, section.data()))
        head = elf.get_section_by_name(".head.text")
        if head is None or head["sh_addr"] != start:
            raise ValueError("Image must start with .head.text at _text")

        destination.parent.mkdir(parents=True, exist_ok=True)
        fd, name = tempfile.mkstemp(prefix=".Image-el1-", dir=destination.parent)
        os.close(fd)
        temporary = Path(name)
        try:
            command = [objcopy, "-O", "binary", "-S"]
            for section_name in sorted(excluded):
                command += ["-R", section_name]
            subprocess.run(command + [str(source), str(temporary)], check=True)
            data = temporary.read_bytes()
            if len(data) < 64 or data[56:60] != b"ARM\x64":
                raise ValueError("export is missing the ARM64 Image header")
            text_offset, image_size, flags = struct.unpack_from("<QQQ", data, 8)
            if image_size != end - start or len(data) > image_size:
                raise ValueError("exported extent disagrees with Image header/_end")
            if flags & ~15 or flags & 1 or (flags >> 1) & 3 != 1:
                raise ValueError("expected an LE/4K Image with known flags")
            for section_name, offset, expected in retained:
                if data[offset:offset + len(expected)] != expected:
                    raise ValueError(f"export changed section bytes: {section_name}")
            os.chmod(temporary, 0o644)
            os.replace(temporary, destination)
        finally:
            temporary.unlink(missing_ok=True)
    print(f"{destination}: {len(data)} bytes; runtime extent {image_size} bytes")
    print(f"text_offset={text_offset:#x} flags={flags:#x}; {len(retained)} allocated sections compared")
    for section_name, size in removed:
        print(f"Excluded top-level unwind metadata after _end: {section_name} ({size} bytes)")
    print(f"SHA256 {hashlib.sha256(data).hexdigest()}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("vmlinux", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--objcopy", default="llvm-objcopy")
    args = parser.parse_args()
    try:
        export_image(args.vmlinux, args.output, args.objcopy)
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"Image export failed: {error}\n")


if __name__ == "__main__":
    main()
