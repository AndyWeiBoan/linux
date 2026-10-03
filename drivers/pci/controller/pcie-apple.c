// SPDX-License-Identifier: GPL-2.0
/*
 * PCIe host bridge driver for Apple system-on-chips.
 *
 * The HW is ECAM compliant, so once the controller is initialized,
 * the driver mostly deals MSI mapping and handling of per-port
 * interrupts (INTx, management and error signals).
 *
 * Initialization requires enabling power and clocks, along with a
 * number of register pokes.
 *
 * Copyright (C) 2021 Alyssa Rosenzweig <alyssa@rosenzweig.io>
 * Copyright (C) 2021 Google LLC
 * Copyright (C) 2021 Corellium LLC
 * Copyright (C) 2021 Mark Kettenis <kettenis@openbsd.org>
 *
 * Author: Alyssa Rosenzweig <alyssa@rosenzweig.io>
 * Author: Marc Zyngier <maz@kernel.org>
 */

#include <linux/bitfield.h>
#include <linux/gpio/consumer.h>
#include <linux/kernel.h>
#include <linux/iopoll.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/irqchip/irq-msi-lib.h>
#include <linux/irqdomain.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/msi.h>
#include <linux/of_irq.h>
#include <linux/pci-ecam.h>
#include <linux/soc/apple/tunable.h>

#include "pci-host-common.h"

static int link_up_timeout = 500;
module_param(link_up_timeout, int, 0644);
MODULE_PARM_DESC(link_up_timeout, "PCIe link training timeout in milliseconds");

/* T8103 (original M1) and related SoCs */
#define CORE_RC_PHYIF_CTL		0x00024
#define   CORE_RC_PHYIF_CTL_RUN		BIT(0)
#define CORE_RC_PHYIF_STAT		0x00028
#define   CORE_RC_PHYIF_STAT_REFCLK	BIT(4)
#define CORE_RC_CTL			0x00050
#define   CORE_RC_CTL_RUN		BIT(0)
#define CORE_RC_STAT			0x00058
#define   CORE_RC_STAT_READY		BIT(0)
#define CORE_FABRIC_STAT		0x04000
#define   CORE_FABRIC_STAT_MASK		0x001F001F

#define CORE_PHY_DEFAULT_BASE(port)	(0x84000 + 0x4000 * (port))

#define PHY_LANE_CFG			0x00000
#define   PHY_LANE_CFG_REFCLK0REQ	BIT(0)
#define   PHY_LANE_CFG_REFCLK1REQ	BIT(1)
#define   PHY_LANE_CFG_REFCLK0ACK	BIT(2)
#define   PHY_LANE_CFG_REFCLK1ACK	BIT(3)
#define   PHY_LANE_CFG_REFCLKEN		(BIT(9) | BIT(10))
#define   PHY_LANE_CFG_REFCLKCGEN	(BIT(30) | BIT(31))
#define PHY_LANE_CTL			0x00004
#define   PHY_LANE_CTL_CFGACC		BIT(15)

#define PORT_LTSSMCTL			0x00080
#define   PORT_LTSSMCTL_START		BIT(0)
#define PORT_INTSTAT			0x00100
#define   PORT_INT_TUNNEL_ERR		31
#define   PORT_INT_CPL_TIMEOUT		23
#define   PORT_INT_RID2SID_MAPERR	22
#define   PORT_INT_CPL_ABORT		21
#define   PORT_INT_MSI_BAD_DATA		19
#define   PORT_INT_MSI_ERR		18
#define   PORT_INT_REQADDR_GT32		17
#define   PORT_INT_AF_TIMEOUT		15
#define   PORT_INT_LINK_DOWN		14
#define   PORT_INT_LINK_UP		12
#define   PORT_INT_LINK_BWMGMT		11
#define   PORT_INT_AER_MASK		(15 << 4)
#define   PORT_INT_PORT_ERR		4
#define   PORT_INT_INTx(i)		i
#define   PORT_INT_INTx_MASK		15
#define PORT_INTMSK			0x00104
#define PORT_INTMSKSET			0x00108
#define PORT_INTMSKCLR			0x0010c
#define PORT_MSICFG			0x00124
#define   PORT_MSICFG_EN		BIT(0)
#define   PORT_MSICFG_L2MSINUM_SHIFT	4
#define PORT_MSIBASE			0x00128
#define   PORT_MSIBASE_1_SHIFT		16
#define PORT_MSIADDR			0x00168
#define PORT_LINKSTS			0x00208
#define   PORT_LINKSTS_UP		BIT(0)
#define   PORT_LINKSTS_BUSY		BIT(2)
#define PORT_LINKCMDSTS			0x00210
#define PORT_OUTS_NPREQS		0x00284
#define   PORT_OUTS_NPREQS_REQ		BIT(24)
#define   PORT_OUTS_NPREQS_CPL		BIT(16)
#define PORT_RXWR_FIFO			0x00288
#define   PORT_RXWR_FIFO_HDR		GENMASK(15, 10)
#define   PORT_RXWR_FIFO_DATA		GENMASK(9, 0)
#define PORT_RXRD_FIFO			0x0028C
#define   PORT_RXRD_FIFO_REQ		GENMASK(6, 0)
#define PORT_OUTS_CPLS			0x00290
#define   PORT_OUTS_CPLS_SHRD		GENMASK(14, 8)
#define   PORT_OUTS_CPLS_WAIT		GENMASK(6, 0)
#define PORT_APPCLK			0x00800
#define   PORT_APPCLK_EN		BIT(0)
#define   PORT_APPCLK_CGDIS		BIT(8)
#define PORT_STATUS			0x00804
#define   PORT_STATUS_READY		BIT(0)
#define PORT_REFCLK			0x00810
#define   PORT_REFCLK_EN		BIT(0)
#define   PORT_REFCLK_CGDIS		BIT(8)
#define PORT_PERST			0x00814
#define   PORT_PERST_OFF		BIT(0)
/*
 * Registers that only show up in a trace of macOS bringing a tunnelled link
 * up (traces/macos-apciec in the asahi-dp-altmode tree). The names say what
 * they are used for here, not what Apple calls them.
 */
