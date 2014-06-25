/*
 * The Linux knvram driver - NVRAM backend driver
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
#include <linux/slab.h>

#include <linux/err.h>
#include <linux/io.h>
#include <linux/crc32.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>

#include <linux/of_device.h>
#include <linux/of_platform.h>

#include <linux/knvram.h>
#include "knvram_int.h"

struct knvram_nvram_partition {
	struct list_head lh;
	struct list_head lh_pt;
	struct knvram_partition p;
	unsigned offset;
	void __iomem *addr;
};

struct nvram_partitiontable_entry {
	u32 offset;
	u32 size;
	u8 flags;
	u8 pagesize;
} __attribute__((__packed__));

typedef u16 nvram_partitiontable_num_entries_t;
typedef u32 nvram_partitiontable_checksum_t;

struct knvram_nvram_partitiontable {
	struct list_head lh;
	char name[KNVRAM_PARTNAME_MAXLEN - 3 + 1];
	unsigned offset;
	size_t size;
	struct miscdevice miscdev;
	struct list_head partitions;
	struct knvram_nvram_device *nvram;
	struct mutex lock;
	int usage;
	nvram_partitiontable_num_entries_t num_entries;
	struct nvram_partitiontable_entry *entries;
	char * read_buf;
	size_t read_buf_len;
	char * write_buf;
	size_t write_buf_len;
};

struct knvram_nvram_device {
	struct device *dev;
	struct list_head partitions;
	struct list_head partitiontables;
	unsigned long phys_addr;
	void __iomem *virt_addr;
	size_t size;
};

static inline void
_nvram_read(void __iomem *addr, void *buf, size_t size)
{
	memcpy_fromio(buf, addr, size);
}

static inline void
_nvram_write(void __iomem *addr, const void *buf, size_t size)
{
	memcpy_toio(addr, buf, size);
}

int
nvram_read(struct knvram_partition *p, char *buf, size_t size, loff_t offset)
{
	struct knvram_nvram_partition *np =
		container_of(p, struct knvram_nvram_partition, p);
	_nvram_read(np->addr + offset, buf, size);
	return 0;
}

int
nvram_write(struct knvram_partition *p, const char *buf, size_t size,
	    loff_t offset)
{
	struct knvram_nvram_partition *np =
		container_of(p, struct knvram_nvram_partition, p);
	_nvram_write(np->addr + offset, buf, size);
	return 0;
}

static struct knvram_nvram_partition *
init_nvram_partition(
	struct knvram_nvram_device *nvram, u32 offset, u32 size,
	struct device *parent)
{
	struct knvram_nvram_partition *np;

	np = kzalloc(sizeof(*np), GFP_KERNEL);
	if (!np) {
		pr_err("out of memory\n");
		return ERR_PTR(-ENOMEM);
	}

	np->p.parent = parent;

	/* Save partition offset and size */
	np->offset = offset;
	np->p.size = size;

	INIT_LIST_HEAD(&np->lh);

	return np;
}

static void
free_nvram_partition(struct knvram_nvram_partition *np)
{
	list_del(&np->lh);
	list_del(&np->lh_pt);
	kfree(np);
}

static int
_nvram_read_partitiontable(struct knvram_nvram_partitiontable *pt)
{
	void __iomem *virt_addr = pt->nvram->virt_addr + pt->offset;
	nvram_partitiontable_num_entries_t num_entries;
	void *table;
	size_t table_len;

	_nvram_read(virt_addr + pt->size
		    - (sizeof(nvram_partitiontable_num_entries_t)
		       + sizeof(nvram_partitiontable_checksum_t)),
		    &num_entries, sizeof(num_entries));

	table_len = num_entries * sizeof(struct nvram_partitiontable_entry)
		+ sizeof(nvram_partitiontable_num_entries_t)
		+ sizeof(nvram_partitiontable_checksum_t);

	if (num_entries == 0 || num_entries > 256) {
		dev_dbg(pt->miscdev.this_device, "invalid partition table\n");
		return -EIO;
	}

	table = kmalloc(table_len, GFP_KERNEL);
	if (table == NULL) {
		dev_err(pt->miscdev.this_device, "out of memory");
		return -ENOMEM;
	}

	_nvram_read(virt_addr + pt->size - table_len, table, table_len);

	if (pt->entries)
		kfree(pt->entries);
	pt->entries = table;
	pt->num_entries = num_entries;

	return 0;
}

