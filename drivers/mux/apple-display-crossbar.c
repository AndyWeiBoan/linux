// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Apple Silicon Display Crossbar multiplexer driver
 *
 * Copyright (C) Asahi Linux Contributors
 *
 * Author: Sven Peter <sven@svenpeter.dev>
 */

#include <linux/bitmap.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mux/driver.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>

/*
 * T602x register interface is cleary different so most of the names below are
 * probably wrong.
 */

#define T602X_FIFO_WR_DPTX_CLK_EN 0x000
#define T602X_FIFO_WR_N_CLK_EN 0x004
#define T602X_FIFO_WR_UNK_EN 0x008
#define T602X_REG_00C 0x00c
#define T602X_REG_014 0x014
#define T602X_REG_018 0x018
#define T602X_REG_01C 0x01c
#define T602X_FIFO_RD_PCLK2_EN 0x024
#define T602X_FIFO_RD_N_CLK_EN 0x028
#define T602X_FIFO_RD_UNK_EN 0x02c
#define T602X_REG_030 0x030
#define T602X_REG_034 0x034

#define T602X_REG_804_STAT 0x804 // status of 0x004
#define T602X_REG_810_STAT 0x810 // status of 0x014
#define T602X_REG_81C_STAT 0x81c // status of 0x024

/*
 * T8013, T600x, T8112 dp crossbar registers.
 */

#define FIFO_WR_DPTX_CLK_EN 0x000
#define FIFO_WR_N_CLK_EN 0x004
#define FIFO_WR_UNK_EN 0x008
#define FIFO_RD_PCLK1_EN 0x020
#define FIFO_RD_PCLK2_EN 0x024
#define FIFO_RD_N_CLK_EN 0x028
#define FIFO_RD_UNK_EN 0x02c

#define OUT_PCLK1_EN 0x040
#define OUT_PCLK2_EN 0x044
#define OUT_N_CLK_EN 0x048
#define OUT_UNK_EN 0x04c

#define CROSSBAR_DISPEXT_EN 0x050
#define CROSSBAR_MUX_CTRL 0x060
#define CROSSBAR_MUX_CTRL_DPPHY_SELECT0 GENMASK(23, 20)
#define CROSSBAR_MUX_CTRL_DPIN1_SELECT0 GENMASK(19, 16)
#define CROSSBAR_MUX_CTRL_DPIN0_SELECT0 GENMASK(15, 12)
#define CROSSBAR_MUX_CTRL_DPPHY_SELECT1 GENMASK(11, 8)
#define CROSSBAR_MUX_CTRL_DPIN1_SELECT1 GENMASK(7, 4)
#define CROSSBAR_MUX_CTRL_DPIN0_SELECT1 GENMASK(3, 0)
#define CROSSBAR_ATC_EN 0x070

#define FIFO_WR_DPTX_CLK_EN_STAT 0x800
#define FIFO_WR_N_CLK_EN_STAT 0x804
#define FIFO_RD_PCLK1_EN_STAT 0x820
#define FIFO_RD_PCLK2_EN_STAT 0x824
#define FIFO_RD_N_CLK_EN_STAT 0x828

#define OUT_PCLK1_EN_STAT 0x840
#define OUT_PCLK2_EN_STAT 0x844
#define OUT_N_CLK_EN_STAT 0x848

#define UNK_TUNABLE 0xc00

#define ATC_DPIN0 BIT(0)
#define ATC_DPIN1 BIT(4)
#define ATC_DPPHY BIT(8)

/*
 * macOS writes 0 here on t6000, every time, right before it enables the
 * crossbar clocks - 10 writes across 6 independent m1n1 hypervisor traces,
 * never 5. With 5 the FIFO clock status registers (0x800/0x820/0x840) stay
 * at 0 even with a trained DP link and a mode set, so no pixels reach the
 * Thunderbolt DP IN adapter and the tunnel reports 0 Mb/s consumed.
 * -1 keeps the per-SoC value.
 */
static int tunable_override = -1;
module_param(tunable_override, int, 0644);

/*
 * Touching the DP IN adapters' blocks is off by default and must stay that
 * way: this driver selects the crossbar at DCP probe time, long before the
 * Type-C PHY is in Thunderbolt mode and ACIO is powered, and writing those
 * registers while the block is unpowered takes the machine down with
 * "Asynchronous SError Interrupt" (see the warning in
 * drivers/thunderbolt/apple.c about ACIO ordering).
 *
 * Turn it on from userspace once a tunnel exists, then re-run the crossbar
 * selection so the DP IN adapter is armed before the clocks start, which is
 * the order macOS uses.
 */