#define PORT_TUNNEL_PRE_RESET		0x0008c	/* 0x110, first write of all */
#define PORT_TUNNEL_CLRSTS		0x00148	/* cleared with ~0 alongside INTSTAT */
#define PORT_TUNNEL_CFG130		0x00130	/* 0x208 */
#define PORT_TUNNEL_CFG13C		0x0013c	/* 0x10 */
#define PORT_TUNNEL_CFG140		0x00140	/* 0x10, then the port tunable sets bit 0 */
#define PORT_TUNNEL_CFG144		0x00144	/* 0x253770 */
#define PORT_TUNNEL_CFG21C		0x0021c	/* 0 */
#define PORT_TUNNEL_CFG808		0x00808	/* 0x100045 */
#define PORT_TUNNEL_CFG81C		0x0081c	/* 0 */
#define PORT_TUNNEL_ARM			0x04020	/* 3, once the clocks are back on */
#define PORT_RID2SID			0x00828
#define   PORT_RID2SID_VALID		BIT(31)
#define   PORT_RID2SID_SID_SHIFT	16
#define   PORT_RID2SID_BUS_SHIFT	8
#define   PORT_RID2SID_DEV_SHIFT	3
#define   PORT_RID2SID_FUNC_SHIFT	0
#define PORT_OUTS_PREQS_HDR		0x00980
#define   PORT_OUTS_PREQS_HDR_MASK	GENMASK(9, 0)
#define PORT_OUTS_PREQS_DATA		0x00984
#define   PORT_OUTS_PREQS_DATA_MASK	GENMASK(15, 0)
#define PORT_TUNCTRL			0x00988
#define   PORT_TUNCTRL_PERST_ON		BIT(0)
#define   PORT_TUNCTRL_PERST_ACK_REQ	BIT(1)
#define PORT_TUNSTAT			0x0098c
#define   PORT_TUNSTAT_PERST_ON		BIT(0)
#define   PORT_TUNSTAT_PERST_ACK_PEND	BIT(1)
#define PORT_PREFMEM_ENABLE		0x00994

/* T602x (M2-pro and co) */
#define PORT_T602X_MSIADDR	0x016c
#define PORT_T602X_MSIADDR_HI	0x0170
#define PORT_T602X_PERST	0x082c
#define PORT_T602X_RID2SID	0x3000
#define PORT_T602X_MSIMAP	0x3800

#define PORT_MSIMAP_ENABLE	BIT(31)
#define PORT_MSIMAP_TARGET	GENMASK(7, 0)

/*
 * The doorbell address is set to 0xfffff000, which by convention
 * matches what MacOS does, and it is possible to use any other
 * address (in the bottom 4GB, as the base register is only 32bit).
 * However, it has to be excluded from the IOVA range, and the DART
 * driver has to know about it.
 */
#define DOORBELL_ADDR		CONFIG_PCIE_APPLE_MSI_DOORBELL_ADDR

/*
 * Bringing a tunnelled root complex up pokes registers in a block that is
 * only powered once both of its power domains are on, and getting that wrong
 * is an SError rather than an error code. Keep it behind an explicit opt-in
 * until it has been shown to work, so a bad guess costs one boot with the
 * parameter set rather than an unbootable kernel.
 */
/*
 * Which steps of the tunnel bring-up to actually perform, one bit each:
 *   0 tunables   1 port down   2 port config   3 port tunable
 *   4 release    5 MSI         6 link training 7 bus rescan
 * Default 0: log what the registers say and change nothing, so that a plug
 * is safe and the steps can be switched on one at a time at runtime rather
 * than one reboot per guess.
 */
/*
 * Arm link training at probe, the way every other port here works. macOS does
 * not, but it is the one thing about the port's state that changed between a
 * plug that worked and a plug that kills the machine, so keep it switchable.
 */
static bool apple_pcie_tunnel_init_at_probe = true;
module_param_named(tunnel_init_at_probe, apple_pcie_tunnel_init_at_probe, bool, 0444);
MODULE_PARM_DESC(tunnel_init_at_probe,
		 "Run macOS's boot-time init on a tunnelled port at probe");

/*
 * Print what the sequence would do instead of doing it, in the same shape as
 * an m1n1 hypervisor trace, so our sequence can be diffed against the trace of
 * macOS doing the same thing before any of it is let near the hardware.
 */
static bool apple_pcie_tunnel_dry_run = true;
module_param_named(tunnel_dry_run, apple_pcie_tunnel_dry_run, bool, 0644);
MODULE_PARM_DESC(tunnel_dry_run, "Log the tunnel sequence instead of running it");

struct apple_pcie;
struct apple_pcie_port;
static void apple_pcie_tunnel_port_init(struct apple_pcie *pcie,
					struct apple_pcie_port *port,
					bool start_link);

static unsigned int apple_pcie_tunnel_steps;
module_param_named(tunnel_steps, apple_pcie_tunnel_steps, uint, 0644);
MODULE_PARM_DESC(tunnel_steps,
		 "Bitmask of tunnel bring-up steps to perform (0 = observe only)");

static bool apple_pcie_enable_tunnel;
module_param_named(enable_tunnel, apple_pcie_enable_tunnel, bool, 0444);
MODULE_PARM_DESC(enable_tunnel,
		 "Bring up the Thunderbolt PCIe root complexes (apciec)");

struct hw_info {
	u32 phy_lane_ctl;
	u32 port_msiaddr;
	u32 port_msiaddr_hi;
	u32 port_refclk;
	u32 port_perst;
	u32 port_rid2sid;
	u32 port_msimap;
	u32 max_rid2sid;
	/*
	 * Thunderbolt-tunnelled root complexes, which Apple calls "apciec".
	 * The link runs over the USB4 fabric, so there is no PERST# GPIO to
	 * drive and no PCIe PHY of our own to hand a refclk request to: the
	 * ATC PHY, driven by phy/apple/atc.c and shared with Thunderbolt,
	 * already owns both. They also sit behind two power domains rather
	 * than one, which the driver core refuses to attach on its own.
	 */
	bool tunnelled;
};

static const struct hw_info t8103_hw = {
	.phy_lane_ctl		= PHY_LANE_CTL,
	.port_msiaddr		= PORT_MSIADDR,
	.port_msiaddr_hi	= 0,
	.port_refclk		= PORT_REFCLK,
	.port_perst		= PORT_PERST,
	.port_rid2sid		= PORT_RID2SID,
	.port_msimap		= 0,
	.max_rid2sid		= 64,
};

static const struct hw_info t602x_hw = {
	.phy_lane_ctl		= 0,
	.port_msiaddr		= PORT_T602X_MSIADDR,
	.port_msiaddr_hi	= PORT_T602X_MSIADDR_HI,
	.port_refclk		= 0,
	.port_perst		= PORT_T602X_PERST,
	.port_rid2sid		= PORT_T602X_RID2SID,
	.port_msimap		= PORT_T602X_MSIMAP,
	/* 16 on t602x, guess for autodetect on future HW */
	.max_rid2sid		= 512,
};

static const struct {
	const char *name;
	const char *reg_name;
} apple_pcie_tunables[] = {
	{ "apple,tunable-debug",  "debug"  },
	{ "apple,tunable-fabric", "fabric" },
	/*
	 * Deliberately not "apple,tunable-rc". m1n1 hands it to us, but in a
	 * full trace of macOS - boot and a hotplug - the rc window is not
	 * touched once, while fabric and debug are written repeatedly. It is
	 * presumably programmed earlier, by iBoot. Writing it from here takes
	 * an asynchronous SError a moment later, so the window is not ours to
	 * touch at this point.
	 */
};

/* A parsed tunable together with the window it is applied to. */
struct apple_pcie_tunable {
	struct apple_tunable *values;
	void __iomem *regs;
};