static int
nvram_reread_partitiontable(struct knvram_nvram_partitiontable *pt)
{
	int err, i;
	//void __iomem *virt_addr = pt->nvram->virt_addr + pt->offset;
	size_t checksum_len;
	struct knvram_nvram_partition *np, *np2;
	struct list_head lh;
	int failed=0;
	int floor, roof;
	//struct nvram_partitiontable_entry *table;
	nvram_partitiontable_checksum_t *checksum;

	err = _nvram_read_partitiontable(pt);
	if (err)
		return err;

	checksum_len = (pt->num_entries
			* sizeof(struct nvram_partitiontable_entry))
		+ sizeof(nvram_partitiontable_num_entries_t);
	checksum = (((void *)pt->entries) + checksum_len);
	if (crc32(0, pt->entries, checksum_len) != *checksum) {
		dev_warn(pt->miscdev.this_device,
			 "bad partitiontable checksum");
		return -EIO;
	}

	/* Unregister current partitions */
	INIT_LIST_HEAD(&lh);
	list_for_each_entry_safe(np, np2, &pt->partitions, lh_pt) {
		err = knvram_lock(&np->p);
		if (err) {
			dev_warn(pt->miscdev.this_device,
				 "failed to reread partition table: %s busy\n",
				 np->p.name);
			list_for_each_entry_safe(np, np2, &lh, lh_pt) {
				knvram_unlock(&np->p);
				list_del_init(&np->lh_pt);
				list_add(&np->lh_pt, &pt->partitions);
			}
			return -EBUSY;
		}
		list_del_init(&np->lh_pt);
		list_add_tail(&np->lh_pt, &lh);
	}
	list_for_each_entry_safe(np, np2, &lh, lh_pt) {
		knvram_partition_del(&np->p);
		free_nvram_partition(np);
	}

	pr_info("Creating knvram partitions on %s (%s*)\n",
		pt->miscdev.this_device->of_node->full_name, pt->name);
	INIT_LIST_HEAD(&pt->partitions);
	floor = 0;
	roof = pt->size - (checksum_len
			   + sizeof(nvram_partitiontable_checksum_t));
	for (i=0 ; i < pt->num_entries ; i++) {
		if (pt->entries[i].size == 0)
			continue;
		if (pt->entries[i].offset < floor ||
		    pt->entries[i].offset >= roof) {
			dev_err(pt->miscdev.this_device,
				"invalid partition offset");
			return -EIO;
		}
		if (pt->entries[i].offset + pt->entries[i].size >= roof) {
			dev_err(pt->miscdev.this_device,
				"invalid partition size");
			return -EIO;
		}

		np = init_nvram_partition(
			pt->nvram,
			pt->offset + pt->entries[i].offset,
			pt->entries[i].size,
			pt->miscdev.this_device);
		if (IS_ERR(np)) {
			dev_err(pt->miscdev.this_device,
				"invalid partition table entry: %d,%u,%u",
				i, pt->offset + pt->entries[i].offset,
				pt->entries[i].size);
			failed = 1;
			err = PTR_ERR(np);
			continue;
		}

		/* Set partition name based on label of partitiontable dev */
		strncpy(np->p.name, pt->name, KNVRAM_PARTNAME_MAXLEN - 3);
		np->p.name[KNVRAM_PARTNAME_MAXLEN - 3] = '\0';
		snprintf(&np->p.name[strlen(np->p.name)], 3, "%d", i);
		np->p.name[KNVRAM_PARTNAME_MAXLEN] = '\0';

		if (pt->entries[i].pagesize)
			knvram_partition_init_transaction(
				&np->p, (1 << pt->entries[i].pagesize));

#ifdef CONFIG_KNVRAM_DEV
		err = knvram_dev_alloc(&np->p);
		if (err)
			return err;

		knvram_dev_readonly(
			&np->p, (pt->entries[i].flags & KNVRAM_PT_READONLY));
#endif /* CONFIG_KNVRAM_DEV */

		pr_info("0x%08x-0x%08x : \"%s\"\n",
			np->offset, np->offset + np->p.size, np->p.name);

		INIT_LIST_HEAD(&np->lh_pt);
		list_add_tail(&np->lh_pt, &pt->partitions);
	}
	if (failed)
		return err;

	list_for_each_entry(np, &pt->partitions, lh_pt) {
		np->addr = pt->nvram->virt_addr + np->offset;
		np->p.hw_read = nvram_read;
		np->p.hw_write = nvram_write;
		err = knvram_partition_add(&np->p);
		if (err)
			dev_err(pt->miscdev.this_device,
				"knvram_partition_add %s failed: %d\n",
				np->p.name, err);

		list_add_tail(&np->lh, &pt->nvram->partitions);
	}

	return 0;
}