static bool dpin_program;
module_param(dpin_program, bool, 0644);

/*
 * The Thunderbolt DP IN adapters have a small SoC-side block of their own,
 * next to the crossbar. macOS programs it as part of bringing a tunnelled
 * display up, in this order (captured under the m1n1 hypervisor):
 *
 *   dpin: ENABLE = 3, REG_04 = 1, MODE = 5      <- before the crossbar
 *   crossbar: MUX_CTRL
 *   dpin: HOLD = 0
 *   crossbar: clock and enable bits
 *
 * Linux has never had a driver or even a device tree node for it. REG_00 and
 * REG_04 are hardware status (writes do not stick); ENABLE and HOLD are the
 * ones that matter.
 */
#define DPIN_MODE 0x00		/* ro: 5 armed, 4 streaming */
#define DPIN_REG_04 0x04	/* ro: 1 armed, 2 streaming */
#define DPIN_ENABLE 0x08	/* 3 = on, 0 = off */
#define DPIN_HOLD 0x0c		/* 1 = held, 0 = released */
#define DPIN_STATUS 0x10	/* follows HOLD */

enum { MUX_DPPHY = 0, MUX_DPIN0 = 1, MUX_DPIN1 = 2, MUX_MAX = 3 };
static const char *apple_dpxbar_names[MUX_MAX] = { "dpphy", "dpin0", "dpin1" };

struct apple_dpxbar_hw {
	unsigned int n_ufp;
	u32 tunable;
	const struct mux_control_ops *ops;
};

struct apple_dpxbar {
	struct device *dev;
	void __iomem *regs;
	/* optional, indexed by MUX_DPIN0 / MUX_DPIN1 */
	void __iomem *dpin[MUX_MAX];
	int selected_dispext[MUX_MAX];
	spinlock_t lock;
};

static inline void dpxbar_mask32(struct apple_dpxbar *xbar, u32 reg, u32 mask,
				 u32 set)
{
	u32 value = readl(xbar->regs + reg);
	value &= ~mask;
	value |= set;
	writel(value, xbar->regs + reg);
}

static inline void dpxbar_set32(struct apple_dpxbar *xbar, u32 reg, u32 set)
{
	dpxbar_mask32(xbar, reg, 0, set);
}

static inline void dpxbar_clear32(struct apple_dpxbar *xbar, u32 reg, u32 clear)
{
	dpxbar_mask32(xbar, reg, clear, 0);
}