struct apple_pcie {
	struct mutex		lock;
	struct device		*dev;
	void __iomem            *base;
	const struct hw_info	*hw;
	unsigned long		*bitmap;
	struct list_head	ports;
	struct completion	event;
	struct irq_fwspec	fwspec;
	u32			nvecs;
	struct apple_pcie_tunable tunables[ARRAY_SIZE(apple_pcie_tunables)];
};

/*
 * Apple hands these down through the device tree and they have to be applied
 * before anything tries to train a link. @name is the tunable property;
 * @reg_name is the register window it goes to, which the device tree names.
 */
static void apple_pcie_tunable_apply(const struct apple_pcie_tunable *t)
{
	if (t->values && t->regs)
		apple_tunable_apply(t->regs, t->values);
}

/*
 * @apply: whether to write it now. The port's own tunable must not be: macOS
 * only ever writes that register as part of bringing a tunnel up, with the
 * port held in reset and its clocks off, and writing it at probe takes an
 * asynchronous SError.
 */
static int apple_pcie_load_tunable(struct platform_device *pdev,
				   struct device_node *np,
				   const char *prop, const char *reg_name,
				   struct apple_pcie_tunable *out, bool apply)
{
	struct device *dev = &pdev->dev;
	struct apple_tunable *tunable;
	void __iomem *regs;
	struct resource *res;

	if (!of_property_present(np, prop))
		return 0;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, reg_name);
	if (!res)
		return dev_err_probe(dev, -ENOENT, "no '%s' window for %s\n",
				     reg_name, prop);

	tunable = devm_apple_tunable_parse(dev, np, prop, res);
	if (IS_ERR(tunable))
		return dev_err_probe(dev, PTR_ERR(tunable),
				     "cannot parse %s\n", prop);

	/*
	 * Map without claiming the region: "rc" and "port0" are already mapped
	 * and owned elsewhere in this driver, and requesting them a second time
	 * fails the whole probe with -EBUSY.
	 */
	regs = devm_ioremap(dev, res->start, resource_size(res));
	if (!regs)
		return -ENOMEM;

	out->values = tunable;
	out->regs = regs;
	if (apply)
		apple_pcie_tunable_apply(out);
	dev_info(dev, "%s %s (%zu entries) for %s\n",
		 apply ? "applied" : "loaded", prop, tunable->sz, reg_name);

	return 0;
}

struct apple_pcie_port {
	raw_spinlock_t		lock;
	struct apple_pcie	*pcie;
	struct device_node	*np;
	void __iomem		*base;
	void __iomem		*phy;
	struct irq_domain	*domain;
	struct list_head	entry;
	unsigned long		*sid_map;
	int			sid_map_sz;
	int			idx;
	struct apple_pcie_tunable tunable;
};

static void rmw_set(u32 set, void __iomem *addr)
{
	writel_relaxed(readl_relaxed(addr) | set, addr);
}

static void rmw_clear(u32 clr, void __iomem *addr)
{
	writel_relaxed(readl_relaxed(addr) & ~clr, addr);
}

static void apple_msi_compose_msg(struct irq_data *data, struct msi_msg *msg)
{
	msg->address_hi = upper_32_bits(DOORBELL_ADDR);
	msg->address_lo = lower_32_bits(DOORBELL_ADDR);
	msg->data = data->hwirq;
}

static struct irq_chip apple_msi_bottom_chip = {
	.name			= "MSI",
	.irq_mask		= irq_chip_mask_parent,
	.irq_unmask		= irq_chip_unmask_parent,
	.irq_eoi		= irq_chip_eoi_parent,
	.irq_set_affinity	= irq_chip_set_affinity_parent,
	.irq_set_type		= irq_chip_set_type_parent,
	.irq_compose_msi_msg	= apple_msi_compose_msg,
};

static int apple_msi_domain_alloc(struct irq_domain *domain, unsigned int virq,
				  unsigned int nr_irqs, void *args)
{
	struct apple_pcie *pcie = domain->host_data;
	struct irq_fwspec fwspec = pcie->fwspec;
	unsigned int i;
	int ret, hwirq;

	mutex_lock(&pcie->lock);

	hwirq = bitmap_find_free_region(pcie->bitmap, pcie->nvecs,
					order_base_2(nr_irqs));

	mutex_unlock(&pcie->lock);

	if (hwirq < 0)
		return -ENOSPC;

	fwspec.param[fwspec.param_count - 2] += hwirq;

	ret = irq_domain_alloc_irqs_parent(domain, virq, nr_irqs, &fwspec);
	if (ret)
		return ret;

	for (i = 0; i < nr_irqs; i++) {
		irq_domain_set_hwirq_and_chip(domain, virq + i, hwirq + i,
					      &apple_msi_bottom_chip, pcie);
	}

	return 0;
}

static void apple_msi_domain_free(struct irq_domain *domain, unsigned int virq,
				  unsigned int nr_irqs)
{
	struct irq_data *d = irq_domain_get_irq_data(domain, virq);
	struct apple_pcie *pcie = domain->host_data;

	mutex_lock(&pcie->lock);

	bitmap_release_region(pcie->bitmap, d->hwirq, order_base_2(nr_irqs));

	mutex_unlock(&pcie->lock);
}

static const struct irq_domain_ops apple_msi_domain_ops = {
	.alloc	= apple_msi_domain_alloc,
	.free	= apple_msi_domain_free,
};

static void apple_port_irq_mask(struct irq_data *data)
{
	struct apple_pcie_port *port = irq_data_get_irq_chip_data(data);

	guard(raw_spinlock_irqsave)(&port->lock);
	rmw_set(BIT(data->hwirq), port->base + PORT_INTMSK);
}

static void apple_port_irq_unmask(struct irq_data *data)
{
	struct apple_pcie_port *port = irq_data_get_irq_chip_data(data);

	guard(raw_spinlock_irqsave)(&port->lock);
	rmw_clear(BIT(data->hwirq), port->base + PORT_INTMSK);
}

static bool hwirq_is_intx(unsigned int hwirq)
{
	return BIT(hwirq) & PORT_INT_INTx_MASK;
}

static void apple_port_irq_ack(struct irq_data *data)
{
	struct apple_pcie_port *port = irq_data_get_irq_chip_data(data);

	if (!hwirq_is_intx(data->hwirq))
		writel_relaxed(BIT(data->hwirq), port->base + PORT_INTSTAT);
}

static int apple_port_irq_set_type(struct irq_data *data, unsigned int type)
{
	/*
	 * It doesn't seem that there is any way to configure the
	 * trigger, so assume INTx have to be level (as per the spec),
	 * and the rest is edge (which looks likely).
	 */
	if (hwirq_is_intx(data->hwirq) ^ !!(type & IRQ_TYPE_LEVEL_MASK))
		return -EINVAL;

	irqd_set_trigger_type(data, type);
	return 0;
}

