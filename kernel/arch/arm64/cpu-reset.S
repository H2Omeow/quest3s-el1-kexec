/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/linkage.h>

/* Called through a direct BL in the executable module mapping. */
SYM_FUNC_START(quest_enter_stub)
	/* IP0 preserves call-type BTI semantics when entering the physical stub. */
	mov	x16, x0
	mov	x0, x1
	mov	x1, x2
	mov	x2, x3
	br	x16
SYM_FUNC_END(quest_enter_stub)
