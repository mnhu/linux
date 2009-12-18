/*
 * Copyright (C) 2006-2008 Artem Bityutskiy
 * Copyright (C) 2006-2008 Jarkko Lavinen
 * Copyright (C) 2006-2008 Adrian Hunter
 * Copyright (C) 2009      Morten Svendsen
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; see the file COPYING. If not, write to the Free Software
 * Foundation, 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * Authors: Artem Bityutskiy, Jarkko Lavinen, Adria Hunter
 *
 * WARNING: this test program may kill your flash and your device. Do not
 * use it unless you know what you do. Authors are not responsible for any
 * damage caused by this program.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/err.h>
#include <linux/mtd/mtd.h>
#include <linux/sched.h>
#include <linux/delay.h>
#include <linux/slab.h>

#define PRINT_PREF KERN_INFO "mtd_pattern: "
#define RETRIES 3

static int eb = 0;
module_param(eb, int, S_IRUGO);
MODULE_PARM_DESC(eb, "eraseblock number within the selected MTD device");

static int dev;
module_param(dev, int, S_IRUGO);
MODULE_PARM_DESC(dev, "MTD device number to use");

static int nerase;
module_param(nerase, int, S_IRUGO);
MODULE_PARM_DESC(nerase, "MTD device number to use");

static int nwrite;
module_param(nwrite, int, S_IRUGO);
MODULE_PARM_DESC(nwrite, "MTD device number to use");

static int nread = 1;
module_param(nread, int, S_IRUGO);
MODULE_PARM_DESC(nread, "MTD device number to use");

static int check = 1;
module_param(check, int, S_IRUGO);
MODULE_PARM_DESC(check, "if the read data should be checked against the pattern");

static unsigned int cycles_count = 0;
module_param(cycles_count, uint, S_IRUGO);
MODULE_PARM_DESC(cycles_count, "how many read cycles to do "
			       "(0 by default, 0 => infinite)");

static unsigned long cycle_period;
module_param(cycle_period, ulong, S_IRUGO);
MODULE_PARM_DESC(cycle_period, "Read cycle period in milliseconds"
			       "(0 by default)");

static char* cpatt = "fec8;0000;8cef";
MODULE_PARM_DESC(cpatt, "Colon separated pattern to write (\"fec8;0000;8cef\" is default");
module_param(cpatt, charp, 0644);


static struct mtd_info *mtd;

/* This buffer contains  bytes */
static unsigned char *patt;
/* This a temporary buffer is use when checking data */
static unsigned char *check_buf;

static unsigned long cycle_start, cycle_finish;

/*
 * Erase eraseblock number @ebnum.
 */
static inline int erase_eraseblock(int ebnum)
{
	int err;
	struct erase_info ei;
	loff_t addr = ebnum * mtd->erasesize;

	memset(&ei, 0, sizeof(struct erase_info));
	ei.mtd  = mtd;
	ei.addr = addr;
	ei.len  = mtd->erasesize;

	err = mtd_erase(mtd, &ei);
	if (err) {
		printk(PRINT_PREF "error %d while erasing EB %d\n", err, ebnum);
		return err;
	}

	if (ei.state == MTD_ERASE_FAILED) {
		printk(PRINT_PREF "some erase error occurred at EB %d\n",
		       ebnum);
		return -EIO;
	}

	return 0;
}

/*
 * Check that the contents of eraseblock number @enbum is equivalent to the
 * @buf buffer.
 */
static inline int check_eraseblock(int ebnum, unsigned char *buf)
{
	int err;
	size_t read = 0;
	loff_t addr = ebnum * mtd->erasesize;
	size_t len = mtd->erasesize;

	err = mtd_read(mtd, addr, len, &read, check_buf);

	if (!check)
		return 0;

	if (err == -EUCLEAN)
		printk(PRINT_PREF "single bit flip occurred at EB %d "
		       "MTD reported that it was fixed.\n", ebnum);
	else if (err) {
		printk(PRINT_PREF "error %d while reading EB %d, "
		       "read %zd\n", err, ebnum, read);
		return err;
	}

	if (read != len) {
		printk(PRINT_PREF "failed to read %zd bytes from EB %d, "
		       "read only %zd, but no error reported\n",
		       len, ebnum, read);
		return -EIO;
	}

	if (memcmp(buf, check_buf, len)) {
		printk(PRINT_PREF "read wrong data from EB %d\n", ebnum);
	}

	return 0;
}