static struct irq_chip apple_port_irqchip = {
	.name		= "PCIe",
	.irq_ack	= apple_port_irq_ack,
	.irq_mask	= apple_port_irq_mask,
	.irq_unmask	= apple_port_irq_unmask,
	.irq_set_type	= apple_port_irq_set_type,
};

static int apple_port_irq_domain_alloc(struct irq_domain *domain,
				       unsigned int virq, unsigned int nr_irqs,
				       void *args)
{
	struct apple_pcie_port *port = domain->host_data;
	struct irq_fwspec *fwspec = args;
	int i;

	for (i = 0; i < nr_irqs; i++) {
		irq_flow_handler_t flow = handle_edge_irq;
		unsigned int type = IRQ_TYPE_EDGE_RISING;

		if (hwirq_is_intx(fwspec->param[0] + i)) {
			flow = handle_level_irq;
			type = IRQ_TYPE_LEVEL_HIGH;
		}

		irq_domain_set_info(domain, virq + i, fwspec->param[0] + i,
				    &apple_port_irqchip, port, flow,
				    NULL, NULL);

		irq_set_irq_type(virq + i, type);
	}

	return 0;
}

static void apple_port_irq_domain_free(struct irq_domain *domain,
				       unsigned int virq, unsigned int nr_irqs)
{
	int i;

	for (i = 0; i < nr_irqs; i++) {
		struct irq_data *d = irq_domain_get_irq_data(domain, virq + i);

		irq_set_handler(virq + i, NULL);
		irq_domain_reset_irq_data(d);
	}
}

static const struct irq_domain_ops apple_port_irq_domain_ops = {
	.translate	= irq_domain_translate_onecell,
	.alloc		= apple_port_irq_domain_alloc,
	.free		= apple_port_irq_domain_free,
};

static void apple_port_irq_handler(struct irq_desc *desc)
{
	struct apple_pcie_port *port = irq_desc_get_handler_data(desc);
	struct irq_chip *chip = irq_desc_get_chip(desc);
	unsigned long stat;
	int i;

	chained_irq_enter(chip, desc);

	stat = readl_relaxed(port->base + PORT_INTSTAT);

	for_each_set_bit(i, &stat, 32)
		generic_handle_domain_irq(port->domain, i);

	chained_irq_exit(chip, desc);
}

static int apple_pcie_port_setup_irq(struct apple_pcie_port *port)
{
	struct fwnode_handle *fwnode = &port->np->fwnode;
	struct apple_pcie *pcie = port->pcie;
	unsigned int irq;
	u32 val = 0;

	/* FIXME: consider moving each interrupt under each port */
	irq = irq_of_parse_and_map(to_of_node(dev_fwnode(port->pcie->dev)),
				   port->idx);
	if (!irq)
		return -ENXIO;

	port->domain = irq_domain_create_linear(fwnode, 32,
						&apple_port_irq_domain_ops,
						port);
	if (!port->domain)
		return -ENOMEM;

	/* Disable all interrupts */
	writel_relaxed(~0, port->base + PORT_INTMSK);
	writel_relaxed(~0, port->base + PORT_INTSTAT);
	writel_relaxed(~0, port->base + PORT_LINKCMDSTS);

	irq_set_chained_handler_and_data(irq, apple_port_irq_handler, port);

	/* Configure MSI base address */
	BUILD_BUG_ON(upper_32_bits(DOORBELL_ADDR));
	writel_relaxed(lower_32_bits(DOORBELL_ADDR),
		       port->base + pcie->hw->port_msiaddr);
	if (pcie->hw->port_msiaddr_hi)
		writel_relaxed(0, port->base + pcie->hw->port_msiaddr_hi);

	/* Enable MSIs, shared between all ports */
	if (pcie->hw->port_msimap) {
		for (int i = 0; i < pcie->nvecs; i++)
			writel_relaxed(FIELD_PREP(PORT_MSIMAP_TARGET, i) |
				       PORT_MSIMAP_ENABLE,
				       port->base + pcie->hw->port_msimap + 4 * i);
	} else {
		writel_relaxed(0, port->base + PORT_MSIBASE);
		val = ilog2(pcie->nvecs) << PORT_MSICFG_L2MSINUM_SHIFT;
	}

	writel_relaxed(val | PORT_MSICFG_EN, port->base + PORT_MSICFG);
	return 0;
}

static irqreturn_t apple_pcie_port_irq(int irq, void *data)
{
	struct apple_pcie_port *port = data;
	unsigned int hwirq = irq_domain_get_irq_data(port->domain, irq)->hwirq;

	switch (hwirq) {
	case PORT_INT_LINK_UP:
		dev_info_ratelimited(port->pcie->dev, "Link up on %pOF\n",
				     port->np);
		complete_all(&port->pcie->event);
		break;
	case PORT_INT_LINK_DOWN:
		dev_info_ratelimited(port->pcie->dev, "Link down on %pOF\n",
				     port->np);
		break;
	default:
		return IRQ_NONE;
	}

	return IRQ_HANDLED;
}

static int apple_pcie_port_register_irqs(struct apple_pcie_port *port)
{
	static struct {
		unsigned int	hwirq;
		const char	*name;
	} port_irqs[] = {
		{ PORT_INT_LINK_UP,	"Link up",	},
		{ PORT_INT_LINK_DOWN,	"Link down",	},
	};
	int i;

	for (i = 0; i < ARRAY_SIZE(port_irqs); i++) {
		struct irq_fwspec fwspec = {
			.fwnode		= &port->np->fwnode,
			.param_count	= 1,
			.param		= {
				[0]	= port_irqs[i].hwirq,
			},
		};
		unsigned int irq;
		int ret;

		irq = irq_domain_alloc_irqs(port->domain, 1, NUMA_NO_NODE,
					    &fwspec);
		if (WARN_ON(!irq))
			continue;

		ret = request_irq(irq, apple_pcie_port_irq, 0,
				  port_irqs[i].name, port);
		WARN_ON(ret);
	}

	return 0;
}

static int apple_pcie_setup_refclk(struct apple_pcie *pcie,
				   struct apple_pcie_port *port)
{
	u32 stat;
	int res;

	if (pcie->hw->phy_lane_ctl)
		rmw_set(PHY_LANE_CTL_CFGACC, port->phy + pcie->hw->phy_lane_ctl);

	rmw_set(PHY_LANE_CFG_REFCLK0REQ, port->phy + PHY_LANE_CFG);

	res = readl_relaxed_poll_timeout(port->phy + PHY_LANE_CFG,
					 stat, stat & PHY_LANE_CFG_REFCLK0ACK,
					 100, 50000);
	if (res < 0)
		return res;

	rmw_set(PHY_LANE_CFG_REFCLK1REQ, port->phy + PHY_LANE_CFG);
	res = readl_relaxed_poll_timeout(port->phy + PHY_LANE_CFG,
					 stat, stat & PHY_LANE_CFG_REFCLK1ACK,
					 100, 50000);

