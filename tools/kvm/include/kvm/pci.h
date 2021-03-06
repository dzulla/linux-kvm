#ifndef KVM__PCI_H
#define KVM__PCI_H

#include <linux/types.h>

#include <linux/pci_regs.h>

/*
 * PCI Configuration Mechanism #1 I/O ports. See Section 3.7.4.1.
 * ("Configuration Mechanism #1") of the PCI Local Bus Specification 2.1 for
 * details.
 */
#define PCI_CONFIG_ADDRESS	0xcf8
#define PCI_CONFIG_DATA		0xcfc
#define PCI_CONFIG_BUS_FORWARD	0xcfa

struct pci_config_address {
	unsigned	zeros		: 2;		/* 1  .. 0  */
	unsigned	register_number	: 6;		/* 7  .. 2  */
	unsigned	function_number	: 3;		/* 10 .. 8  */
	unsigned	device_number	: 5;		/* 15 .. 11 */
	unsigned	bus_number	: 8;		/* 23 .. 16 */
	unsigned	reserved	: 7;		/* 30 .. 24 */
	unsigned	enable_bit	: 1;		/* 31       */
};

struct pci_device_header {
	u16		vendor_id;
	u16		device_id;
	u16		command;
	u16		status;
	u16		revision_id		:  8;
	u32		class			: 24;
	u8		cacheline_size;
	u8		latency_timer;
	u8		header_type;
	u8		bist;
	u32		bar[6];
	u32		card_bus;
	u16		subsys_vendor_id;
	u16		subsys_id;
	u32		exp_rom_bar;
	u32		capabilities		:  8;
	u32		reserved1		: 24;
	u32		reserved2;
	u8		irq_line;
	u8		irq_pin;
	u8		min_gnt;
	u8		max_lat;
};

void pci__init(void);
void pci__register(struct pci_device_header *dev, u8 dev_num);

#endif /* KVM__PCI_H */