static void
nvram_read_partitiontables(struct knvram_nvram_device *nvram)
{
	struct knvram_nvram_partitiontable *pt;

	list_for_each_entry(pt, &nvram->partitiontables, lh) {
		/* Read partition table, but not doing any further
		 * error handling */
		nvram_reread_partitiontable(pt);
	}
}

static void
nvram_cleanup(struct knvram_nvram_device *nvram)
{
	struct knvram_nvram_partition *np;

	if (nvram->virt_addr) {
		list_for_each_entry(np, &nvram->partitions, lh) {
			// FIXME: lock partition
			np->addr = NULL;
			// FIXME: unlock partition
		}

		iounmap(nvram->virt_addr);
		nvram->virt_addr = NULL;
	}

	list_for_each_entry(np, &nvram->partitions, lh) {
		kfree(np);
		// FIXME: does core.c need to fixup something here?
	}

	kfree(nvram);
}

static void
cleanup_nvram(struct knvram_nvram_device *nvram)
{
}

/*
 * Partition table device file operations
 */

static int
format_read_buf(struct knvram_nvram_partitiontable *pt)
{
	int err, i, len=0;
	struct nvram_partitiontable_entry *entry;

	if (pt->read_buf) {
		kfree(pt->read_buf);
		pt->read_buf = NULL;
	}

	if (pt->entries == NULL)
		return -EIO;

	pt->read_buf = kmalloc((32 * pt->num_entries) + 1, GFP_KERNEL);
	pt->read_buf[0] = '\0';
	for (i=0 ; i < pt->num_entries ; i++) {
		entry = &pt->entries[i];
		err = snprintf(&pt->read_buf[len], 33,
			       "%u,0x%x,0x%x,%d,0x%02x\n",
			       i, entry->offset, entry->size,
			       entry->pagesize, entry->flags);
		if (err < 0) {
			kfree(pt->read_buf);
			pt->read_buf = NULL;
			return -EINVAL;
		}
		len += err;
	}

	pt->read_buf_len = len;

	return 0;
}

static int
process_write_buf(struct knvram_nvram_partitiontable *pt)
{
	int err=0;
	unsigned i, offset, size, pagesize, flags;
	unsigned last_offset=0;
	const char *buf = pt->write_buf;
	struct nvram_partitiontable_entry *entries;
	void *table;
	u16 num_entries = 0;
	u32 checksum;
	size_t entries_len, checksum_len, table_len, table_offset;

	entries = kzalloc(256 * sizeof(*table), GFP_KERNEL);
	if (entries == NULL) {
		pr_err("out of memory\n");
		return -ENOMEM;
	}

	while (buf &&
	       sscanf(buf, "%u,0x%x,0x%x,%u,0x%x\n",
		      &i, &offset, &size, &pagesize, &flags) == 5) {

		if (i < num_entries) {
			pr_warning("partition table entries must be entered consecutively\n");
			err = -EINVAL;
		}
		if (i >= 256) {
			pr_warning("maximum partiton table entry index is 255\n");
			err = -EINVAL;
		}
		if (offset >= pt->size) {
			pr_warning("partition table entry offset must be within boundaries\n");
			err = -EINVAL;
		}
		if (offset < last_offset) {
			pr_warning("partition table entries must be consecutive\n");
			err = -EINVAL;
		}
		if (offset + size > pt->size) {
			pr_warning("partition table entry size must be within boundaries\n");
			err = -EINVAL;
		}
		if (err) {
			pr_err("%s: invalid partition table\n", __func__);
			goto fail_after_entries_alloc;
		}

		entries[i].offset = offset;
		entries[i].size = size;
		entries[i].pagesize = pagesize;
		entries[i].flags = flags;

		num_entries = i + 1;
		last_offset = offset + size;
		buf = strchr(buf, '\n') + 1;
	}

	entries_len = num_entries * sizeof(*entries);
	checksum_len = entries_len + sizeof(num_entries);
	table_len = checksum_len + sizeof(checksum);

	if (last_offset >= (pt->size - table_len)) {
		pr_warning("not enough space for partition table\n");
		err = -EINVAL;
		goto fail_after_entries_alloc;
	}

	table = kzalloc(table_len, GFP_KERNEL);
	if (table == NULL) {
		pr_err("out of memory\n");
		err = -ENOMEM;
		goto fail_after_table_alloc;
	}
	memcpy(table, entries, entries_len);
	memcpy(table + entries_len, &num_entries, sizeof(num_entries));
	checksum = crc32(0, table, checksum_len);
	memcpy(table + checksum_len, &checksum, sizeof(checksum));

	table_offset = pt->offset + pt->size - table_len;
	_nvram_write(pt->nvram->virt_addr + table_offset, table, table_len);

fail_after_table_alloc:
	kfree(table);

fail_after_entries_alloc:
	kfree(entries);

	return err;
}