	if (res < 0)
		return res;

	if (pcie->hw->phy_lane_ctl)
		rmw_clear(PHY_LANE_CTL_CFGACC, port->phy + pcie->hw->phy_lane_ctl);

	rmw_set(PHY_LANE_CFG_REFCLKEN, port->phy + PHY_LANE_CFG);

	if (pcie->hw->port_refclk)
		rmw_set(PORT_REFCLK_EN, port->base + pcie->hw->port_refclk);

	return 0;
}

static void __iomem *port_rid2sid_addr(struct apple_pcie_port *port, int idx)
{
	return port->base + port->pcie->hw->port_rid2sid + 4 * idx;
}

static u32 apple_pcie_rid2sid_write(struct apple_pcie_port *port,
				    int idx, u32 val)
{
	writel_relaxed(val, port_rid2sid_addr(port, idx));
	/* Read back to ensure completion of the write */
	return readl_relaxed(port_rid2sid_addr(port, idx));
}

static int apple_pcie_setup_link(struct apple_pcie *pcie,
				 struct apple_pcie_port *port,
				 struct device_node *np)
{
#define MAX_AUX_PERST 3
	struct gpio_desc *aux_reset[MAX_AUX_PERST] = { NULL };
	u32 num_aux_resets = 0;
	struct gpio_desc *reset, *pwren = NULL;
	u32 stat;
	int ret;

	/*
	 * A tunnelled link has nothing of its own to sequence. There is no
	 * PERST# pin on a Type-C port, and the reference clock comes from the
	 * ATC PHY, which phy/apple/atc.c has already configured by the time a
	 * cable is up. All that is left is to let the block have its clock and
	 * take the port out of reset.
	 */
	if (pcie->hw->tunnelled) {
		/*
		 * Nothing to sequence here: apple_pcie_tunnel_port_init() runs
		 * the whole thing at the end of setup, the way macOS does.
		 *
		 * Kept for the register dump only.
		 */
		ret = readl_relaxed_poll_timeout(port->base + PORT_STATUS, stat,
						 stat & PORT_STATUS_READY,
						 100, 250000);

		/*
		 * With no cable in the port there is nothing on the far side of
		 * the fabric for the link to reach, so not becoming ready is the
		 * normal state at boot rather than a failure. Keep the port
		 * registered either way: the link-up interrupt is what brings a
		 * tunnel in later, and it only arrives if we are still here.
		 */
		dev_info(pcie->dev, "%pOF: status 0x%08x, link 0x%08x%s\n", np,
			 readl_relaxed(port->base + PORT_STATUS),
			 readl_relaxed(port->base + PORT_LINKSTS),
			 ret < 0 ? " (not ready, no cable?)" : "");
		return 0;
	}

	/*
	 * Assert PERST# and configure the pin as output.
	 * The Aquantia AQC113 10GB nic used desktop macs is sensitive to
	 * deasserting it without prior clock setup.
	 * Observed on M1 Max/Ultra Mac Studios under m1n1's hypervisor.
	 */
	reset = devm_fwnode_gpiod_get(pcie->dev, of_fwnode_handle(np), "reset",
				      GPIOD_OUT_HIGH, "PERST#");
	if (IS_ERR(reset))
		return PTR_ERR(reset);
	// HACK: use additional "reset-gpios" until pci-pwrctrl gains PERST# support.
	for (u32 idx = 0; idx < MAX_AUX_PERST; idx++) {
		aux_reset[idx] = devm_fwnode_gpiod_get_index(pcie->dev,
							     of_fwnode_handle(np),
							     "reset", idx + 1,
							     GPIOD_OUT_HIGH,
							     "PERST#");
		if (IS_ERR(aux_reset[idx])) {
			if (PTR_ERR(aux_reset[idx]) == -ENOENT)
				break;
			else
				return PTR_ERR(aux_reset[idx]);
		}
		num_aux_resets++;
	}
	dev_info(pcie->dev, "Using %u auxiliary PERST#\n", num_aux_resets);

	pwren = devm_fwnode_gpiod_get(pcie->dev, of_fwnode_handle(np), "pwren",
					    GPIOD_ASIS, "PWREN");
	if (IS_ERR(pwren)) {
		if (PTR_ERR(pwren) == -ENOENT)
			pwren = NULL;
		else
			return PTR_ERR(pwren);
	}

	rmw_set(PORT_APPCLK_EN, port->base + PORT_APPCLK);

	/* Assert PERST# before setting up the clock */
	gpiod_set_value_cansleep(reset, 1);
	for (u32 idx = 0; idx < num_aux_resets; idx++)
		gpiod_set_value_cansleep(aux_reset[idx], 1);

	/* Power on the device if required */
	gpiod_set_value_cansleep(pwren, 1);

	ret = apple_pcie_setup_refclk(pcie, port);
	if (ret < 0)
		return ret;

	/*
	 * The minimal Tperst-clk value is 100us (PCIe CEM r5.0, 2.9.2)
	 * If powering up, the minimal Tpvperl is 100ms
	 */
	if (pwren)
		msleep(100);
	else
		usleep_range(100, 200);

	/* Deassert PERST# */
	rmw_set(PORT_PERST_OFF, port->base + pcie->hw->port_perst);
	gpiod_set_value_cansleep(reset, 0);
	for (u32 idx = 0; idx < num_aux_resets; idx++)
		gpiod_set_value_cansleep(aux_reset[idx], 0);

	/* Wait for 100ms after PERST# deassertion (PCIe r5.0, 6.6.1) */
	msleep(100);

	ret = readl_relaxed_poll_timeout(port->base + PORT_STATUS, stat,
					 stat & PORT_STATUS_READY, 100, 250000);
	if (ret < 0) {
		dev_err(pcie->dev, "port %pOF ready wait timeout\n", np);
		return ret;
	}

	return 0;
}

static int apple_pcie_setup_port(struct apple_pcie *pcie,
				 struct device_node *np)
{
	struct platform_device *platform = to_platform_device(pcie->dev);
	struct apple_pcie_port *port;
	struct resource *res;
	char name[16];
	u32 link_stat, idx;
	int ret, i;

	port = devm_kzalloc(pcie->dev, sizeof(*port), GFP_KERNEL);
	if (!port)
		return -ENOMEM;

	port->sid_map = devm_bitmap_zalloc(pcie->dev, pcie->hw->max_rid2sid, GFP_KERNEL);
	if (!port->sid_map)
		return -ENOMEM;

	ret = of_property_read_u32_index(np, "reg", 0, &idx);
	if (ret)
		return ret;

	/* Use the first reg entry to work out the port index */
	port->idx = idx >> 11;
	port->pcie = pcie;
	port->np = np;

	raw_spin_lock_init(&port->lock);

	snprintf(name, sizeof(name), "port%d", port->idx);
	res = platform_get_resource_byname(platform, IORESOURCE_MEM, name);
	if (!res)
		res = platform_get_resource(platform, IORESOURCE_MEM, port->idx + 2);

