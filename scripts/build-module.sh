#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Build with the prepared output tree of the *running stock* kernel.
set -euo pipefail
mode=module
if [[ ${1:-} == --check-only ]]; then mode=check; shift; fi
if [[ $# != 1 ]]; then
    echo "Usage: $0 [--check-only] /absolute/path/to/prepared-stock-KDIR" >&2
    exit 2
fi
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
kdir=$(realpath -- "$1")
[[ -f $kdir/.config && -f $kdir/include/generated/autoconf.h ]] || {
    echo "KDIR needs stock .config and modules_prepare output." >&2; exit 1;
}
for expected in CONFIG_ARM64=y CONFIG_MODULES=y CONFIG_ARM64_4K_PAGES=y CONFIG_ARM64_VA_BITS=39 CONFIG_PGTABLE_LEVELS=3; do
    rg -qx -- "$expected" "$kdir/.config" || {
        echo "Required stock configuration: $expected" >&2; exit 1;
    }
done
if rg -q '^CONFIG_KEXEC(_CORE|_FILE)?=y$' "$kdir/.config"; then
    echo "This module carries its own kexec core; KDIR must have KEXEC_CORE disabled." >&2
    exit 1
fi
if [[ $mode == module ]] && rg -qx 'CONFIG_MODVERSIONS=y' "$kdir/.config" && [[ ! -s $kdir/Module.symvers ]]; then
    echo "Missing stock Module.symvers. Use --check-only for a candidate object, never a loadable-device claim." >&2
    exit 1
fi
# Select the exact stock tools through PATH/LLVM/REAL_CC/CROSS_COMPILE.
# Defaults describe the local candidate, not an asserted device ABI.
jobs=${QUEST_EL1_JOBS:-6}
[[ $jobs =~ ^[1-9][0-9]*$ ]] || { echo 'QUEST_EL1_JOBS must be positive.' >&2; exit 2; }
check_args=()
if [[ $mode == check ]]; then
    # LTO's first .o is an archive of bitcode. Run the native link too, then
    # stop before .ko generation; missing stock symvers cannot validate imports.
    check_args=(KBUILD_MODPOST_WARN=1 KBUILD_MODPOST_NOFINAL=1)
    printf 'Candidate compile/native link only; no stock symbol/CRC validation or .ko generation.\n'
fi
exec make -C "$kdir" ARCH=arm64 LLVM="${LLVM:-1}" LLVM_IAS="${LLVM_IAS:-1}" \
    REAL_CC="${REAL_CC:-clang}" CROSS_COMPILE="${CROSS_COMPILE:-aarch64-linux-gnu-}" \
    M="$root/kernel" -j"$jobs" "${check_args[@]}" modules