static int
nvrampt_open(struct inode *inode_p, struct file *filp)
{
	int err = 0;
	struct knvram_nvram_partitiontable *pt =
		container_of(filp->private_data,
			     struct knvram_nvram_partitiontable, miscdev);

	mutex_lock(&pt->lock);

	if (pt->usage > 0) {
		dev_dbg(pt->miscdev.this_device, "usage=%d\n", pt->usage);
		err = -EBUSY;
		goto err_out;
	}

	if (filp->f_mode & FMODE_READ) {
		err = _nvram_read_partitiontable(pt);
		if (err)
			goto err_out;
		format_read_buf(pt);
	}

	if (filp->f_mode & FMODE_WRITE) {
		pt->write_buf = kzalloc(8192, GFP_KERNEL);
		if (!pt->write_buf) {
			err = -ENOMEM;
			goto err_out;
		}
		pt->write_buf_len = 0;
	}

	pt->usage++;

err_out:
	mutex_unlock(&pt->lock);
	return err;
}

static int
nvrampt_close(struct inode *inode_p, struct file *filp)
{
	int err=0;
	struct knvram_nvram_partitiontable *pt =
		container_of(filp->private_data,
			     struct knvram_nvram_partitiontable, miscdev);

	mutex_lock(&pt->lock);

	if (filp->f_mode & FMODE_WRITE && pt->write_buf_len) {
		err = process_write_buf(pt);
		if (err == 0)
			err = nvram_reread_partitiontable(pt);
	}

	if (pt->write_buf) {
		kfree(pt->write_buf);
		pt->write_buf = NULL;
	}

	if (pt->read_buf) {
		kfree(pt->read_buf);
		pt->read_buf = NULL;
	}

	--pt->usage;

	mutex_unlock(&pt->lock);
	return err;
}

static ssize_t
nvrampt_read(struct file *filp,
	     char __user *buf, size_t count, loff_t *offset)
{
	ssize_t err=0;
	struct knvram_nvram_partitiontable *pt =
		container_of(filp->private_data,
			     struct knvram_nvram_partitiontable, miscdev);

	if (pt->read_buf == 0)
		return 0;

	if (*offset >= pt->read_buf_len)
		return 0;

	if ((*offset + count) > pt->read_buf_len)
		count = pt->read_buf_len - *offset;

	err = copy_to_user(buf, &pt->read_buf[*offset], count);
	if (err)
		return err;

	*offset += count;
	return count;
}

static ssize_t
nvrampt_write(struct file *filp,
	      const char __user *buf, size_t count, loff_t *offset)
{
	ssize_t err;
	struct knvram_nvram_partitiontable *pt =
		container_of(filp->private_data,
			     struct knvram_nvram_partitiontable, miscdev);

	if (*offset >= 8192)
		return 0;

	if ((*offset + count) > 8192)
		count = 8192 - *offset;

	err = copy_from_user(&pt->write_buf[*offset], buf, count);
	pt->write_buf_len = *offset + count;
	*offset += count;

	return err ? err : count;
}

/*
static long
nvrampt_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	long err=0;
	struct knvram_nvram_partitiontable *pt =
		container_of(filp->private_data,
			     struct knvram_nvram_partitiontable, miscdev);

	mutex_lock(&pt->lock);

	switch (cmd) {

	default:
		dev_dbg(pt->miscdev.this_device, "unsupported ioctl: %d", cmd);
		err = -EINVAL;
	}

	mutex_unlock(&pt->lock);
	return err;
}
*/

