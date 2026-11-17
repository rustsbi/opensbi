/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2021 Samuel Holland <samuel@sholland.org>
 */

#include <libfdt.h>
#include <sbi/riscv_barrier.h>
#include <sbi/riscv_io.h>
#include <sbi/sbi_bitops.h>
#include <sbi/sbi_error.h>
#include <sbi/sbi_hart.h>
#include <sbi/sbi_system.h>
#include <sbi/sbi_timer.h>
#include <sbi_utils/fdt/fdt_driver.h>
#include <sbi_utils/fdt/fdt_helper.h>

#define WDT_KEY_VAL			0x16aa0000

#define WDT_SOFT_RST_REG		0x08
#define WDT_SOFT_RST_EN			BIT(0)

#define WDT_CFG_REG			0x14
#define WDT_CFG_SYS_RESET		BIT(0)
#define WDT_CFG_CLK_SRC			BIT(8)
#define WDT_MODE_REG			0x18
#define WDT_MODE_EN			BIT(0)

struct sunxi_wdt_reset_data {
	void (*system_reset)(u32 type, u32 reason);
};

static volatile char *sunxi_wdt_base;

static int sunxi_wdt_system_reset_check(u32 type, u32 reason)
{
	switch (type) {
	case SBI_SRST_RESET_TYPE_COLD_REBOOT:
	case SBI_SRST_RESET_TYPE_WARM_REBOOT:
		return 1;
	}

	return 0;
}

static void sunxi_wdt_timeout_reset(u32 type, u32 reason)
{
	u32 cfg;

	writel_relaxed(WDT_KEY_VAL, sunxi_wdt_base + WDT_MODE_REG);
	/* Stop counting before changing the reset output. */
	mb();

	/* Disable the reset output before reprogramming it. */
	cfg = readl_relaxed(sunxi_wdt_base + WDT_CFG_REG);
	writel_relaxed(WDT_KEY_VAL | (cfg & ~WDT_CFG_SYS_RESET),
		       sunxi_wdt_base + WDT_CFG_REG);
	/* Order the reset-output write before the timer reads. */
	mb();
	sbi_timer_mdelay(1);

	writel_relaxed(WDT_KEY_VAL | WDT_CFG_SYS_RESET | (cfg & WDT_CFG_CLK_SRC),
		       sunxi_wdt_base + WDT_CFG_REG);
	/* Order the CFG write before enabling the watchdog in MODE. */
	wmb();
	/* A zero interval field selects the shortest timeout. */
	writel_relaxed(WDT_KEY_VAL | WDT_MODE_EN,
		       sunxi_wdt_base + WDT_MODE_REG);
	sbi_hart_hang();
}

static void sunxi_wdt_soft_reset(u32 type, u32 reason)
{
	/* Disable the watchdog. */
	writel_relaxed(WDT_KEY_VAL,
		       sunxi_wdt_base + WDT_MODE_REG);

	/* Trigger soft reset. */
	writel_relaxed(WDT_KEY_VAL | WDT_SOFT_RST_EN,
		       sunxi_wdt_base + WDT_SOFT_RST_REG);
}

static struct sbi_system_reset_device sunxi_wdt_reset = {
	.name = "sunxi-wdt-reset",
	.system_reset_check = sunxi_wdt_system_reset_check,
};

static const struct sunxi_wdt_reset_data sun20i_d1_wdt_data = {
	.system_reset = sunxi_wdt_soft_reset,
};

static const struct sunxi_wdt_reset_data sun252i_f101_wdt_data = {
	.system_reset = sunxi_wdt_timeout_reset,
};

static int sunxi_wdt_reset_init(const void *fdt, int nodeoff,
				const struct fdt_match *match)
{
	const struct sunxi_wdt_reset_data *data = match->data;
	uint64_t reg_addr;
	int rc;

	rc = fdt_get_node_addr_size(fdt, nodeoff, 0, &reg_addr, NULL);
	if (rc < 0 || !reg_addr)
		return SBI_ENODEV;

	sunxi_wdt_base = (volatile char *)(unsigned long)reg_addr;
	sunxi_wdt_reset.system_reset = data->system_reset;

	sbi_system_reset_add_device(&sunxi_wdt_reset);

	return 0;
}

static const struct fdt_match sunxi_wdt_reset_match[] = {
	{ .compatible = "allwinner,sun20i-d1-wdt-reset", .data = &sun20i_d1_wdt_data },
	{ .compatible = "allwinner,sun252i-f101-wdt", .data = &sun252i_f101_wdt_data },
	{ },
};

const struct fdt_driver fdt_reset_sunxi_wdt = {
	.match_table = sunxi_wdt_reset_match,
	.init = sunxi_wdt_reset_init,
};
