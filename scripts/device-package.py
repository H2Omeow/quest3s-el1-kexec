#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Assemble everything that has to reach the headset into one directory.

    device-package.py --out out/device \
        --module dist/quest_el1_kexec.ko \
        --tool out/quest-el1-aarch64 \
        --image Image.el1 \
        --initramfs initramfs.cpio.gz

The result is what `adb push out/device/. /data/local/tmp/q3s/` sends: the
module, the static planner, the target Image, the first-boot initramfs, the
bring-up script and a manifest that records exactly what was shipped, so the
device log can be matched against the build later.
"""
import argparse
import hashlib
import json
import os
import shutil
import sys

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def copy(src, dst, mode=None):
    if not os.path.isfile(src):
        raise SystemExit("missing input: %s" % src)
    shutil.copyfile(src, dst)
    if mode is not None:
        os.chmod(dst, mode)
    return {"source": os.path.abspath(src), "name": os.path.basename(dst),
            "size": os.path.getsize(dst), "sha256": sha256(dst)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", required=True, help="output directory")
    parser.add_argument("--module", required=True, help="quest_el1_kexec.ko")
    parser.add_argument("--tool", required=True, help="static quest-el1")
    parser.add_argument("--image", required=True, help="Image.el1 for the target")
    parser.add_argument("--initramfs", required=True, help="first-boot initramfs")
    parser.add_argument("--kernel-release", default="",
                        help="the target's include/config/kernel.release")
    args = parser.parse_args()

    os.makedirs(args.out, exist_ok=True)
    files = {}
    files["quest_el1_kexec.ko"] = copy(args.module,
                                       os.path.join(args.out, "quest_el1_kexec.ko"))
    files["quest-el1"] = copy(args.tool, os.path.join(args.out, "quest-el1"), 0o755)
    files["Image.el1"] = copy(args.image, os.path.join(args.out, "Image.el1"))
    files["initramfs.cpio.gz"] = copy(args.initramfs,
                                      os.path.join(args.out, "initramfs.cpio.gz"))
    files["bringup.sh"] = copy(os.path.join(HERE, "scripts", "bringup.sh"),
                               os.path.join(args.out, "bringup.sh"), 0o755)

    release = ""
    if args.kernel_release and os.path.isfile(args.kernel_release):
        with open(args.kernel_release, encoding="utf-8") as handle:
            release = handle.read().strip()

    manifest = {"target_kernel_release": release, "files": files}
    with open(os.path.join(args.out, "manifest.json"), "w", encoding="utf-8") as handle:
        json.dump(manifest, handle, indent=2, sort_keys=True)
        handle.write("\n")

    with open(os.path.join(args.out, "SHA256SUMS"), "w", encoding="utf-8") as handle:
        for name in sorted(files):
            handle.write("%s  %s\n" % (files[name]["sha256"], name))

    total = sum(entry["size"] for entry in files.values())
    print("package %s (%d files, %d bytes)" % (args.out, len(files), total))
    for name in sorted(files):
        print("  %-20s %9d  %s" % (name, files[name]["size"],
                                   files[name]["sha256"][:16]))
    if not release:
        print("warning: the target kernel release was not recorded", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
