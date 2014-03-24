/*
 * The Linux knvram driver - user-space device interface
 *
 * Copyright 2010 Prevas A/S.
 *
 * This file is part of the Linux knvram driver.
 *
 * The Linux knvram driver is free software: you can redistribute it
 * and/or modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * The Linux knvram driver is distributed in the hope that it will be
 * useful, but WITHOUT ANY WARRANTY; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with the Linux knvram driver.  If not, see
 * <http://www.gnu.org/licenses/>.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/bug.h>
#include <linux/slab.h>

#include <linux/err.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/uaccess.h>

#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>

#include <linux/knvram.h>
#include "knvram_int.h"

struct knvram_device {
	struct cdev cdev;
	struct device *dev;
	dev_t devnum;
	struct knvram_partition *p;
	int read_only;
};

static struct class *knvram_class = NULL;
static int major, first_minor, next_minor;

#define DEV_NAME "knvram-dev"

#define KNVRAM_MAX_PARTITIONS (1u << MINORBITS)


/*
 * File operations
 */

static int
knvram_dev_open(struct inode *inode, struct file *file)
{
	struct knvram_device *dev;
	struct knvram_handle *h;
	int flags = KNVRAM_USER;

	dev = container_of(inode->i_cdev, struct knvram_device, cdev);

	if (file->f_mode & FMODE_WRITE) {
		if (dev->read_only)
			return -EPERM;
		flags |= KNVRAM_WRITE;
	}

	if (file->f_flags & O_NONBLOCK)
		flags |= KNVRAM_NONBLOCK;

	h = _knvram_open(dev->p, flags);
	if (IS_ERR(h))
		return PTR_ERR(h);

	file->private_data = h;

	return 0;
}

static int
knvram_dev_release(struct inode *inode_p, struct file *file)
{
	struct knvram_handle *h = file->private_data;

	return knvram_close(h);
}

static long
knvram_dev_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct knvram_handle *h = file->private_data;
	void __user *argp = (void __user *)arg;
	int __user *p = argp;

	switch (cmd) {

	case KNVRAMIOC_SYNC:
		return knvram_sync(h->p);

	case KNVRAMIOC_TBEGIN:
		return knvram_tbegin(h);

	case KNVRAMIOC_TCOMMIT:
		return knvram_tcommit(h);

	case KNVRAMIOC_TABORT:
		return knvram_tabort(h);

	case KNVRAMIOC_SETAUTOT: {
		int autot, err;
		if (get_user(autot, p))
			return -EFAULT;
		err = knvram_setautot(h, autot);
		if (err)
			return err;
		// fall-through to GETAUTOT
	}

	case KNVRAMIOC_GETAUTOT: {
		int autot = ((h->flags & KNVRAM_AUTOT) == KNVRAM_AUTOT);
		return put_user(autot, p);
	}

	default:
		return -ENOTTY;
	}
}

static ssize_t
knvram_dev_read(struct file *file, char __user *buffer,
		size_t size, loff_t *loffp)
{
	struct knvram_handle *h = file->private_data;
	int ret;

	ret = knvram_read(h, buffer, size, loffp);

	return ret;
}

static ssize_t
knvram_dev_write(struct file *file, const char __user *buffer,
		 size_t size, loff_t *loffp)
{
	struct knvram_handle *h = file->private_data;
	struct knvram_device *dev = h->p->dev;
	int ret;

	/* Report EOF */
	if (*loffp == h->p->size)
		return -ENOSPC;

	ret = knvram_write(h, (const char *)buffer, size, loffp);

	if (file->f_flags & O_SYNC) {
		int err = knvram_sync(h->p);
		if (err)
			dev_warn(dev->dev, "knvram_sync failed: %d\n", err);
	}

	return ret;
}

static loff_t
knvram_dev_llseek(struct file *file, loff_t offset, int origin)
{
	struct knvram_handle *h = file->private_data;
	struct knvram_device *dev = h->p->dev;
	int err = 0;

	switch (origin) {
	case SEEK_SET:
		break;
		//dev_dbg(dev->dev, "SEEK_SET %lld\n", offset);
	case SEEK_CUR:
		offset = file->f_pos + offset;
		//dev_dbg(dev->dev, "SEEK_CUR %lld\n", offset);
		break;
	case SEEK_END:
		offset = h->p->size + offset;
		//dev_dbg(dev->dev, "SEEK_END %lld\n", offset);
		break;
	default:
		dev_err(dev->dev, "invalid origin\n");
		err = -EINVAL;
		goto out;
	}

	if (offset > h->p->size) {
		dev_dbg(dev->dev, "cannot seek beyond end-of-file\n");
		err = -EINVAL;
		goto out;
	}

	if (offset < 0) {
		dev_dbg(dev->dev, "cannot seek to ahead start-of-file\n");
		err = -EINVAL;
		goto out;
	}

	file->f_pos = offset;

out:
	return err ? err : file->f_pos;
}

