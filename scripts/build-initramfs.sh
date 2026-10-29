#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Pack the first-boot initramfs for the handed-off kernel.
#
#   build-initramfs.sh <static-init-binary> <output.cpio.gz> [extra-dir]
#
# The image holds /init (the given static binary), the mount points it needs
# and, optionally, the contents of extra-dir overlaid on top.  No shell and no
# modules: the first attempt must not depend on anything the target kernel may
# fail to provide.
set -euo pipefail

if (( $# < 2 || $# > 3 )); then
    echo "Usage: $0 <static-init-binary> <output.cpio.gz> [extra-dir]" >&2
    exit 2
fi
init=$(realpath -- "$1")
out=$2
extra=${3:-}

[[ -f $init ]] || { echo "init binary not found: $init" >&2; exit 1; }
if command -v file >/dev/null 2>&1; then
    if ! file -b "$init" | grep -q 'statically linked'; then
        echo "init must be statically linked: $(file -b "$init")" >&2
        exit 1
    fi
fi

root=$(mktemp -d)
trap 'rm -rf -- "$root"' EXIT
mkdir -p "$root"/{proc,sys,dev,tmp,mnt}
install -m 0755 "$init" "$root/init"
if [[ -n $extra ]]; then
    cp -a -- "$extra"/. "$root"/
fi

# Reproducible: fixed order, root ownership, no timestamps in newc headers
# other than mtime, which we normalise.
find "$root" -exec touch -h -d @0 {} +
( cd "$root" && find . -mindepth 1 -printf '%P\n' | LC_ALL=C sort |
    cpio --quiet -o -H newc --owner=0:0 ) | gzip -9n > "$out"

printf 'initramfs %s (%s bytes)\n' "$out" "$(stat -c %s "$out")"
