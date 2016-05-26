/* OMAP appliance watchdog.
 * Based the driver omap_wdt.c
 * Copyright (C) 2014 Prevas A/S.
 * Author: Jacob Kjærgaard <jabk@prevas.dk>
 *
 * Watchdog driver for the TI OMAP 16xx & 24xx/34xx 32KHz (non-secure) watchdog
 *
 * Author: MontaVista Software, Inc.
 *	 <gdavis@mvista.com> or <source@mvista.com>
 *
 * 2003 (c) MontaVista Software, Inc. This file is licensed under the
 * terms of the GNU General Public License version 2. This program is
 * licensed "as is" without any warranty of any kind, whether express
 * or implied.
 *
 * History:
 *
 * 20030527: George G. Davis <gdavis@mvista.com>
 *	Initially based on linux-2.4.19-rmk7-pxa1/drivers/char/sa1100_wdt.c
 *	(c) Copyright 2000 Oleg Drokin <green@crimea.edu>
 *	Based on SoftDog driver by Alan Cox <alan@lxorguk.ukuu.org.uk>
 *
 * Copyright (c) 2004 Texas Instruments.
 *	1. Modified to support OMAP1610 32-KHz watchdog timer
 *	2. Ported to 2.6 kernel
 *
 * Copyright (c) 2005 David Brownell
 *	Use the driver model and standard identifiers; handle bigger timeouts.
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/watchdog.h>
#include <linux/slab.h>
#include <linux/clk.h>
#include <linux/of_platform.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>

#include "appwd_int.h"
#include "../omap_wdt.h"

#define DRV_NAME "appwd_wdt_omap"

static struct {
	void __iomem *base;
} omap_wdt;

struct wdt_omap_data {
	int heartbeat;
	u32 timeout_ms;
};


#define OMAP_WDT_SEQ1		0x1234		/* -> service sequence 1 */


static void wdt_omap_keepalive(void __iomem *base)
{
        int pattern = OMAP_WDT_SEQ1;

	__raw_writel(pattern, (omap_wdt.base + OMAP_WATCHDOG_TGR));
	while ((__raw_readl(omap_wdt.base + OMAP_WATCHDOG_WPS)) & 0x08)
		cpu_relax();

	__raw_writel(~pattern, (omap_wdt.base + OMAP_WATCHDOG_TGR));
	while ((__raw_readl(omap_wdt.base + OMAP_WATCHDOG_WPS)) & 0x08)
		cpu_relax();
}

static struct wdt_operations wdt_omap_ops = {
	.keepalive	= wdt_omap_keepalive,
};


static const struct of_device_id wdt_omap_match[];

static inline void omap_wdt_setup(int timeout)
{
	u32 pre_margin = GET_WLDR_VAL(timeout);
	/* sequence required to disable watchdog */
	__raw_writel(0xAAAA, omap_wdt.base + OMAP_WATCHDOG_SPR);	/* TIMER_MODE */
	while (__raw_readl(omap_wdt.base + OMAP_WATCHDOG_WPS) & 0x10)
		cpu_relax();
	__raw_writel(0x5555, omap_wdt.base + OMAP_WATCHDOG_SPR);	/* TIMER_MODE */
	while (__raw_readl(omap_wdt.base + OMAP_WATCHDOG_WPS) & 0x10)
		cpu_relax();

	/* initialize prescaler */
	while (__raw_readl(omap_wdt.base + OMAP_WATCHDOG_WPS) & 0x01)
		cpu_relax();
	__raw_writel((1 << 5) | (PTV << 2), omap_wdt.base + OMAP_WATCHDOG_CNTRL);
	while (__raw_readl(omap_wdt.base + OMAP_WATCHDOG_WPS) & 0x01)
		cpu_relax();

	/* set timer - just count up at 32 KHz */
	while (__raw_readl(omap_wdt.base + OMAP_WATCHDOG_WPS) & 0x04)
		cpu_relax();
	__raw_writel(pre_margin, omap_wdt.base + OMAP_WATCHDOG_LDR);
	while (__raw_readl(omap_wdt.base + OMAP_WATCHDOG_WPS) & 0x04)
		cpu_relax();

	/* Sequence to enable the watchdog */
        __raw_writel(0xBBBB, omap_wdt.base + OMAP_WATCHDOG_SPR);
	while ((__raw_readl(omap_wdt.base + OMAP_WATCHDOG_WPS)) & 0x10)
		cpu_relax();
	__raw_writel(0x4444, omap_wdt.base + OMAP_WATCHDOG_SPR);
	while ((__raw_readl(omap_wdt.base + OMAP_WATCHDOG_WPS)) & 0x10)
		cpu_relax();
}

static int
wdt_omap_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct wdt_omap_data * data;
	const struct of_device_id *match;
	u32 val;
	int err;

	match = of_match_device(wdt_omap_match, &pdev->dev);
	if (!match)
		return -EINVAL;

	omap_wdt.base = of_iomap(np, 0);

	if (!omap_wdt.base)
		return -ENOMEM;

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data) {
		dev_err(&pdev->dev, "out of memory\n");
		err = -ENOMEM;
		goto err_unmap;
	}

	//default timeout
	data->timeout_ms = 60000;
	if (!of_property_read_u32(np, "timeout", &val)) {
		dev_info(&pdev->dev, "wdt_omap timeout=%d\n", val);
		data->timeout_ms = val;
	}

	pm_runtime_enable(&pdev->dev);
	pm_runtime_get_sync(&pdev->dev);

	omap_wdt_setup(data->timeout_ms / 1000);

	//default heartbeat
	data->heartbeat = 2000;
	if (of_property_read_u32(np, "heartbeat", &val)) {
		dev_err(&pdev->dev, "heartbeat not specified\n");
		err = -EINVAL;
		goto invalid_heartbeat;
	} else {
		dev_info(&pdev->dev, "wdt_omap heartbeat=%d\n", val);
		data->heartbeat = val * HZ / 1000;
		if (data->heartbeat < 0) {
			dev_err(&pdev->dev, "heartbeat delay must be at least 1 jiffy\n");
			err = -EINVAL;
			goto invalid_heartbeat;
		}
	}

	dev_info(&pdev->dev, "WDT driver for OMAP initialized. timeout=%d "
		"(%d ms)\n", val, data->timeout_ms);

	wdt_omap_keepalive(data);

	err = appwd_wdt_register(DRV_NAME, &wdt_omap_ops, data->heartbeat,
				data);
	if (err < 0) {
		dev_err(&pdev->dev, "failed to register wdt_omap: %d\n", err);
		goto appwd_wdt_register_failed;
	}

	return 0;

invalid_heartbeat:
appwd_wdt_register_failed:
	kfree(data);

err_unmap:
	iounmap(omap_wdt.base);
	omap_wdt.base = NULL;
	return err;
}

static const struct of_device_id wdt_omap_match[] = {
	{
		.compatible = "appwd-omap",
	},
	{},
};
MODULE_DEVICE_TABLE(of, wdt_omap_match);


static struct platform_driver wdt_omap_driver = {
	.probe		= wdt_omap_probe,
	.driver = {
		.name		= DRV_NAME,
		.owner		= THIS_MODULE,
		.of_match_table	= wdt_omap_match,
	},
};

static int __init wdt_omap_init(void)
{
	pr_debug("initializing appwd_omap driver\n");
	return platform_driver_register(&wdt_omap_driver);
}
device_initcall(wdt_omap_init);

/*
 * Local Variables:
 * compile-command: "make -C ../../.. M=drivers/watchdog/appwd wdt_omap.o"
 * End:
 */
