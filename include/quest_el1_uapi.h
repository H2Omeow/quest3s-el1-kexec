/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef QUEST_EL1_UAPI_H
#define QUEST_EL1_UAPI_H

#include <linux/types.h>
#include <linux/ioctl.h>

#define QUEST_EL1_MAGIC 0x51454c31U
/*
 * ABI 2 adds the read()-able preflight report and the QUEST_EL1_IOC_REHEARSE
 * reversible self-tests. LOAD/EXEC/STATUS/UNLOAD are unchanged from ABI 1, so
 * an ABI-1 caller still interoperates for those; the module reports 2.
 */
#define QUEST_EL1_ABI_VERSION 2U
#define QUEST_EL1_DEVICE "/dev/quest_el1"
#define QUEST_EL1_MAX_SEGMENTS 16U
#define QUEST_EL1_ARCH_AARCH64 (183ULL << 16)

/* Reversible rehearsal selectors (quest_el1_rehearse.flags), OR together. */
#define QUEST_EL1_REHEARSE_WATCHDOG  (1U << 0) /* stop+restart watchdogs */
#define QUEST_EL1_REHEARSE_RPROC     (1U << 1) /* stop+restart remoteprocs */
#define QUEST_EL1_REHEARSE_CPUS      (1U << 2) /* offline+online secondaries */
#define QUEST_EL1_REHEARSE_MMU       (1U << 3) /* TTBR0 swap + MMU-off probe */
#define QUEST_EL1_REHEARSE_COPY      (1U << 4) /* full relocation to scratch */
#define QUEST_EL1_REHEARSE_ALL       0x1fU

struct quest_el1_header {
	__u32 magic;
	__u32 abi_version;
};

/* All pointers are user virtual addresses encoded as unsigned 64-bit values. */
struct quest_el1_segment {
	__u64 buf;
	__u64 bufsz;
	__u64 mem;
	__u64 memsz;
};

struct quest_el1_load {
	struct quest_el1_header hdr;
	__u64 entry;
	__u64 dtb;
	__u64 nr_segments;
	__u64 flags;
	__u64 segments;
};

struct quest_el1_status {
	struct quest_el1_header hdr;
	__u32 loaded;
	__u32 current_el;
	__u32 stock_va_bits;
	__u32 page_shift;
	__u64 entry;
	__u64 dtb;
	__u64 nr_segments;
};

/*
 * Reversible self-test. flags selects which stages to run; the module runs
 * each requested stage, restores all state, and returns. done reports the
 * stages that actually ran, result is the first stage's error (0 = all ok),
 * and the detail fields carry per-stage observations. No image is required
 * except for COPY, which needs a loaded image to clone its segment list.
 */
struct quest_el1_rehearse {
	struct quest_el1_header hdr;
	__u32 flags;
	__u32 done;
	__s32 result;
	__u32 watchdogs_stopped;
	__u32 rprocs_stopped;
	__u32 cpus_offlined;
	__u64 mmu_sctlr;      /* SCTLR_EL1 observed with the MMU disabled */
	__u64 copy_pages;     /* pages relocated through the real blob */
	__u64 copy_ns;        /* wall-clock time for the COPY stage */
};

#define QUEST_EL1_IOC_STATUS _IOWR('Q', 0, struct quest_el1_status)
#define QUEST_EL1_IOC_LOAD   _IOW('Q', 1, struct quest_el1_load)
#define QUEST_EL1_IOC_UNLOAD _IOW('Q', 2, struct quest_el1_header)
#define QUEST_EL1_IOC_EXEC   _IOW('Q', 3, struct quest_el1_header)
#define QUEST_EL1_IOC_REHEARSE _IOWR('Q', 4, struct quest_el1_rehearse)

#endif
