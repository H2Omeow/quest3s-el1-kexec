#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Fetch everything the headset needs from a GitHub release and verify it.
#
#   scripts/fetch-bringup.sh [tag] [outdir]
#
# Defaults: tag bringup-1, outdir bringup.  The five payload files are named
# exactly as scripts/bringup.sh expects them under /data/local/tmp/q3s/, so the
# directory this produces can be pushed as-is:
#
#   adb shell mkdir -p /data/local/tmp/q3s
#   adb push bringup/. /data/local/tmp/q3s/
#
# SHA256SUMS covers the five payload files; a missing or corrupted one aborts
# before anything reaches the device, which is the whole point of this script.
set -euo pipefail

tag=${1:-bringup-1}
out=${2:-bringup}
repo=${Q3S_REPO:-H2Omeow/quest3s-el1-kexec}
base="https://github.com/${repo}/releases/download/${tag}"

payload="Image.el1 initramfs.cpio.gz quest-el1 quest_el1_kexec.ko bringup.sh"
sidecar="SHA256SUMS manifest.json"

mkdir -p -- "$out"

fetch() {
    local name=$1
    if [[ -s $out/$name ]]; then
        echo "have   $name"
        return
    fi
    echo "fetch  $name"
    curl -fL --retry 3 --retry-delay 2 -o "$out/$name.part" "$base/$name"
    mv -- "$out/$name.part" "$out/$name"
}

for name in $sidecar $payload; do
    fetch "$name"
done

if command -v sha256sum >/dev/null 2>&1; then
    (cd -- "$out" && sha256sum -c SHA256SUMS)
else
    (cd -- "$out" && shasum -a 256 -c SHA256SUMS)
fi

missing=0
for name in $payload; do
    [[ -s $out/$name ]] || { echo "MISSING $name" >&2; missing=1; }
done
[[ $missing == 0 ]] || { echo "incomplete download in $out" >&2; exit 1; }

cat <<EOF
ok: $out holds all five files for the headset

    adb shell mkdir -p /data/local/tmp/q3s
    adb push $out/. /data/local/tmp/q3s/
    adb shell su -c 'sh /data/local/tmp/q3s/bringup.sh stage1'
EOF