static int apple_dpxbar_set_t602x(struct mux_control *mux, int state)
{
	struct apple_dpxbar *dpxbar = mux_chip_priv(mux->chip);
	unsigned int index = mux_control_get_index(mux);
	unsigned long flags;
	unsigned int mux_state;
	unsigned int dispext_bit;
	unsigned int dispext_bit_en;
	bool enable;
	int ret = 0;

	if (state == MUX_IDLE_DISCONNECT) {
		/*
		 * Technically this will select dispext0,0 in the mux control
		 * register. Practically that doesn't matter since everything
		 * else is disabled.
		 */
		mux_state = 0;
		enable = false;
	} else if (state >= 0 && state < 9) {
		dispext_bit = 1 << state;
		dispext_bit_en = 1 << (2 * state);
		mux_state = state;
		enable = true;
	} else {
		return -EINVAL;
	}

	spin_lock_irqsave(&dpxbar->lock, flags);

	/* ensure the selected dispext isn't already used in this crossbar */
	if (enable) {
		for (int i = 0; i < MUX_MAX; ++i) {
			if (i == index)
				continue;
			if (dpxbar->selected_dispext[i] == state) {
				spin_unlock_irqrestore(&dpxbar->lock, flags);
				return -EBUSY;
			}
		}
	}

	if (dpxbar->selected_dispext[index] >= 0) {
		u32 prev_dispext_bit = 1 << dpxbar->selected_dispext[index];
		u32 prev_dispext_bit_en = 1 << (2 * dpxbar->selected_dispext[index]);

		dpxbar_clear32(dpxbar, T602X_FIFO_RD_UNK_EN, prev_dispext_bit);
		dpxbar_clear32(dpxbar, T602X_FIFO_WR_DPTX_CLK_EN, prev_dispext_bit);
		dpxbar_clear32(dpxbar, T602X_REG_00C, prev_dispext_bit_en);

		dpxbar_clear32(dpxbar, T602X_REG_01C, 0x100);

		dpxbar_clear32(dpxbar, T602X_FIFO_WR_UNK_EN, prev_dispext_bit);
		dpxbar_clear32(dpxbar, T602X_REG_018, prev_dispext_bit_en);

		dpxbar_clear32(dpxbar, T602X_FIFO_RD_N_CLK_EN, 0x100);

		dpxbar_set32(dpxbar, T602X_FIFO_WR_N_CLK_EN, prev_dispext_bit);
		dpxbar_set32(dpxbar, T602X_REG_014, 0x4);

		dpxbar_set32(dpxbar, FIFO_RD_PCLK1_EN, 0x100);

		dpxbar->selected_dispext[index] = -1;
	}

	if (enable) {
		dpxbar_set32(dpxbar, T602X_REG_030, state << 20);
		dpxbar_set32(dpxbar, T602X_REG_030, state << 8);
		udelay(10);

		dpxbar_clear32(dpxbar, T602X_FIFO_WR_N_CLK_EN, dispext_bit);
		dpxbar_clear32(dpxbar, T602X_REG_014, 0x4);

		dpxbar_clear32(dpxbar, T602X_FIFO_RD_PCLK2_EN, 0x100);

		dpxbar_set32(dpxbar, T602X_FIFO_WR_UNK_EN, dispext_bit);
		dpxbar_set32(dpxbar, T602X_REG_018, dispext_bit_en);

		dpxbar_set32(dpxbar, T602X_FIFO_RD_N_CLK_EN, 0x100);
		dpxbar_set32(dpxbar, T602X_FIFO_WR_DPTX_CLK_EN, dispext_bit);
		dpxbar_set32(dpxbar, T602X_REG_00C, dispext_bit);

		dpxbar_set32(dpxbar, T602X_REG_01C, 0x100);
		dpxbar_set32(dpxbar, T602X_REG_034, 0x100);

		dpxbar_set32(dpxbar, T602X_FIFO_RD_UNK_EN, dispext_bit);

		dpxbar->selected_dispext[index] = state;
	}

	spin_unlock_irqrestore(&dpxbar->lock, flags);

	if (enable)
		dev_info(dpxbar->dev, "Switched %s to dispext%u,%u\n",
			 apple_dpxbar_names[index], mux_state >> 1,
			 mux_state & 1);
	else
		dev_info(dpxbar->dev, "Switched %s to disconnected state\n",
			 apple_dpxbar_names[index]);

	return ret;
}

