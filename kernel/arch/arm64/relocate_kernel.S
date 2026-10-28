/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Derived from Linux ARM64 kexec relocation.
 * Copyright (C) Linaro.
 * Copyright (C) Huawei Futurewei Technologies.
 */
#include "../../kexec.h"
#include <linux/linkage.h>
#include <asm/assembler.h>
#include <asm/page.h>
#include <asm/sysreg.h>

/*
 * Copied as one contiguous blob to the protected control page. No external
 * calls, literal addresses, stack, or module VA references may occur here.
 * Entry is the identity-mapped physical address, with x0=tagged first list
 * entry value, x1=Image, x2=DTB. After disabling the MMU, all remaining
 * accesses are physical. x0 is the value of image->head, never its address.
 */
.pushsection .text.quest_el1_transition, "ax"
.balign 16
.globl quest_el1_transition
quest_el1_transition:
	hint	#34				/* BTI c, harmless on older CPUs */
	mov	x18, x2
	mov	x17, x1
	mov	x16, x0

	mov_q	x12, INIT_SCTLR_EL1_MMU_OFF
	dsb	sy
	pre_disable_mmu_workaround
	msr	sctlr_el1, x12
	isb

	raw_dcache_line_size x15, x0
	mov	x14, xzr
	mov	x13, xzr
	tbnz	x16, IND_DONE_BIT, .Ldone

.Lloop:
	and	x12, x16, PAGE_MASK
	tbz	x16, IND_SOURCE_BIT, .Lindirection

	/* Eliminate stale dirty destination cache lines before uncached writes. */
	mov	x0, x13
	add	x20, x0, #PAGE_SIZE
	sub	x1, x15, #1
	bic	x0, x0, x1
1:
	dc	ivac, x0
	add	x0, x0, x15
	cmp	x0, x20
	b.lo	1b
	dsb	sy
	mov	x20, x13
	mov	x21, x12
	copy_page x20, x21, x0, x1, x2, x3, x4, x5, x6, x7
	add	x13, x13, #PAGE_SIZE
	b	.Lnext

.Lindirection:
	tbz	x16, IND_INDIRECTION_BIT, .Ldestination
	mov	x14, x12
	b	.Lnext
.Ldestination:
	tbz	x16, IND_DESTINATION_BIT, .Lnext
	mov	x13, x12
.Lnext:
	ldr	x16, [x14], #8
	tbz	x16, IND_DONE_BIT, .Lloop

.Ldone:
	dsb	sy
	ic	iallu
	dsb	sy
	isb
	mov	x0, x18
	mov	x1, xzr
	mov	x2, xzr
	mov	x3, xzr
	br	x17

.globl quest_el1_transition_end
quest_el1_transition_end:
.if (quest_el1_transition_end - quest_el1_transition) > PAGE_SIZE
.error "EL1 transition stub exceeds the protected control page"
.endif
.popsection