static int
knvram_dev_fsync(struct file *file, loff_t start, loff_t end, int datasync)
{
	struct knvram_handle *h = file->private_data;

	return knvram_sync(h->p);

	return 0;
}

struct file_operations knvram_dev_fops =
{
	.owner		= THIS_MODULE,
	.open		= knvram_dev_open,
	.release	= knvram_dev_release,
	.unlocked_ioctl	= knvram_dev_ioctl,
	.read		= knvram_dev_read,
	.write		= knvram_dev_write,
	.llseek		= knvram_dev_llseek,
	.fsync		= knvram_dev_fsync,
	// FIXME: consider if mmap can be added in a sane way
};


/*
 * Init / exit code
 */

static int __init knvram_dev_init(void);

int
knvram_dev_alloc(struct knvram_partition *p)
{
	struct knvram_device *dev;

	dev = kzalloc(sizeof(*p->dev), GFP_KERNEL);
	if (!dev) {
		pr_err("out of memory\n");
		return -ENOMEM;
	}

	p->dev = dev;

	return 0;
}

void
knvram_dev_readonly(struct knvram_partition *p, int readonly)
{
	struct knvram_device *dev = p->dev;
	dev->read_only = !!readonly;
}

int
knvram_dev_of_get_config(struct knvram_partition *p, struct device_node *dn)
{
	int err, len;

	if (!of_device_is_compatible(dn, "knvram-dev"))
		return 0;

	err = knvram_dev_alloc(p);
	if (err)
		return err;

	if (of_get_property(dn, "read-only", &len))
		p->dev->read_only = 1;

	return 0;
}

int
knvram_dev_register(struct knvram_partition *p)
{
	struct knvram_device *dev = p->dev;
	int err;

	if (!dev)
		return 0;

	if (next_minor > (first_minor + KNVRAM_MAX_PARTITIONS)) {
		pr_err("%s: out of minor numbers!\n", __func__);
		return -EIO;
	}

	err = knvram_dev_init();
	if (err) {
		pr_err("%s: knvram_dev_init failed: %d\n", __func__, err);
		return err;
	}

	dev->devnum = MKDEV(major, next_minor);

	/* Initialize and register cdev structure */
	cdev_init(&dev->cdev, &knvram_dev_fops);
	dev->cdev.owner = THIS_MODULE;
	err = cdev_add(&dev->cdev, dev->devnum, 1);
	if (err) {
		pr_err("%s: cdev_add failed: %d\n", __func__, err);
		goto fail_cdev_add;
	}

	/* Create device and register it with sysfs (sends uevents to
	 * udev, so it will create /dev nodes) */
	dev->dev = device_create(knvram_class, p->parent, dev->devnum, p,
				 "%s", p->name);
	if (IS_ERR(dev->dev)) {
		err = PTR_ERR(dev->dev);
		pr_err("%s: device_create failed: %d\n", __func__, err);
		goto fail_device_create;
	}

	p->dev = dev;
	dev->p = p;

	next_minor++;

	return 0;

	device_destroy(knvram_class, dev->devnum);
fail_device_create:

	cdev_del(&dev->cdev);
fail_cdev_add:

	kfree(dev);
	p->dev = NULL;

	return err;
}

void
knvram_dev_unregister(struct knvram_partition *p)
{
	BUG_ON(p->dev == NULL);
	BUG_ON(knvram_class == NULL);

	device_destroy(knvram_class, p->dev->devnum);

	cdev_del(&p->dev->cdev);

	kfree(p->dev);
	p->dev = NULL;
}

static int __init
knvram_dev_init(void)
{
	int err=0;
	dev_t devnum;

	if (knvram_class != NULL)
		return 0;

	/* Allocate char device regions starting from minor 0 */
	err = alloc_chrdev_region(&devnum, 0, KNVRAM_MAX_PARTITIONS, DEV_NAME);
	if (err) {
		pr_err("%s: alloc_chrdev_region failed: %d\n", __func__, err);
		return err;
	}

	knvram_class = class_create(THIS_MODULE, DRV_NAME);
	if (IS_ERR(knvram_class)) {
		err = PTR_ERR(knvram_class);
		pr_err("%s: class_create failed: %d\n", __func__, err);
		return err;
	}

	major = MAJOR(devnum);
	next_minor = first_minor = MINOR(devnum);

	return err;
}

static void __exit
knvram_dev_exit(void)
{
	unregister_chrdev_region(MKDEV(major, first_minor),
				 KNVRAM_MAX_PARTITIONS);
}


/*
 * Local Variables:
 * compile-command: "make -C ../../.. M=drivers/char/knvram dev.o"
 * End:
 */
