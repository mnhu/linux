/* IMX2 appliance watchdog.
 *  drivers/watchdog/appwd/wdt_imx2.c
 *
 * Copyright (C) 2014 Prevas A/S.
 * this driver is partly based on drivers/watchdog/imx2_wdt.c
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.

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
#include <linux/reboot.h>
#include <linux/delay.h>

#include "appwd_int.h"

#define DRV_NAME "appwd_wdt_imx2"


#define IMX2_WDT_WCR		0x00		/* Control Register */
#define IMX2_WDT_WCR_WT		(0xFF << 8)	/* -> Watchdog Timeout Field */
#define IMX2_WDT_WCR_WRE	(1 << 3)	/* -> WDOG Reset Enable */
#define IMX2_WDT_WCR_WDE	(1 << 2)	/* -> Watchdog Enable */
#define IMX2_WDT_WCR_WDZST	(1 << 0)	/* -> Watchdog timer Suspend */

#define IMX2_WDT_WSR		0x02		/* Service Register */
#define IMX2_WDT_SEQ1		0x5555		/* -> service sequence 1 */
#define IMX2_WDT_SEQ2		0xAAAA		/* -> service sequence 2 */

#define IMX2_WDT_WRSR		0x04		/* Reset Status Register */
#define IMX2_WDT_WRSR_TOUT	(1 << 1)	/* -> Reset due to Timeout */

#define WDOG_SEC_TO_COUNT(s)	((s * 2 - 1) << 8)

static struct imx2_wdt {
	struct clk *clk;
	void __iomem *base;
	struct notifier_block restart_handler;
} imx2_wdt;

struct wdt_imx2_data {
	int heartbeat;
	u32 timeout_ms;
};

static int imx2_restart_handler(struct notifier_block *this, unsigned long mode,
				void *cmd)
{
	unsigned int wcr_enable = IMX2_WDT_WCR_WDE;
	struct imx2_wdt *wdev = container_of(this, struct imx2_wdt, restart_handler);
	/* Assert SRS signal */
	__raw_writew(wcr_enable, wdev->base + IMX2_WDT_WCR);

	/*
	 * Due to imx6q errata ERR004346 (WDOG: WDOG SRS bit requires to be
	 * written twice), we add another two writes to ensure there must be at
	 * least two writes happen in the same one 32kHz clock period.  We save
	 * the target check here, since the writes shouldn't be a huge burden
	 * for other platforms.
	 */
	__raw_writew(wcr_enable, wdev->base + IMX2_WDT_WCR);
	__raw_writew(wcr_enable, wdev->base + IMX2_WDT_WCR);

	/* wait for reset to assert... */
	mdelay(500);

	return NOTIFY_DONE;
}


static void wdt_imx2_keepalive(void *base)
{
	__raw_writew(IMX2_WDT_SEQ1, imx2_wdt.base + IMX2_WDT_WSR);
	__raw_writew(IMX2_WDT_SEQ2, imx2_wdt.base + IMX2_WDT_WSR);
}

static struct wdt_operations wdt_imx2_ops = {
	.keepalive	= wdt_imx2_keepalive,
};


static const struct of_device_id wdt_imx2_match[];

static inline void imx2_wdt_setup(int timeout)
{
	u16 val = __raw_readw(imx2_wdt.base + IMX2_WDT_WCR);

	/* Suspend timer in low power mode, write once-only */
	val |= IMX2_WDT_WCR_WDZST;
	/* Strip the old watchdog Time-Out value */
	val &= ~IMX2_WDT_WCR_WT;
	/* Generate reset if WDOG times out */
	val &= ~IMX2_WDT_WCR_WRE;
	/* Keep Watchdog Disabled */
	val &= ~IMX2_WDT_WCR_WDE;
	/* Set the watchdog's Time-Out value */
	val |= WDOG_SEC_TO_COUNT(timeout);

	__raw_writew(val, imx2_wdt.base + IMX2_WDT_WCR);

	/* enable the watchdog */
	val |= IMX2_WDT_WCR_WDE;
	__raw_writew(val, imx2_wdt.base + IMX2_WDT_WCR);
}

static int
wdt_imx2_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct wdt_imx2_data * data;
	const struct of_device_id *match;
	u32 val;
	int err;

	match = of_match_device(wdt_imx2_match, &pdev->dev);
	if (!match)
		return -EINVAL;

	imx2_wdt.base = of_iomap(np, 0);

	if (!imx2_wdt.base)
		return -ENOMEM;

	imx2_wdt.clk = clk_get(&pdev->dev, NULL);
	if (IS_ERR(imx2_wdt.clk)) {
		dev_err(&pdev->dev, "can't get Watchdog clock\n");
		return PTR_ERR(imx2_wdt.clk);
	}

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data) {
		dev_err(&pdev->dev, "out of memory\n");
		err = -ENOMEM;
		goto err_unmap;
	}

	//default timeout
	data->timeout_ms = 60000;
	if (!of_property_read_u32(np, "timeout", &val)) {
		dev_info(&pdev->dev, "wdt_imx2 timeout=%d\n", val);
		data->timeout_ms = val;
	}

	clk_prepare_enable(imx2_wdt.clk);
	imx2_wdt_setup(data->timeout_ms / 1000);

	//default heartbeat
	data->heartbeat = 2000;
	if (of_property_read_u32(np, "heartbeat", &val)) {
		dev_err(&pdev->dev, "heartbeat not specified\n");
		err = -EINVAL;
		goto invalid_heartbeat;
	} else {
		dev_info(&pdev->dev, "wdt_imx2 heartbeat=%d\n", val);
		data->heartbeat = val * HZ / 1000;
		if (data->heartbeat < 0) {
			dev_err(&pdev->dev, "heartbeat delay must be at least 1 jiffy\n");
			err = -EINVAL;
			goto invalid_heartbeat;
		}
	}

	dev_info(&pdev->dev, "WDT driver for IMX2 initialized. timeout=%d "
		"(%d ms)\n", val, data->timeout_ms);

	wdt_imx2_keepalive(data);

	err = appwd_wdt_register(DRV_NAME, &wdt_imx2_ops, data->heartbeat,
				data);
	if (err < 0) {
		dev_err(&pdev->dev, "failed to register wdt_imx2: %d\n", err);
		goto appwd_wdt_register_failed;
	}

	imx2_wdt.restart_handler.notifier_call = imx2_restart_handler;
	imx2_wdt.restart_handler.priority = 128;
	err = register_restart_handler(&imx2_wdt.restart_handler);
	if (err)
		dev_err(&pdev->dev, "cannot register restart handler\n");

	return 0;

invalid_heartbeat:
appwd_wdt_register_failed:
	kfree(data);

err_unmap:
	iounmap(imx2_wdt.base);
	imx2_wdt.base = NULL;
	return err;
}

static const struct of_device_id wdt_imx2_match[] = {
	{
		.compatible = "appwd-imx2",
	},
	{},
};
MODULE_DEVICE_TABLE(of, wdt_imx2_match);


static struct platform_driver wdt_imx2_driver = {
	.probe		= wdt_imx2_probe,
	.driver = {
		.name		= DRV_NAME,
		.owner		= THIS_MODULE,
		.of_match_table	= wdt_imx2_match,
	},
};

static int __init wdt_imx2_init(void)
{
	pr_debug("initializing appwd_imx2 driver\n");
	return platform_driver_register(&wdt_imx2_driver);
}
device_initcall(wdt_imx2_init);

/*
 * Local Variables:
 * compile-command: "make -C ../../.. M=drivers/watchdog/appwd wdt_imx2.o"
 * End:
 */