	port->base = devm_ioremap_resource(&platform->dev, res);
	if (IS_ERR(port->base))
		return PTR_ERR(port->base);

	if (pcie->hw->tunnelled) {
		snprintf(name, sizeof(name), "port%d", port->idx);
		ret = apple_pcie_load_tunable(platform, np, "apple,tunable",
					      name, &port->tunable, false);
		if (ret)
			return ret;
	}

	snprintf(name, sizeof(name), "phy%d", port->idx);
	res = platform_get_resource_byname(platform, IORESOURCE_MEM, name);
	if (res)
		port->phy = devm_ioremap_resource(&platform->dev, res);
	else if (!pcie->hw->tunnelled)
		port->phy = pcie->base + CORE_PHY_DEFAULT_BASE(port->idx);
	/*
	 * A tunnelled root complex has no PCIe PHY of its own - the ATC PHY
	 * owns the lanes - so there is no window to fall back to and nothing
	 * below dereferences this.
	 */

	/* link might be already brought up by u-boot, skip setup then */
	link_stat = readl_relaxed(port->base + PORT_LINKSTS);
	if (!(link_stat & PORT_LINKSTS_UP)) {
		ret = apple_pcie_setup_link(pcie, port, np);
		if (ret)
			return ret;
	}

	if (pcie->hw->port_refclk)
		rmw_clear(PORT_REFCLK_CGDIS, port->base + pcie->hw->port_refclk);
	else
		rmw_set(PHY_LANE_CFG_REFCLKCGEN, port->phy + PHY_LANE_CFG);

	rmw_clear(PORT_APPCLK_CGDIS, port->base + PORT_APPCLK);

	ret = apple_pcie_port_setup_irq(port);
	if (ret)
		return ret;

	/* Reset all RID/SID mappings, and check for RAZ/WI registers */
	for (i = 0; i < pcie->hw->max_rid2sid; i++) {
		if (apple_pcie_rid2sid_write(port, i, 0xbad1d) != 0xbad1d)
			break;
		apple_pcie_rid2sid_write(port, i, 0);
	}

	dev_dbg(pcie->dev, "%pOF: %d RID/SID mapping entries\n", np, i);

	port->sid_map_sz = i;

	list_add_tail(&port->entry, &pcie->ports);
	init_completion(&pcie->event);

	/* In the success path, we keep a reference to np around */
	of_node_get(np);

	ret = apple_pcie_port_register_irqs(port);
	WARN_ON(ret);

	link_stat = readl_relaxed(port->base + PORT_LINKSTS);
	if (pcie->hw->tunnelled) {
		/*
		 * Leave the port the way macOS leaves it between plugs:
		 * configured, clocks on, reset released, link training
		 * stopped. Nothing will answer until a tunnel arrives, so
		 * there is nothing to wait for.
		 */
		if (apple_pcie_tunnel_init_at_probe)
			apple_pcie_tunnel_port_init(pcie, port, false);
	} else if (!(link_stat & PORT_LINKSTS_UP)) {
		unsigned long timeout, left;
		/* start link training */
		writel_relaxed(PORT_LTSSMCTL_START, port->base + PORT_LTSSMCTL);

		timeout = link_up_timeout * HZ / 1000;
		left = wait_for_completion_timeout(&pcie->event, timeout);
		if (!left)
			dev_warn(pcie->dev, "%pOF link didn't come up\n", np);
		else
			dev_info(pcie->dev, "%pOF link up after %ldms\n", np,
				 (timeout - left) * 1000 / HZ);

	}

	return 0;
}

static const struct msi_parent_ops apple_msi_parent_ops = {
	.supported_flags	= (MSI_GENERIC_FLAGS_MASK	|
				   MSI_FLAG_PCI_MSIX		|
				   MSI_FLAG_MULTI_PCI_MSI),
	.required_flags		= (MSI_FLAG_USE_DEF_DOM_OPS	|
				   MSI_FLAG_USE_DEF_CHIP_OPS	|
				   MSI_FLAG_PCI_MSI_MASK_PARENT),
	.chip_flags		= MSI_CHIP_FLAG_SET_EOI,
	.bus_select_token	= DOMAIN_BUS_PCI_MSI,
	.init_dev_msi_info	= msi_lib_init_dev_msi_info,
};

static int apple_msi_init(struct apple_pcie *pcie)
{
	struct fwnode_handle *fwnode = dev_fwnode(pcie->dev);
	struct irq_domain_info info = {
		.fwnode		= fwnode,
		.ops		= &apple_msi_domain_ops,
		.size		= pcie->nvecs,
		.host_data	= pcie,
	};
	struct of_phandle_args args = {};
	int ret;

	ret = of_parse_phandle_with_args(to_of_node(fwnode), "msi-ranges",
					 "#interrupt-cells", 0, &args);
	if (ret)
		return ret;

	ret = of_property_read_u32_index(to_of_node(fwnode), "msi-ranges",
					 args.args_count + 1, &pcie->nvecs);
	if (ret)
		return ret;

	of_phandle_args_to_fwspec(args.np, args.args, args.args_count,
				  &pcie->fwspec);

	pcie->bitmap = devm_bitmap_zalloc(pcie->dev, pcie->nvecs, GFP_KERNEL);
	if (!pcie->bitmap)
		return -ENOMEM;

	info.parent = irq_find_matching_fwspec(&pcie->fwspec, DOMAIN_BUS_WIRED);
	if (!info.parent) {
		dev_err(pcie->dev, "failed to find parent domain\n");
		return -ENXIO;
	}

	if (!msi_create_parent_irq_domain(&info, &apple_msi_parent_ops)) {
		dev_err(pcie->dev, "failed to create IRQ domain\n");
		return -ENOMEM;
	}
	return 0;
}

static struct apple_pcie *apple_pcie_lookup(struct device *dev)
{
	return pci_host_bridge_priv(dev_get_drvdata(dev));
}

static struct apple_pcie_port *apple_pcie_get_port(struct pci_dev *pdev)
{
	struct pci_config_window *cfg = pdev->sysdata;
	struct apple_pcie *pcie;
	struct pci_dev *port_pdev;
	struct apple_pcie_port *port;

	pcie = apple_pcie_lookup(cfg->parent);
	if (WARN_ON(!pcie))
		return NULL;

	/* Find the root port this device is on */
	port_pdev = pcie_find_root_port(pdev);

	/* If finding the port itself, nothing to do */
	if (WARN_ON(!port_pdev) || pdev == port_pdev)
		return NULL;

	list_for_each_entry(port, &pcie->ports, entry) {
		if (port->idx == PCI_SLOT(port_pdev->devfn))
			return port;
	}

	return NULL;
}

