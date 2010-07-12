/*
 * MPC8xxx reset events driver
 *
 * Copyright 2010 DoréDevelopment ApS.
 * Author: Esben Haabendal (eha@doredevelopment.dk.dk)
 *
 * Based on original work by DoréDevelopment ApS for Focon Electronic Systems.
 * Copyright 2009 Focon Electronic Systems A/S.
 *
 * This driver is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This driver is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this driver.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/kernel.h>
#include <linux/err.h>
#include <linux/ctype.h>
#include <linux/slab.h>
#include <linux/miscdevice.h>
#include <linux/reboot.h>
#include <linux/bitops.h>
#include <linux/proc_fs.h>

#define DRV_NAME	"mpc8xxx_rste"
#define DEBUG_LEVEL	0
#define DEBUG_PREFIX	DRV_NAME

#include <linux/knvram.h>
#include <linux/mpc8xxx_rste.h>

#ifndef CONFIG_MPC8xxx_RSTE
# error You need to enable CONFIG_MPC8xxx_RSTE
#endif

#define RSTE_KNVRAM_PARTITION DRV_NAME


/*
 * U-Boot maintains a set of reset counters. Each counter have both a
 * current and a total value.  The current value is reset on
 * coldstart, while the total value is never automatically reset. Both
 * current and total values can be manually reset via sysfs interfaces
 * clear_current_counters and clear_total_counters.
 *
 * Reset counters are maintained in NVRAM by U-boot during boot,
 * based on the value of the reset_cause bit-field (both Linux and
 * U-Boot can set bits in it) and the RSR register.
 */


struct reset_counter {
	u16 current;
	u32 total;
} __attribute__ ((packed));

typedef enum {
	RESET_COUNTER_COLDSTART = 0,
	RESET_COUNTER_BOOT_TIMEOUT,
	RESET_COUNTER_APP_TIMEOUT,
	RESET_COUNTER_REBOOT_TIMEOUT,
	RESET_COUNTER_LINUX_RESET,
	RESET_COUNTER_LINUX_PANIC,
	RESET_COUNTER_UBOOT_RESET,
	RESET_COUNTER_WDT_RESET,
	RESET_COUNTER_CHECKSTOP,
	RESET_COUNTER_BUSMONITOR,
	RESET_COUNTER_JTAG_HRST,
	RESET_COUNTER_JTAG_SRST,
	RESET_COUNTER_HW_HRST,
	RESET_COUNTER_HW_SRST,
	RESET_COUNTER_SW_HRST,
	RESET_COUNTER_SW_SRST,
	RESET_COUNTER_UNKNOWN_RESET,
	RESET_COUNTER_INVALID_CAUSE,
	__NUM_RESET_COUNTERS__,
} reset_counter_index_t;

static const char *reset_counter_name[] = {
	"coldstart",
	"boot_timeout_reset",
	"app_timeout_reset",
	"reboot_timeout",
	"linux_reset",
	"linux_panic",
	"uboot_reset",
	"wdt_reset",
	"checkstop_reset",
	"busmonitor_reset",
	"jtag_hreset",
	"jtag_sreset",
	"hw_hreset",
	"hw_sreset",
	"sw_hreset",
	"sw_sreset",
	"unknown_reset",
	"invalid_cause",
};

struct mpc8xxx_rste {
	struct reset_counter counter[__NUM_RESET_COUNTERS__];
	u16 reset_cause;
	u32 last_unknown_rsr;
	u16 last_unknown_reset_cause;
	u16 last_invalid_reset_cause;
	u32 current;
} __attribute__ ((packed));

static struct mpc8xxx_rste * mpc8xxx_rste = NULL;

static struct miscdevice miscdev;
static struct proc_dir_entry * procfile;