static int apple_dpxbar_set(struct mux_control *mux, int state)
{
	struct apple_dpxbar *dpxbar = mux_chip_priv(mux->chip);
	unsigned int index = mux_control_get_index(mux);
	unsigned long flags;
	unsigned int mux_state;
	unsigned int dispext_bit;
	unsigned int dispext_bit_en;
	unsigned int atc_bit;
	bool enable;
	int ret = 0;
	u32 mux_mask, mux_set;

	if (state == MUX_IDLE_DISCONNECT) {
		/*
		 * Technically this will select dispext0,0 in the mux control
		 * register. Practically that doesn't matter since everything
		 * else is disabled.
		 */
		mux_state = 0;
		enable = false;
	} else if (state >= 0 && state < 9) {
		dispext_bit = 1 << state;
		dispext_bit_en = 1 << (2 * state);
		mux_state = state;
		enable = true;
	} else {
		return -EINVAL;
	}

	switch (index) {
	case MUX_DPPHY:
		mux_mask = CROSSBAR_MUX_CTRL_DPPHY_SELECT0 |
			   CROSSBAR_MUX_CTRL_DPPHY_SELECT1;
		mux_set =
			FIELD_PREP(CROSSBAR_MUX_CTRL_DPPHY_SELECT0, mux_state) |
			FIELD_PREP(CROSSBAR_MUX_CTRL_DPPHY_SELECT1, mux_state);
		atc_bit = ATC_DPPHY;
		break;
	case MUX_DPIN0:
		mux_mask = CROSSBAR_MUX_CTRL_DPIN0_SELECT0 |
			   CROSSBAR_MUX_CTRL_DPIN0_SELECT1;
		mux_set =
			FIELD_PREP(CROSSBAR_MUX_CTRL_DPIN0_SELECT0, mux_state) |
			FIELD_PREP(CROSSBAR_MUX_CTRL_DPIN0_SELECT1, mux_state);
		atc_bit = ATC_DPIN0;
		break;
	case MUX_DPIN1:
		mux_mask = CROSSBAR_MUX_CTRL_DPIN1_SELECT0 |
			   CROSSBAR_MUX_CTRL_DPIN1_SELECT1;
		mux_set =
			FIELD_PREP(CROSSBAR_MUX_CTRL_DPIN1_SELECT0, mux_state) |
			FIELD_PREP(CROSSBAR_MUX_CTRL_DPIN1_SELECT1, mux_state);
		atc_bit = ATC_DPIN1;
		break;
	default:
		return -EINVAL;
	}

	spin_lock_irqsave(&dpxbar->lock, flags);

	/* ensure the selected dispext isn't already used in this crossbar */
	if (enable) {
		for (int i = 0; i < MUX_MAX; ++i) {
			if (i == index)
				continue;
			if (dpxbar->selected_dispext[i] == state) {
				spin_unlock_irqrestore(&dpxbar->lock, flags);
				return -EBUSY;
			}
		}
	}

	if (!enable && dpin_program && dpxbar->dpin[index]) {
		writel(1, dpxbar->dpin[index] + DPIN_HOLD);
		writel(0, dpxbar->dpin[index] + DPIN_ENABLE);
	}

	dpxbar_set32(dpxbar, OUT_N_CLK_EN, atc_bit);
	dpxbar_clear32(dpxbar, OUT_UNK_EN, atc_bit);
	dpxbar_clear32(dpxbar, OUT_PCLK1_EN, atc_bit);
	dpxbar_clear32(dpxbar, CROSSBAR_ATC_EN, atc_bit);

	if (dpxbar->selected_dispext[index] >= 0) {
		u32 prev_dispext_bit = 1 << dpxbar->selected_dispext[index];
		u32 prev_dispext_bit_en = 1 << (2 * dpxbar->selected_dispext[index]);

		dpxbar_set32(dpxbar, FIFO_WR_N_CLK_EN, prev_dispext_bit);
		dpxbar_set32(dpxbar, FIFO_RD_N_CLK_EN, prev_dispext_bit);
		dpxbar_clear32(dpxbar, FIFO_WR_UNK_EN, prev_dispext_bit);
		dpxbar_clear32(dpxbar, FIFO_RD_UNK_EN, prev_dispext_bit_en);
		dpxbar_clear32(dpxbar, FIFO_WR_DPTX_CLK_EN, prev_dispext_bit);
		dpxbar_clear32(dpxbar, FIFO_RD_PCLK1_EN, prev_dispext_bit);
		dpxbar_clear32(dpxbar, CROSSBAR_DISPEXT_EN, prev_dispext_bit);

		dpxbar->selected_dispext[index] = -1;
	}

	dpxbar_mask32(dpxbar, CROSSBAR_MUX_CTRL, mux_mask, mux_set);

	if (enable) {
		/*
		 * Arm the DP IN adapter before the crossbar starts feeding it,
		 * which is the order macOS uses. Doing it afterwards (by hand
		 * from userspace) makes no difference.
		 */
		if (dpin_program && dpxbar->dpin[index])
			writel(3, dpxbar->dpin[index] + DPIN_ENABLE);

		/* macOS writes the tunable here, just before the clock enables */
		writel(tunable_override >= 0 ? (u32)tunable_override : 0,
		       dpxbar->regs + UNK_TUNABLE);

		dpxbar_clear32(dpxbar, FIFO_WR_N_CLK_EN, dispext_bit);
		dpxbar_clear32(dpxbar, FIFO_RD_N_CLK_EN, dispext_bit);
		dpxbar_clear32(dpxbar, OUT_N_CLK_EN, atc_bit);
		dpxbar_set32(dpxbar, FIFO_WR_UNK_EN, dispext_bit);
		dpxbar_set32(dpxbar, FIFO_RD_UNK_EN, dispext_bit_en);
		dpxbar_set32(dpxbar, OUT_UNK_EN, atc_bit);
		dpxbar_set32(dpxbar, FIFO_WR_DPTX_CLK_EN, dispext_bit);
		dpxbar_set32(dpxbar, FIFO_RD_PCLK1_EN, dispext_bit);
		dpxbar_set32(dpxbar, OUT_PCLK1_EN, atc_bit);
		dpxbar_set32(dpxbar, CROSSBAR_ATC_EN, atc_bit);
		dpxbar_set32(dpxbar, CROSSBAR_DISPEXT_EN, dispext_bit);

		/*
		 * Work around some HW quirk:
		 * Without toggling the RD_PCLK enable here the connection
		 * doesn't come up. Testing has shown that a delay of about
		 * 5 usec is required which is doubled here to be on the
		 * safe side.
		 */
		dpxbar_clear32(dpxbar, FIFO_RD_PCLK1_EN, dispext_bit);
		udelay(10);
		dpxbar_set32(dpxbar, FIFO_RD_PCLK1_EN, dispext_bit);

		/* release the DP IN adapter now that it has a source */
		if (dpin_program && dpxbar->dpin[index])
			writel(0, dpxbar->dpin[index] + DPIN_HOLD);

		dpxbar->selected_dispext[index] = state;
	}

	spin_unlock_irqrestore(&dpxbar->lock, flags);

	if (enable)
		dev_info(dpxbar->dev, "Switched %s to dispext%u,%u\n",
			 apple_dpxbar_names[index], mux_state >> 1,
			 mux_state & 1);
	else
		dev_info(dpxbar->dev, "Switched %s to disconnected state\n",
			 apple_dpxbar_names[index]);

	return ret;
}