static struct file_operations nvrampt_fops = {
	.owner		= THIS_MODULE,
	.open		= nvrampt_open,
	.release	= nvrampt_close,
	.read		= nvrampt_read,
	.write		= nvrampt_write,
//	.unlocked_ioctl	= nvrampt_ioctl,
	.llseek		= no_llseek,
};

/*
 * OpenFirmware specific initialization
 */

static struct knvram_nvram_partitiontable *
init_nvram_partitiontable(
	struct device_node *dn,
	struct knvram_nvram_device *nvram, u32 offset, u32 size)
{
	int err;
	struct knvram_nvram_partitiontable *pt;
	const char *name;
	size_t len;

	/* Must at least have space for a single partition description */
	if (size < 16 || offset > nvram->size || offset + size > nvram->size) {
		pr_err("invalid partitiontable device boundary");
		return ERR_PTR(-EINVAL);
	}

	pt = kzalloc(sizeof(*pt), GFP_KERNEL);
	if (!pt) {
		pr_err("out of memory\n");
		return ERR_PTR(-ENOMEM);
	}

	/* Get device name from label property or node name */
	name = of_get_property(dn, "label", &len);
	if (!name)
		name = of_get_property(dn, "name", &len);
	if (len > KNVRAM_PARTNAME_MAXLEN - 3) {
		len = KNVRAM_PARTNAME_MAXLEN - 3;
		pt->name[len] = '\0';
	}
	strncpy(pt->name, name, len);

	mutex_init(&pt->lock);

	pt->miscdev.minor = MISC_DYNAMIC_MINOR;
	pt->miscdev.name = pt->name;
	pt->miscdev.fops = &nvrampt_fops;
	err = misc_register(&pt->miscdev);
	if (err) {
		pr_err("misc_register failed for %s\n", pt->name);
		goto fail_misc_register;
	}
	pt->miscdev.this_device->of_node = dn;

	pt->nvram = nvram;

	/* Save offset and size */
	pt->offset = offset;
	pt->size = size;
	INIT_LIST_HEAD(&pt->partitions);

	INIT_LIST_HEAD(&pt->lh);
	list_add_tail(&pt->lh, &nvram->partitiontables);

	return pt;

	misc_deregister(&pt->miscdev);
fail_misc_register:

	kfree(pt);
	return ERR_PTR(err);
}

static struct knvram_nvram_device *
nvram_of_get_config(struct device *dev)
{
	struct device_node *dn = dev->of_node, *child;
	struct resource res;
	const u32 *reg;
	int len;
	int err;
	struct knvram_nvram_device *nvram;
	struct knvram_nvram_partition *np;
	struct knvram_nvram_partitiontable *pt;

	/* Get 'reg' values for 'nvram' */
	if (of_address_to_resource(dn, 0, &res)) {
		dev_err(dev, "can't get IO address from device tree\n");
		return ERR_PTR(-ENXIO);;
	}

	if (!request_mem_region(res.start, res.end - res.start + 1, DRV_NAME)) {
		dev_err(dev, "request_mem_region failed");
		return ERR_PTR(-EBUSY);
	}

	nvram = kzalloc(sizeof(*nvram), GFP_KERNEL);
	if (!nvram) {
		dev_err(dev, "out of memory\n");
		return ERR_PTR(-ENOMEM);
	}

	nvram->dev = dev;

	/* Set NVRAM device start address and size */
	nvram->phys_addr = res.start;
	nvram->size = res.end - res.start + 1;

	INIT_LIST_HEAD(&nvram->partitions);
	INIT_LIST_HEAD(&nvram->partitiontables);

	/* Traverse through defined partitions */
	pr_info("Creating knvram partitions on %s\n", dn->full_name);
	child = NULL;
	while ((child = of_get_next_child(dn, child))) {

		/* Get 'reg' values */
		reg = of_get_property(child, "reg", &len);
		if (!reg || (len != 2 * sizeof(u32))) {
			of_node_put(child);
			dev_warn(dev, "invalid reg property for %s",
				 dn->full_name);
			continue;
		}

		/* Initialize partitions with "extended partition",
		 * ie. embedded partitions defined by an in-partition
		 * partitiontable.  The partition is not read yet (as
		 * io memory is not mapped yet). */
		if (of_device_is_compatible(child, "knvram-devs")) {
			pt = init_nvram_partitiontable(
				child, nvram, reg[0], reg[1]);
			if (IS_ERR(pt)) {
				of_node_put(child);
				err = PTR_ERR(pt);
				goto err_out;
			}
			pr_info("0x%08x-0x%08x : \"%s*\"\n",
				reg[0], reg[0] + reg[1], pt->name);
			continue;
		}


		np = init_nvram_partition(nvram, reg[0], reg[1], nvram->dev);
		if (IS_ERR(np)) {
			of_node_put(child);
			err = PTR_ERR(np);
			kfree(np);
			goto err_out;
		}

		err = knvram_partition_of_get_config(&np->p, child);
		if (err) {
			pr_warning("knvram_partition_of_get_config failed: %d\n", err);
			of_node_put(child);
			goto err_out;
		}

		pr_info("0x%08x-0x%08x : \"%s\"\n", reg[0], reg[0] + reg[1], np->p.name);

		list_add_tail(&np->lh, &nvram->partitions);

		/* Do _not_ call of_node_put, as it is called by
		 * of_get_next_child() */
	}

	return nvram;

err_out:
	nvram_cleanup(nvram);

	return ERR_PTR(err);
}

