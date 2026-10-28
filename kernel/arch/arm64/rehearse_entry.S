/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Reversible rehearsal of the real EL1 transition blob.
 *
 * quest_rehearse_enter() enters the unmodified quest_el1_transition copy in a
 * private identity-mapped control page, exactly as machine_kexec() does, but
 * passes "entry" = quest_el1_return (copied into the same page right after
 * the blob) and "dtb" = a cookie. The blob disables the MMU, walks the list
 * and copies every page with the MMU off, then branches to entry with
 * x0 = cookie. The return stub turns the MMU back on with the SCTLR_EL1 value
 * saved here and branches back to the module mapping.
 *
 * Register contract with relocate_kernel.S: the blob clobbers x0-x7,
 * x12-x18, x20 and x21 only. x19 (return VA) and x22 (SCTLR_EL1) survive.
 * x18 is the shadow call stack pointer and is saved on the normal stack,
 * which is not touched while the MMU is off.
 */
#include <linux/linkage.h>
#include <asm/assembler.h>

/*
 * u64 quest_rehearse_enter(phys_addr_t code, unsigned long head,
 *			    phys_addr_t ret_stub, u64 cookie)
 * Caller: IRQs/FIQs/SError/debug masked, idmap of the code page installed.
 * Returns the cookie as delivered in x0 by the blob.
 */
SYM_FUNC_START(quest_rehearse_enter)
	stp	x29, x30, [sp, #-96]!
	mov	x29, sp
	stp	x18, x19, [sp, #16]
	stp	x20, x21, [sp, #32]
	stp	x22, x23, [sp, #48]
	stp	x24, x25, [sp, #64]
	stp	x26, x27, [sp, #80]
	mrs	x22, sctlr_el1
	adr	x19, .Lquest_rehearse_back
	/* Same entry convention as quest_enter_stub: IP0 keeps BTI-c semantics. */
	mov	x16, x0
	mov	x0, x1
	mov	x1, x2
	mov	x2, x3
	br	x16
.Lquest_rehearse_back:
	hint	#38				/* BTI jc: landing pad for br x19 */
	ldp	x26, x27, [sp, #80]
	ldp	x24, x25, [sp, #64]
	ldp	x22, x23, [sp, #48]
	ldp	x20, x21, [sp, #32]
	ldp	x18, x19, [sp, #16]
	ldp	x29, x30, [sp], #96
	ret
SYM_FUNC_END(quest_rehearse_enter)

/*
 * Copied to the control page directly after the transition blob. Runs at its
 * physical address with the MMU off; once SCTLR_EL1 is restored the same
 * address is translated by the private idmap (TTBR0) and the kernel mapping
 * (TTBR1) is unchanged, so br x19 returns into the module.
 */
.pushsection .text.quest_el1_return, "ax"
.balign 16
.globl quest_el1_return
quest_el1_return:
	hint	#34				/* BTI c: entered with br x17 */
	dsb	sy
	msr	sctlr_el1, x22
	isb
	ic	iallu
	dsb	nsh
	isb
	br	x19
.globl quest_el1_return_end
quest_el1_return_end:
.popsection