static knvram_handle_t
get_knvram_handle(void)
{
	static knvram_handle_t handle = NULL;

	if (handle)
		return handle;

	/* Open knvram if configured */
	handle = knvram_open(RSTE_KNVRAM_PARTITION, KNVRAM_WRITE|KNVRAM_AUTOT);
	if (IS_ERR(handle)) {
		pr_warning("%s: failed to open knvram partition %s: %ld\n",
			   __func__, RSTE_KNVRAM_PARTITION, PTR_ERR(handle));
		handle = NULL;
	}

	return handle;
}

static void
write_to_knvram(void)
{
	ssize_t len = sizeof(*mpc8xxx_rste);
	loff_t offset = 0;
	knvram_handle_t knvram = get_knvram_handle();

	if (knvram == NULL)
		return;

	len = knvram_write(knvram, (void *)mpc8xxx_rste, len, &offset);
	if (len < 0) {
		pr_warning("%s: write to knvram failed: %d\n", __func__, len);
	} else if (len != sizeof(*mpc8xxx_rste)) {
		pr_warning("%s: partial write to knvram (%d expected): %d\n",
			   __func__, sizeof(*mpc8xxx_rste), len);
		knvram_tabort(knvram);
	} else {
		knvram_tcommit(knvram);
		knvram_sync(knvram->p);
	}
}

static int
clear_counters(int total)
{
	int i;

	BUG_ON(mpc8xxx_rste == NULL);

	for (i=0 ; i < __NUM_RESET_COUNTERS__ ; i++) {
		mpc8xxx_rste->counter[i].current = 0;
		if (total)
			mpc8xxx_rste->counter[i].total = 0;
	}

	write_to_knvram();

	return 0;
}


void
mpc8xxx_rste_cause(unsigned cause)
{
	if (mpc8xxx_rste == NULL) {
		pr_warning("Unable to save reset cause: 0x%x\n", cause);
		return;
	}

	pr_debug("%s: %d\n", __func__, cause);

	if ((cause & ~RESET_CAUSE_MASK) != 0) {
		pr_warning("%s: invalid cause: 0x%x", __func__, cause);
	}

	mpc8xxx_rste->reset_cause |= cause;

	write_to_knvram();
}

void
mpc8xxx_rste_panic(char *str)
{
        mpc8xxx_rste_cause(RESET_CAUSE_LINUX_PANIC);
}

static int
mpc8xxx_rste_reboot(struct notifier_block *this,
		    unsigned long code, void *unused)
{
        if (code == SYS_RESTART)
                mpc8xxx_rste_cause(RESET_CAUSE_LINUX_RESET);
        return NOTIFY_DONE;
}

static struct notifier_block mpc8xxx_rste_reboot_notifier = {
        .notifier_call  = mpc8xxx_rste_reboot,
};

static ssize_t
scnprintf_mpc8xxx_rste_current(char * buf, size_t len)
{
	int bit, rv, total = 0;
	BUG_ON(mpc8xxx_rste == NULL);
	for_each_set_bit(bit, (unsigned long *)&mpc8xxx_rste->current,
		     __NUM_RESET_COUNTERS__) {
		const char *fmt;
		if (total == 0)
			fmt = "%s";
		else
			fmt = "%s ";
		BUG_ON(total >= (len - 2));
		rv = scnprintf(buf + total, len - total, fmt,
			       reset_counter_name[bit]);
		if (rv < 0)
			return rv;
		total += rv;
	}
	rv = scnprintf(buf + total, len - total, "\n");
	if (rv < 0)
		return rv;
	return total + rv;
}

static ssize_t show_current(struct device * dev,
			    struct device_attribute * attr, char * buf)
{
	return scnprintf_mpc8xxx_rste_current(buf, PAGE_SIZE);
}
static DEVICE_ATTR(current, S_IRUGO, show_current, NULL);

#define counter_show_func(name,index) \
static ssize_t show_##name(struct device * dev, \
			   struct device_attribute * attr, char * buf)\
{\
	BUG_ON(mpc8xxx_rste == NULL);\
	return scnprintf(buf, PAGE_SIZE, "%hu %u\n",\
		       mpc8xxx_rste->counter[index].current,\
		       mpc8xxx_rste->counter[index].total);\
}