static int apple_pcie_enable_device(struct pci_host_bridge *bridge, struct pci_dev *pdev)
{
	u32 sid, rid = pci_dev_id(pdev);
	struct apple_pcie_port *port;
	int idx, err;

	port = apple_pcie_get_port(pdev);
	if (!port)
		return 0;

	dev_dbg(&pdev->dev, "added to bus %s, index %d\n",
		pci_name(pdev->bus->self), port->idx);

	err = of_map_id(port->pcie->dev->of_node, rid, "iommu-map",
			"iommu-map-mask", NULL, &sid);
	if (err)
		return err;

	mutex_lock(&port->pcie->lock);

	idx = bitmap_find_free_region(port->sid_map, port->sid_map_sz, 0);
	if (idx >= 0) {
		apple_pcie_rid2sid_write(port, idx,
					 PORT_RID2SID_VALID |
					 (sid << PORT_RID2SID_SID_SHIFT) | rid);

		dev_dbg(&pdev->dev, "mapping RID%x to SID%x (index %d)\n",
			rid, sid, idx);
	}

	mutex_unlock(&port->pcie->lock);

	return idx >= 0 ? 0 : -ENOSPC;
}

static void apple_pcie_disable_device(struct pci_host_bridge *bridge, struct pci_dev *pdev)
{
	struct apple_pcie_port *port;
	u32 rid = pci_dev_id(pdev);
	int idx;

	port = apple_pcie_get_port(pdev);
	if (!port)
		return;

	mutex_lock(&port->pcie->lock);

	for_each_set_bit(idx, port->sid_map, port->sid_map_sz) {
		u32 val;

		val = readl_relaxed(port_rid2sid_addr(port, idx));
		if ((val & 0xffff) == rid) {
			apple_pcie_rid2sid_write(port, idx, 0);
			bitmap_release_region(port->sid_map, idx, 0);
			dev_dbg(&pdev->dev, "Released %x (%d)\n", val, idx);
			break;
		}
	}

	mutex_unlock(&port->pcie->lock);
}

static int apple_pcie_init(struct pci_config_window *cfg)
{
	struct device *dev = cfg->parent;
	struct apple_pcie *pcie;
	int ret;

	pcie = apple_pcie_lookup(dev);
	if (WARN_ON(!pcie))
		return -ENOENT;

	for_each_available_child_of_node_scoped(dev->of_node, of_port) {
		ret = apple_pcie_setup_port(pcie, of_port);
		if (ret) {
			dev_err(dev, "Port %pOF setup fail: %d\n", of_port, ret);
			return ret;
		}
	}

	return 0;
}

static const struct pci_ecam_ops apple_pcie_cfg_ecam_ops = {
	.init		= apple_pcie_init,
	.enable_device	= apple_pcie_enable_device,
	.disable_device	= apple_pcie_disable_device,
	.pci_ops	= {
		.map_bus	= pci_ecam_map_bus,
		.read		= pci_generic_config_read,
		.write		= pci_generic_config_write,
	}
};

static int apple_pcie_probe_port(struct device_node *np, const struct hw_info *hw)
{
	struct gpio_desc *gd;

	/* A tunnelled port has neither GPIO; there is nothing to wait for. */
	if (hw->tunnelled)
		return 0;

	/* check whether the GPPIO pin exists but leave it as is */
	gd = fwnode_gpiod_get_index(of_fwnode_handle(np), "reset", 0,
				    GPIOD_ASIS, "PERST#");
	if (IS_ERR(gd))
		return PTR_ERR(gd);

	gpiod_put(gd);

	gd = fwnode_gpiod_get_index(of_fwnode_handle(np), "pwren", 0,
				    GPIOD_ASIS, "PWREN");
	if (IS_ERR(gd)) {
		if (PTR_ERR(gd) != -ENOENT)
			return PTR_ERR(gd);
	} else {
		gpiod_put(gd);
	}

	return 0;
}

static int apple_pcie_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct pci_host_bridge *bridge;
	struct device_node *of_port;
	struct apple_pcie *pcie;
	const struct hw_info *hw;
	int ret;

	hw = of_device_get_match_data(dev);
	if (!hw)
		return -ENODEV;

	if (hw->tunnelled && !apple_pcie_enable_tunnel) {
		dev_info(dev, "tunnelled root complex left alone (pass pcie_apple.enable_tunnel=1 to bring it up)\n");
		return -ENODEV;
	}

	/* Check for probe dependencies for all ports first */
	for_each_available_child_of_node(dev->of_node, of_port) {
		ret = apple_pcie_probe_port(of_port, hw);
		if (ret) {
			of_node_put(of_port);
			return dev_err_probe(dev, ret, "Port %pOF probe fail\n", of_port);
		}
	}

	bridge = devm_pci_alloc_host_bridge(dev, sizeof(*pcie));
	if (!bridge)
		return -ENOMEM;

	pcie = pci_host_bridge_priv(bridge);
	pcie->dev = dev;
	pcie->hw = hw;

	pcie->base = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(pcie->base))
		return PTR_ERR(pcie->base);

	/*
	 * Load the tunables but do not write anything here. macOS programs
	 * them as the first step of bringing a tunnel up, not at boot, and
	 * every attempt to write any of them at probe has ended in an
	 * asynchronous SError before login. Probe touches exactly what it
	 * touched before tunables existed; apple_pcie_tunnel_up() does the
	 * writing, in macOS's order.
	 */
	if (hw->tunnelled) {
		for (int i = 0; i < ARRAY_SIZE(apple_pcie_tunables); i++) {
			ret = apple_pcie_load_tunable(pdev, dev->of_node,
						apple_pcie_tunables[i].name,
						apple_pcie_tunables[i].reg_name,
						&pcie->tunables[i], false);
			if (ret)
				return ret;
		}
	}

	mutex_init(&pcie->lock);
	INIT_LIST_HEAD(&pcie->ports);

	ret = apple_msi_init(pcie);
	if (ret)
		return ret;

	return pci_host_common_init(pdev, bridge, &apple_pcie_cfg_ecam_ops);
}

static const struct hw_info t6000_pciec_hw = {
	.phy_lane_ctl		= 0,
	.port_msiaddr		= PORT_MSIADDR,
	.port_msiaddr_hi	= 0,
	.port_refclk		= PORT_REFCLK,
	.port_perst		= PORT_PERST,
	.port_rid2sid		= PORT_RID2SID,
	.port_msimap		= 0,
	.max_rid2sid		= 64,
	.tunnelled		= true,
};

/*
 * The sequence macOS runs on a tunnelled port, transcribed from a trace of it
 * (traces/macos-apciec in the asahi-dp-altmode tree). It runs this twice: once
 * at boot, leaving the port configured but with link training stopped, and
 * again when a tunnel arrives, that time starting training at the end.
 *
 * Doing it only on the second occasion is not enough. A port that never had
 * the first pass is in a state the fabric does not expect, and touching it
 * once a tunnel is live takes the machine down - with any write at all, not
 * a particular one.
 */
