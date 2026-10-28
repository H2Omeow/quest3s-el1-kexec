// SPDX-License-Identifier: GPL-2.0-only
/* Restore the PCI kexec shutdown step absent from a CONFIG_KEXEC_CORE=n host. */
#define pr_fmt(fmt) "quest_el1_kexec: " fmt

#include <linux/compiler.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/pci.h>

#include "kexec_compat.h"
#include "platform_pci.h"

#if IS_ENABLED(CONFIG_PCI)
static struct pci_dev *(*get_device_fn)(unsigned int, unsigned int,
				       struct pci_dev *);
static void (*clear_master_fn)(struct pci_dev *);

int quest_pci_compat_init(void)
{
	/* Keep stock symbol/CRC imports out of an unverified candidate object. */
	get_device_fn = (void *)quest_lookup_name("pci_get_device");
	clear_master_fn = (void *)quest_lookup_name("pci_clear_master");
	if (!get_device_fn || !clear_master_fn) {
		pr_err("Required stock PCI shutdown helper is unavailable\n");
		return -ENOENT;
	}
	return 0;
}

/* Runtime symbol addresses need not be canonical cross-DSO CFI entries. */
void __nocfi quest_pci_quiesce(void)
{
	struct pci_dev *dev = NULL;
	unsigned int cleared = 0;

	/*
	 * The stock pci_device_shutdown() normally does this after the driver's
	 * shutdown callback. CONFIG_KEXEC_CORE=n compiled its kexec_in_progress
	 * test to false; the module's private flag cannot change that code.
	 *
	 * Execute while interrupts and the other CPUs are still available:
	 * enumeration and config access may take locks. pci_get_device() holds
	 * the returned device's reference and drops the preceding one, including
	 * on the final iteration. Match the stock power-state condition so that
	 * config space of D3cold and unknown devices is not accessed here.
	 */
	while ((dev = get_device_fn(PCI_ANY_ID, PCI_ANY_ID, dev))) {
		if (READ_ONCE(dev->current_state) <= PCI_D3hot) {
			clear_master_fn(dev);
			cleared++;
		}
	}
	pr_info("PCI shutdown: cleared bus mastering on %u devices\n", cleared);
}
#else
int quest_pci_compat_init(void)
{
	return 0;
}

void quest_pci_quiesce(void)
{
}
#endif
