#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Build a static Android-usable ARM64 CLI with a caller-supplied libfdt source.
set -euo pipefail
if [[ $# != 1 ]]; then
    echo "Usage: $0 /absolute/path/to/dtc-source" >&2
    exit 2
fi
task_root=$(cd -- "$(dirname -- "$0")/.." && pwd)
task_dtc=$(realpath -- "$1")
task_libfdt="$task_dtc/libfdt"
task_output="$task_root/build/bin/quest-el1"
[[ -f $task_libfdt/fdt_check.c && -f $task_libfdt/libfdt.h ]] || {
    echo "Needs a complete libfdt source including fdt_check.c (tested: dtc 1.7.2)." >&2
    exit 1
}
mkdir -p -- "$(dirname -- "$task_output")"
"${QUEST_EL1_CC:-aarch64-linux-gnu-gcc}" -O2 -Wall -Wextra -Werror -static \
    -I"$task_root/include" -I"$task_libfdt" \
    "$task_root/user/quest-el1.c" \
    "$task_libfdt/fdt.c" "$task_libfdt/fdt_ro.c" \
    "$task_libfdt/fdt_check.c" "$task_libfdt/fdt_addresses.c" \
    "$task_libfdt/fdt_rw.c" "$task_libfdt/fdt_wip.c" \
    "$task_libfdt/fdt_sw.c" "$task_libfdt/fdt_strerror.c" \
    "$task_libfdt/fdt_empty_tree.c" \
    -o "$task_output"
file -- "$task_output"
sha256sum -- "$task_output"