static const struct mux_control_ops apple_dpxbar_ops = {
	.set = apple_dpxbar_set,
};

static const struct mux_control_ops apple_dpxbar_t602x_ops = {
	.set = apple_dpxbar_set_t602x,
};

static const struct { u16 off; const char *name; } dpxbar_regs[] = {
	{ 0x000, "FIFO_WR_DPTX_CLK_EN" }, { 0x004, "FIFO_WR_N_CLK_EN" },
	{ 0x008, "FIFO_WR_UNK_EN" },      { 0x020, "FIFO_RD_PCLK1_EN" },
	{ 0x024, "FIFO_RD_PCLK2_EN" },    { 0x028, "FIFO_RD_N_CLK_EN" },
	{ 0x02c, "FIFO_RD_UNK_EN" },      { 0x040, "OUT_PCLK1_EN" },
	{ 0x044, "OUT_PCLK2_EN" },        { 0x048, "OUT_N_CLK_EN" },
	{ 0x04c, "OUT_UNK_EN" },          { 0x050, "CROSSBAR_DISPEXT_EN" },
	{ 0x060, "CROSSBAR_MUX_CTRL" },   { 0x070, "CROSSBAR_ATC_EN" },
	{ 0x800, "WR_DPTX_CLK_EN_STAT" }, { 0x804, "WR_N_CLK_EN_STAT" },
	{ 0x820, "RD_PCLK1_EN_STAT" },    { 0x824, "RD_PCLK2_EN_STAT" },
	{ 0x828, "RD_N_CLK_EN_STAT" },    { 0x840, "OUT_PCLK1_EN_STAT" },
	{ 0x844, "OUT_PCLK2_EN_STAT" },   { 0x848, "OUT_N_CLK_EN_STAT" },
	{ 0xc00, "UNK_TUNABLE" },
};

static ssize_t regs_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct mux_chip *chip = dev_get_drvdata(dev);
	struct apple_dpxbar *dpxbar = mux_chip_priv(chip);
	int len = 0;

	for (int i = 0; i < ARRAY_SIZE(dpxbar_regs); i++)
		len += sysfs_emit_at(buf, len, "0x%03x %-22s = 0x%08x\n",
				     dpxbar_regs[i].off, dpxbar_regs[i].name,
				     readl(dpxbar->regs + dpxbar_regs[i].off));
	for (int i = 0; i < MUX_MAX; i++)
		len += sysfs_emit_at(buf, len, "sel[%s] = %d\n",
				     apple_dpxbar_names[i],
				     dpxbar->selected_dispext[i]);

	/*
	 * The DP IN blocks are claimed by this driver now, so /dev/mem cannot
	 * reach them any more - dump them here instead. macOS shows MODE going
	 * 5 -> 4 and REG_04 going 1 -> 2 once a stream is actually running.
	 */
	for (int i = MUX_DPIN0; i <= MUX_DPIN1; i++) {
		if (!dpxbar->dpin[i])
			continue;
		len += sysfs_emit_at(buf, len,
				     "%s: MODE=0x%x REG_04=0x%x ENABLE=0x%x HOLD=0x%x STATUS=0x%x\n",
				     apple_dpxbar_names[i],
				     readl(dpxbar->dpin[i] + DPIN_MODE),
				     readl(dpxbar->dpin[i] + DPIN_REG_04),
				     readl(dpxbar->dpin[i] + DPIN_ENABLE),
				     readl(dpxbar->dpin[i] + DPIN_HOLD),
				     readl(dpxbar->dpin[i] + DPIN_STATUS));
	}

	return len;
}
static DEVICE_ATTR_RO(regs);