static inline int write_pattern(int ebnum, void *buf)
{
	int err;
	size_t written = 0;
	loff_t addr = ebnum * mtd->erasesize;
	size_t len = mtd->erasesize;

	err = mtd_write(mtd, addr, len, &written, buf);
	if (err) {
		printk(PRINT_PREF "error %d while writing EB %d, written %zd"
		      " bytes\n", err, ebnum, written);
		return err;
	}
	if (written != len) {
		printk(PRINT_PREF "written only %zd bytes of %zd, but no error"
		       " reported\n", written, len);
		return -EIO;
	}

	return 0;
}

static void set_patt(unsigned char *p, uint32_t size)
{
	unsigned char *tmp;
	unsigned c,i = 0;
	uint16_t opatt[strlen(cpatt)/sizeof(uint16_t)];

	tmp = cpatt;
	while (*(tmp++)) {
		opatt[i++] = (uint16_t)simple_strtoull(tmp,(char **)&tmp,16);
	}

	for (c=0; (c+i*sizeof(uint16_t))<=size; c=c+i*sizeof(uint16_t)) {
		memcpy(&p[c],opatt,i*sizeof(uint16_t));
	}
}

static int __init tort_init(void)
{
	int err = 0, infinite = !cycles_count;

	printk(KERN_INFO "\n");
	printk(KERN_INFO "=================================================\n");
	printk(PRINT_PREF "MTD device: %d\n", dev);
	printk(PRINT_PREF "Working on %d eraseblock of mtd%d\n", eb, dev);

	mtd = get_mtd_device(NULL, dev);
	if (IS_ERR(mtd)) {
		err = PTR_ERR(mtd);
		printk(PRINT_PREF "error: cannot get MTD device\n");
		return err;
	}

	err = -ENOMEM;
	patt = kmalloc(mtd->erasesize, GFP_KERNEL);
	if (!patt) {
		printk(PRINT_PREF "error: cannot allocate memory\n");
		goto out_mtd;
	}

	check_buf = kmalloc(mtd->erasesize, GFP_KERNEL);
	if (!check_buf) {
		printk(PRINT_PREF "error: cannot allocate memory\n");
		goto out_patt;
	}

	err = 0;

	/* Initialize patterns */
	set_patt(patt,mtd->erasesize);

	/*
	 * Check if bad eraseblock.
	 */
	if (nerase) {
		err = mtd_block_isbad(mtd, (loff_t)eb * mtd->erasesize);

		if (err < 0) {
			printk(PRINT_PREF "block_isbad() returned %d "
				"for EB %d\n", err, eb);
			goto out;
		}

		if (err) {
			printk("EB %d is bad.\n", eb);
			goto out;
		}
	}

	if (cycle_period) {
		cycle_period = msecs_to_jiffies(cycle_period);
		cycle_start = jiffies;
	}

	if (nerase) {
		printk(PRINT_PREF "Erasing block %d\n",eb);
		err = erase_eraseblock(eb);
		if (err)
			goto out;
		cond_resched();
	}

	if (nwrite) {
		printk(PRINT_PREF "Writing to block %d\n",eb);
		err = write_pattern(eb, patt);
		if (err)
			goto out;
		cond_resched();
	}

	if (!nread)
		goto out;

	printk(PRINT_PREF "Reading %d times from block %d withe delay %u ms\n",
		cycles_count,eb,jiffies_to_msecs(cycle_period));
	while (1) {
		/* Read and verify what we wrote */
		err = check_eraseblock(eb, patt);
		if (err) {
			printk(PRINT_PREF "verify failed for %s"
				" pattern\n","0x55AA55...");
			goto out;
		}
		cond_resched();

		if (!infinite && --cycles_count == 0)
			break;

		/* Delay cycle to match cycle_delay*/
		if (cycle_period) {
			long j;
			cycle_finish = jiffies;
			j = cycle_finish - cycle_start;
			if (j < cycle_period) {
				schedule_timeout_interruptible(cycle_period-j);
				cycle_start = cycle_finish + cycle_period-j;
			} else {
				cycle_period = 0;
			}
		}
	}
out:

	printk(PRINT_PREF "finished\n");
	kfree(check_buf);
out_patt:
	kfree(patt);
out_mtd:
	put_mtd_device(mtd);
	if (err)
		printk(PRINT_PREF "error %d occurred\n", err);
	printk(KERN_INFO "=================================================\n");
	return err;
}
module_init(tort_init);

static void __exit tort_exit(void)
{
	return;
}
module_exit(tort_exit);

MODULE_DESCRIPTION("Pattern reading/writing module");
MODULE_AUTHOR("Artem Bityutskiy, Jarkko Lavinen, Adrian Hunter, Morten Svendsen");
MODULE_LICENSE("GPL");