#define COUNTER_ATTR(name,index) \
counter_show_func(name,index); \
static DEVICE_ATTR(name, S_IRUGO, show_##name, NULL);

COUNTER_ATTR(coldstart, RESET_COUNTER_COLDSTART);
COUNTER_ATTR(boot_timeout_reset, RESET_COUNTER_BOOT_TIMEOUT);
COUNTER_ATTR(app_timeout_reset, RESET_COUNTER_APP_TIMEOUT);
COUNTER_ATTR(reboot_timeout, RESET_COUNTER_REBOOT_TIMEOUT);
COUNTER_ATTR(linux_reset, RESET_COUNTER_LINUX_RESET);
COUNTER_ATTR(linux_panic, RESET_COUNTER_LINUX_PANIC);
COUNTER_ATTR(uboot_reset, RESET_COUNTER_UBOOT_RESET);
COUNTER_ATTR(wdt_reset, RESET_COUNTER_WDT_RESET);
COUNTER_ATTR(checkstop_reset, RESET_COUNTER_CHECKSTOP);
COUNTER_ATTR(busmonitor_reset, RESET_COUNTER_BUSMONITOR);
COUNTER_ATTR(jtag_hreset, RESET_COUNTER_JTAG_HRST);
COUNTER_ATTR(jtag_sreset, RESET_COUNTER_JTAG_SRST);
COUNTER_ATTR(hw_hreset, RESET_COUNTER_HW_HRST);
COUNTER_ATTR(hw_sreset, RESET_COUNTER_HW_SRST);
COUNTER_ATTR(sw_hreset, RESET_COUNTER_SW_HRST);
COUNTER_ATTR(sw_sreset, RESET_COUNTER_SW_SRST);
COUNTER_ATTR(unknown_reset, RESET_COUNTER_UNKNOWN_RESET);
COUNTER_ATTR(invalid_cause, RESET_COUNTER_INVALID_CAUSE);

static ssize_t
store_clear(struct device * dev, struct device_attribute * attr,
		     const char * buf, size_t count)
{
	int res, err=0;
	unsigned val;

	res = sscanf(buf, "%u", &val);
	if (res != 1 || val < 0 || val > 1) {
		return -EINVAL;
	}

	clear_counters(val);

	return err ? err : count;
}
static DEVICE_ATTR(clear, S_IWUSR|S_IWGRP, NULL, store_clear);

static struct attribute *sysfs_attrs[] = {
	&dev_attr_current.attr,
	&dev_attr_coldstart.attr,
	&dev_attr_boot_timeout_reset.attr,
	&dev_attr_app_timeout_reset.attr,
	&dev_attr_reboot_timeout.attr,
	&dev_attr_linux_reset.attr,
	&dev_attr_linux_panic.attr,
	&dev_attr_uboot_reset.attr,
	&dev_attr_wdt_reset.attr,
	&dev_attr_checkstop_reset.attr,
	&dev_attr_busmonitor_reset.attr,
	&dev_attr_jtag_hreset.attr,
	&dev_attr_jtag_sreset.attr,
	&dev_attr_hw_hreset.attr,
	&dev_attr_hw_sreset.attr,
	&dev_attr_sw_hreset.attr,
	&dev_attr_sw_sreset.attr,
	&dev_attr_unknown_reset.attr,
	&dev_attr_invalid_cause.attr,

	&dev_attr_clear.attr,

	NULL
};

static struct attribute_group sysfs_group = {
	.name	= NULL,
	.attrs	= sysfs_attrs,
};


static int
mpc8xxx_rste_proc_read(char *page, char **start, off_t off,
		       int count, int *eof, void *data)
{
        char *p = page;
	int i, written;

	BUG_ON(mpc8xxx_rste == NULL);

	written = scnprintf_mpc8xxx_rste_current(p, count);
	if (written < 0)
		goto out;
	count -= written;
	p += written;
	for (i=0 ; i < __NUM_RESET_COUNTERS__ && count > 0 ; i++) {
		written = scnprintf(p, count, "%-18s = %hu / %u\n",
				    reset_counter_name[i],
				    mpc8xxx_rste->counter[i].current,
				    mpc8xxx_rste->counter[i].total);
		if (written < 0)
			goto out;
		count -= written;
		p += written;
	}

out:
        return (p - page);
}


int __init
mpc8xxx_rste_init(void)
{
	int err = 0;
	knvram_handle_t knvram;
	char buf[80];

	mpc8xxx_rste = kzalloc(sizeof(*mpc8xxx_rste), GFP_KERNEL);
	if (!mpc8xxx_rste) {
		err = -ENOMEM;
		pr_err("%s: out of memory\n", __func__);
		goto fail_kmalloc;
	}

	/* Read from knvram */
	knvram = get_knvram_handle();
	if (knvram) {
		ssize_t len = sizeof(*mpc8xxx_rste);
		loff_t offset = 0;

		len = knvram_read(knvram, (void *)mpc8xxx_rste, len, &offset);

		if (len < 0) {
			err = len;
			pr_err("%s: failed to read from knvram: %d\n",
			       __func__, err);
		} else if (len != sizeof(*mpc8xxx_rste)) {
			err = -EIO;
			pr_err("%s: partial read from knvram (%d needed): %d\n",
			       __func__, sizeof(*mpc8xxx_rste), len);
			memset(&mpc8xxx_rste, 0, sizeof(*mpc8xxx_rste));
		}

		if (err) {
			int close_err;
			close_err = knvram_close(knvram);
			if (close_err)
				pr_warning("%s: failed to close knvram: %d\n",
					   __func__, close_err);
			knvram = NULL;
		}
	} else {
		pr_warning("%s: did not get a knvram handle\n", __func__);
	}

	err = scnprintf_mpc8xxx_rste_current(buf, sizeof(buf));
	if (err > 0)
		pr_info(DRV_NAME ": %s", buf);
	else
		pr_warning(DRV_NAME ": failed to log current reset event(s)");

        err = register_reboot_notifier(&mpc8xxx_rste_reboot_notifier);
        if (err) {
                pr_err("%s: reboot notifier registration failed: %d\n",
		       __func__, err);
                goto fail_register_reboot_notifier;
        }

	miscdev.name = DRV_NAME;
	miscdev.fops = NULL;
	miscdev.minor = MISC_DYNAMIC_MINOR;
	err = misc_register(&miscdev);
	if (err) {
		pr_err("%s: device registration failed: %d", __func__, err);
		goto fail_misc_register;
	}

	procfile = create_proc_read_entry(DRV_NAME, S_IRUGO, NULL,
					  mpc8xxx_rste_proc_read, NULL);
	if (IS_ERR(procfile)) {
		err = PTR_ERR(procfile);
		pr_err("%s: cailed to create proc entry: %d", __func__, err);
		goto fail_create_proc_read_entry;
	}

	err = sysfs_create_group(&miscdev.this_device->kobj, &sysfs_group);
	if (err < 0) {
		pr_err("%s: failed to create sysfs group: %d", __func__, err);
		goto fail_sysfs_create_group;
	}

	return 0;

	sysfs_remove_group(&miscdev.this_device->kobj, &sysfs_group);
fail_sysfs_create_group:

	remove_proc_entry(DRV_NAME, NULL);
fail_create_proc_read_entry:

	misc_deregister(&miscdev);
fail_misc_register:

	unregister_reboot_notifier(&mpc8xxx_rste_reboot_notifier);
fail_register_reboot_notifier:

	kfree(mpc8xxx_rste);
	mpc8xxx_rste = NULL;
fail_kmalloc:

	return err;
}
device_initcall(mpc8xxx_rste_init);


/*
 * Local Variables:
 * compile-command: "make -C ../.. M=drivers/misc mpc8xxx_rste.o"
 * End:
 */