static int
knvram_nvram_of_probe(struct platform_device *pdev) {
	int err;
	struct knvram_nvram_device *nvram;
	struct knvram_nvram_partition *np;

	/* Get configuration from device tree */
	nvram = nvram_of_get_config(&pdev->dev);
	if (IS_ERR(nvram)) {
		err = PTR_ERR(nvram);
		goto fail_of_get_config;
	}
	/* after this, nvram struct is allocated, and
	 * nvram->partitions contains the fdt configured partitions,
	 * and nvram->partitiontables contains the fdt configured
	 * extended partitions */

	if (nvram->size < 0) {
		dev_err(nvram->dev,
			"start and/or end address is invalid\n");
		return -EINVAL;
	}

	/* Map all of NVRAM */
	nvram->virt_addr = ioremap(nvram->phys_addr, nvram->size);
	if (!nvram->virt_addr) {
		dev_err(nvram->dev, "ioremap failed\n");
		err = -EFAULT;
		goto fail_ioremap;
	}

	/* Initialize partitions */
	list_for_each_entry(np, &nvram->partitions, lh) {
		np->addr = nvram->virt_addr + np->offset;
		np->p.hw_read = nvram_read;
		np->p.hw_write = nvram_write;
		err = knvram_partition_add(&np->p);
		if (err)
			pr_warning("knvram_partition_add %s failed: %d\n",
				   np->p.name, err);
	}

	/* Stick nvram pointer in device struct */
	dev_set_drvdata(nvram->dev, nvram);

	/* Read partitiontables, initializing any partitions defined there */
	nvram_read_partitiontables(nvram);

	return 0;

fail_ioremap:
	nvram_cleanup(nvram);
fail_of_get_config:

	return err;
}

static int
knvram_nvram_of_remove(struct platform_device *pdev)
{
	cleanup_nvram(dev_get_drvdata(&pdev->dev));
	return 0;
}

static struct of_device_id knvram_nvram_of_match[] = {
	{ .compatible = "knvram-nvram", },
	{}
};
MODULE_DEVICE_TABLE(of, knvram_nvram_of_match);

static struct platform_driver knvram_nvram_of_driver =
{
	.probe		= knvram_nvram_of_probe,
	.remove		= knvram_nvram_of_remove,
	.driver = {
		.owner		= THIS_MODULE,
		.name		= "knvram_nvram",
		.of_match_table	= knvram_nvram_of_match,
	},
};

static int __init
knvram_nvram_init(void)
{
	int err;
	pr_debug("knvram: nvram driver\n");
	err = platform_driver_register(&knvram_nvram_of_driver);
	if (err < 0)
		pr_err("platform_driver_register failed: %d\n", err);
	return err;
}
module_init(knvram_nvram_init);

static void __exit
knvram_nvram_exit(void)
{
	platform_driver_unregister(&knvram_nvram_of_driver);
}
module_exit(knvram_nvram_exit);
