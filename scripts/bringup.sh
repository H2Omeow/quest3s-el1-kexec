#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# Quest 3S handoff bring-up, run ON the device through adb + su.
#
#   adb push <package>/ /data/local/tmp/q3s/
#   adb shell su -c 'sh /data/local/tmp/q3s/bringup.sh stage1'
#
# Every stage is deliberately small and leaves a log behind, because the
# bootloader is locked and a bad handoff can only be undone with the power
# button.  Nothing here writes a partition.
#
#   stage1  load the module, collect the report and the runtime DTB
#   stage2  plan the handoff against the live iomem and DT, then load it
#   stage3  rehearse the copy relocation (reversible), optional
#   stage4  hand off (irreversible; the headset restarts)
#
# Environment overrides (addresses default to the layout measured on the
# reference unit; stage2 re-validates all of them against live data):
#   KERNEL_BASE  INITRD_ADDR  DTB_ADDR  IMAGE  INITRD  DTB

set -u

DIR=${Q3S_DIR:-/data/local/tmp/q3s}
LOG="$DIR/bringup.log"
MODULE="$DIR/quest_el1_kexec.ko"
TOOL="$DIR/quest-el1"
IMAGE=${IMAGE:-$DIR/Image.el1}
INITRD=${INITRD:-$DIR/initramfs.cpio.gz}
DTB=${DTB:-$DIR/runtime.dtb}
REPORT="$DIR/report.txt"
KERNEL_BASE=${KERNEL_BASE:-0xa8000000}
INITRD_ADDR=${INITRD_ADDR:-0xaca00000}
DTB_ADDR=${DTB_ADDR:-0xacc00000}
DEV=${DEV:-/dev/quest_el1}

say() {
    printf '%s\n' "$*" | tee -a "$LOG"
}

die() {
    say "FAILED: $*"
    exit 1
}

run() {
    say "+ $*"
    "$@" >>"$LOG" 2>&1 || die "$*"
}

# One option list shared by plan, load and rehearse: they must agree exactly.
plan_args() {
    printf '%s ' \
        --image "$IMAGE" --dtb "$DTB" --initrd "$INITRD" \
        --report "$REPORT" --iomem /proc/iomem \
        --kernel-base "$KERNEL_BASE" \
        --initrd-addr "$INITRD_ADDR" \
        --dtb-addr "$DTB_ADDR" \
        --disable /soc/qseecom@c1700000
}

stage1() {
    say "== stage1: module + report + runtime DTB"
    [ -f "$MODULE" ] || die "missing $MODULE"
    [ -f "$TOOL" ] || die "missing $TOOL"
    chmod 0755 "$TOOL" 2>/dev/null
    if [ -e "$DEV" ]; then
        say "module already loaded"
    else
        run insmod "$MODULE"
        run sh -c "cat $DEV > $REPORT"
    fi
    [ -s "$REPORT" ] || die "empty report"
    if ! tail -n 1 "$REPORT" | grep -qx 'end'; then
        say "WARNING: report is truncated (no 'end' line); re-read $DEV"
    fi
    grep -E '^(current_el|arch_check|tcr_el1|cpus|watchdog|gh-watchdog|driver|rproc|rmem|secure_summary|dmabuf_summary)' "$REPORT" |
        tee -a "$LOG" >/dev/null
    run "$TOOL" dump-dt "$DTB"
    run "$TOOL" report
    say "== stage1 done: $DTB and $REPORT ready"
}

stage2() {
    say "== stage2: plan against live state"
    [ -f "$IMAGE" ] || die "missing $IMAGE"
    [ -f "$DTB" ] || die "missing $DTB"
    [ -f "$REPORT" ] || die "run stage1 first"
    # shellcheck disable=SC2046
    run "$TOOL" plan $(plan_args)
    # shellcheck disable=SC2046
    run "$TOOL" load $(plan_args)
    run "$TOOL" status
    say "== stage2 done: image staged; the running kernel is untouched"
}

stage3() {
    say "== stage3: rehearse (reversible)"
    run "$TOOL" rehearse --steps copy
    say "== stage3 done: relocation verified without switching"
}

stage4() {
    say "== stage4: handoff"
    say "the headset will restart; power-cycle is the undo"
    run "$TOOL" exec --yes
    die "exec returned instead of switching"
}

case ${1:-} in
stage1 | stage2 | stage3 | stage4) "$1" ;;
*)
    echo "usage: $0 {stage1|stage2|stage3|stage4}" >&2
    exit 2
    ;;
esac