#define tw(off, val) do {						\
	if (apple_pcie_tunnel_dry_run)					\
		dev_info(pcie->dev, "DRY W.4 %#07x = %#x\n",		\
			 (unsigned int)(off), (unsigned int)(val));	\
	else								\
		writel_relaxed((val), port->base + (off));		\
} while (0)

#define tset(bits, off) do {						\
	if (apple_pcie_tunnel_dry_run)					\
		dev_info(pcie->dev, "DRY RMW %#07x |= %#x\n",		\
			 (unsigned int)(off), (unsigned int)(bits));	\
	else								\
		rmw_set((bits), port->base + (off));			\
} while (0)

#define tclr(bits, off) do {						\
	if (apple_pcie_tunnel_dry_run)					\
		dev_info(pcie->dev, "DRY RMW %#07x &= ~%#x\n",		\
			 (unsigned int)(off), (unsigned int)(bits));	\
	else								\
		rmw_clear((bits), port->base + (off));			\
} while (0)

static void apple_pcie_tunnel_port_init(struct apple_pcie *pcie,
					struct apple_pcie_port *port,
					bool start_link)
{
	unsigned int sids = port->sid_map_sz ?: pcie->hw->max_rid2sid;

	/* The two tunables macOS re-applies every time; "rc" it never writes. */
	for (int i = 0; i < ARRAY_SIZE(pcie->tunables); i++) {
		if (apple_pcie_tunnel_dry_run)
			dev_info(pcie->dev, "DRY tunable %d (%zu entries)\n", i,
				 pcie->tunables[i].values ?
					pcie->tunables[i].values->sz : 0);
		else
			apple_pcie_tunable_apply(&pcie->tunables[i]);
	}

	/* Take the port down. */
	tw(PORT_TUNNEL_PRE_RESET, 0x110);
	tw(PORT_INTSTAT, ~0);
	tw(PORT_TUNNEL_CLRSTS, ~0);
	tw(PORT_LINKCMDSTS, ~0);
	tw(PORT_LTSSMCTL, 0);

	if (apple_pcie_tunnel_dry_run)
		dev_info(pcie->dev, "DRY clear %u RID/SID entries\n", sids);
	else
		for (int i = 0; i < sids; i++)
			apple_pcie_rid2sid_write(port, i, 0);

	tw(PORT_INTMSK, ~0);
	tw(PORT_MSICFG, 0);
	tw(PORT_MSIBASE, 0);
	tw(pcie->hw->port_msiaddr, 0);
	tw(PORT_TUNNEL_CFG13C, 0x10);

	tclr(PORT_APPCLK_EN, PORT_APPCLK);
	tset(PORT_APPCLK_CGDIS, PORT_APPCLK);
	tw(PORT_TUNNEL_CFG808, 0x100045);
	tclr(PORT_REFCLK_EN, PORT_REFCLK);
	tset(PORT_REFCLK_CGDIS, PORT_REFCLK);
	tclr(PORT_PERST_OFF, pcie->hw->port_perst);

	tw(PORT_TUNNEL_CFG130, 0x208);
	tw(PORT_TUNNEL_CFG140, 0x10);
	tw(PORT_TUNNEL_CFG144, 0x253770);
	tw(PORT_TUNNEL_CFG21C, 0);
	tw(PORT_TUNNEL_CFG81C, 0);

	/* The port's own tunable sets bit 0 of the 0x10 just written. */
	if (apple_pcie_tunnel_dry_run)
		dev_info(pcie->dev, "DRY port tunable\n");
	else
		apple_pcie_tunable_apply(&port->tunable);

	/* And back up. */
	tset(PORT_PERST_OFF, pcie->hw->port_perst);
	tset(PORT_APPCLK_EN, PORT_APPCLK);
	tclr(PORT_APPCLK_CGDIS, PORT_APPCLK);
	tw(PORT_TUNNEL_ARM, 3);

	/* Put back the MSI configuration the reset above cleared. */
	tw(PORT_INTSTAT, ~0);
	writel_relaxed(lower_32_bits(DOORBELL_ADDR),
		       port->base + pcie->hw->port_msiaddr);
	tw(PORT_MSIBASE, 0);
	tw(PORT_MSICFG, (ilog2(pcie->nvecs) << PORT_MSICFG_L2MSINUM_SHIFT) |
		       PORT_MSICFG_EN);

	if (start_link) {
		reinit_completion(&pcie->event);
		tw(PORT_LTSSMCTL, PORT_LTSSMCTL_START);
	}
}

int apple_pcie_tunnel_up(struct platform_device *pdev)
{
	struct apple_pcie *pcie = apple_pcie_lookup(&pdev->dev);
	struct pci_host_bridge *bridge;
	struct apple_pcie_port *port;
	unsigned long timeout, left;
	u32 link_stat;

	if (!pcie || !pcie->hw || !pcie->hw->tunnelled)
		return -ENODEV;

	bridge = pci_host_bridge_from_priv(pcie);
	if (!bridge || !bridge->bus)
		return -ENODEV;

	port = list_first_entry_or_null(&pcie->ports, struct apple_pcie_port,
					entry);
	if (!port)
		return -ENODEV;

	dev_info(pcie->dev, "tunnel up: status 0x%08x, link 0x%08x\n",
		 readl_relaxed(port->base + PORT_STATUS),
		 readl_relaxed(port->base + PORT_LINKSTS));

	apple_pcie_tunnel_port_init(pcie, port, true);

	timeout = link_up_timeout * HZ / 1000;
	left = wait_for_completion_timeout(&pcie->event, timeout);
	link_stat = readl_relaxed(port->base + PORT_LINKSTS);
	dev_info(pcie->dev, "after bring-up: status 0x%08x, link 0x%08x%s\n",
		 readl_relaxed(port->base + PORT_STATUS), link_stat,
		 left ? "" : " (timed out)");

	if (!(link_stat & PORT_LINKSTS_UP))
		return -ENODEV;

	pci_lock_rescan_remove();
	pci_rescan_bus(bridge->bus);
	pci_unlock_rescan_remove();
	dev_info(pcie->dev, "tunnel up: done\n");

	return 0;
}
EXPORT_SYMBOL_GPL(apple_pcie_tunnel_up);

static const struct of_device_id apple_pcie_of_match[] = {
	{ .compatible = "apple,t6000-pciec",	.data = &t6000_pciec_hw },
	{ .compatible = "apple,t6020-pcie",	.data = &t602x_hw },
	{ .compatible = "apple,pcie",		.data = &t8103_hw },
	{ }
};
MODULE_DEVICE_TABLE(of, apple_pcie_of_match);

static struct platform_driver apple_pcie_driver = {
	.probe	= apple_pcie_probe,
	.driver	= {
		.name			= "pcie-apple",
		.of_match_table		= apple_pcie_of_match,
		.suppress_bind_attrs	= true,
	},
};
module_platform_driver(apple_pcie_driver);

MODULE_DESCRIPTION("Apple PCIe host bridge driver");
MODULE_LICENSE("GPL v2");
