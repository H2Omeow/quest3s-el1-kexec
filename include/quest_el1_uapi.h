/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef QUEST_EL1_UAPI_H
#define QUEST_EL1_UAPI_H

#include <linux/types.h>
#include <linux/ioctl.h>

#define QUEST_EL1_MAGIC 0x51454c31U
#define QUEST_EL1_ABI_VERSION 1U
#define QUEST_EL1_DEVICE "/dev/quest_el1"
#define QUEST_EL1_MAX_SEGMENTS 16U
#define QUEST_EL1_ARCH_AARCH64 (183ULL << 16)

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

#define QUEST_EL1_IOC_STATUS _IOWR('Q', 0, struct quest_el1_status)
#define QUEST_EL1_IOC_LOAD   _IOW('Q', 1, struct quest_el1_load)
#define QUEST_EL1_IOC_UNLOAD _IOW('Q', 2, struct quest_el1_header)
#define QUEST_EL1_IOC_EXEC   _IOW('Q', 3, struct quest_el1_header)

#endif