static struct attribute *dpxbar_attrs[] = { &dev_attr_regs.attr, NULL };
ATTRIBUTE_GROUPS(dpxbar);

static int apple_dpxbar_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mux_chip *mux_chip;
	struct apple_dpxbar *dpxbar;
	const struct apple_dpxbar_hw *hw;
	int ret;

	hw = of_device_get_match_data(dev);
	mux_chip = devm_mux_chip_alloc(dev, MUX_MAX, sizeof(*dpxbar));
	if (IS_ERR(mux_chip))
		return PTR_ERR(mux_chip);

	dpxbar = mux_chip_priv(mux_chip);
	mux_chip->ops = hw->ops;
	spin_lock_init(&dpxbar->lock);

	dpxbar->dev = dev;
	dpxbar->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(dpxbar->regs))
		return PTR_ERR(dpxbar->regs);

	/* optional: the Thunderbolt DP IN adapters' SoC-side blocks */
	for (unsigned int i = MUX_DPIN0; i <= MUX_DPIN1; i++) {
		const char *name = i == MUX_DPIN0 ? "dpin0" : "dpin1";
		void __iomem *p;

		p = devm_platform_ioremap_resource_byname(pdev, name);
		if (IS_ERR(p)) {
			dpxbar->dpin[i] = NULL;
			continue;
		}
		dpxbar->dpin[i] = p;
		dev_info(dev, "%s block available\n", name);
	}

	if (!of_device_is_compatible(dev->of_node, "apple,t6020-display-crossbar")) {
		readl(dpxbar->regs + UNK_TUNABLE);
		writel(tunable_override >= 0 ? (u32)tunable_override : hw->tunable,
		       dpxbar->regs + UNK_TUNABLE);
		readl(dpxbar->regs + UNK_TUNABLE);
	}

	for (unsigned int i = 0; i < MUX_MAX; ++i) {
		mux_chip->mux[i].states = hw->n_ufp;
		mux_chip->mux[i].idle_state = MUX_IDLE_DISCONNECT;
		dpxbar->selected_dispext[i] = -1;
	}

	platform_set_drvdata(pdev, mux_chip);

	ret = devm_mux_chip_register(dev, mux_chip);
	if (ret < 0)
		return ret;

	return 0;
}

static const struct apple_dpxbar_hw apple_dpxbar_hw_t8103 = {
	.n_ufp = 2,
	.tunable = 0,
	.ops = &apple_dpxbar_ops,
};

static const struct apple_dpxbar_hw apple_dpxbar_hw_t8112 = {
	.n_ufp = 4,
	.tunable = 4278196325,
	.ops = &apple_dpxbar_ops,
};

static const struct apple_dpxbar_hw apple_dpxbar_hw_t6000 = {
	.n_ufp = 9,
	.tunable = 0,		/* macOS writes 0, not 5 - see tunable_override */
	.ops = &apple_dpxbar_ops,
};

static const struct apple_dpxbar_hw apple_dpxbar_hw_t6020 = {
	.n_ufp = 9,
	.ops = &apple_dpxbar_t602x_ops,
};

static const struct of_device_id apple_dpxbar_ids[] = {
	{
		.compatible = "apple,t8103-display-crossbar",
		.data = &apple_dpxbar_hw_t8103,
	},
	{
		.compatible = "apple,t8112-display-crossbar",
		.data = &apple_dpxbar_hw_t8112,
	},
	{
		.compatible = "apple,t6000-display-crossbar",
		.data = &apple_dpxbar_hw_t6000,
	},
	{
		.compatible = "apple,t6020-display-crossbar",
		.data = &apple_dpxbar_hw_t6020,
	},
	{}
};
MODULE_DEVICE_TABLE(of, apple_dpxbar_ids);

static struct platform_driver apple_dpxbar_driver = {
	.driver = {
		.dev_groups = dpxbar_groups,
		.name = "apple-display-crossbar",
		.of_match_table	= apple_dpxbar_ids,
	},
	.probe = apple_dpxbar_probe,
};
module_platform_driver(apple_dpxbar_driver);

MODULE_DESCRIPTION("Apple Silicon display crossbar multiplexer driver");
MODULE_AUTHOR("Sven Peter <sven@svenpeter.dev>");
MODULE_LICENSE("GPL v2");
