/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef QUEST_EL1_CPU_RESET_H
#define QUEST_EL1_CPU_RESET_H

/* No return and no CFI-indirect call to a dynamically copied physical address. */
void __noreturn quest_enter_stub(unsigned long stub, unsigned long head,
			       unsigned long entry, unsigned long dtb);

#endif
