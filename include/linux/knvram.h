/*
 * The Linux knvram driver
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

#ifndef _KNVRAM_H_

#include <linux/ioctl.h>

#define KNVRAM_IOCTL_BASE	'K'

#define KNVRAMIOC_SYNC		_IO(KNVRAM_IOCTL_BASE, 0)
#define KNVRAMIOC_TBEGIN	_IO(KNVRAM_IOCTL_BASE, 1)
#define KNVRAMIOC_TCOMMIT	_IO(KNVRAM_IOCTL_BASE, 2)
#define KNVRAMIOC_TABORT	_IO(KNVRAM_IOCTL_BASE, 3)
#define KNVRAMIOC_SETAUTOT	_IOW(KNVRAM_IOCTL_BASE, 4, int)
#define KNVRAMIOC_GETAUTOT	_IOR(KNVRAM_IOCTL_BASE, 5, int)

#ifdef __KERNEL__

#ifdef CONFIG_KNVRAM

#include <linux/list.h>
#include <linux/of.h>

#define KNVRAM_PARTNAME_MAXLEN 31

struct knvram_device;

struct knvram_partition {
	struct list_head lh;
	char name[KNVRAM_PARTNAME_MAXLEN + 1];
	size_t size;
	struct mutex open_lock;
	int handles;
	int writer;
	void * shadow;
	struct rw_semaphore shadow_lock;
	void * transaction;
	struct mutex transaction_lock;
	size_t transaction_pagemask;
	size_t cow_bottom;
	size_t cow_top;
	struct device *parent;
	struct knvram_device *dev;
	int (*hw_read)(struct knvram_partition *, char *, size_t, loff_t);
	int (*hw_write)(struct knvram_partition *, const char *, size_t,
			loff_t);
};

struct knvram_handle {
	struct knvram_partition *p;
	int flags;
};
typedef struct knvram_handle *knvram_handle_t;

/* For knvram_handle.flags */
#define KNVRAM_WRITE		(1 << 0)  /* called from writer */
#define KNVRAM_NONBLOCK		(1 << 1)  /* don't block (ie. O_NONBLOCK) */
#define KNVRAM_USER		(1 << 2)  /* user-space buffer pointer */
#define KNVRAM_AUTOT		(1 << 3)  /* automatic transactions */
#define KNVRAM_TRANSACTION	(1 << 4)  /* transaction in progress */

/* core.c prototypes */
extern int knvram_lock(struct knvram_partition *);
extern void knvram_unlock(struct knvram_partition *);
extern int knvram_partition_add(struct knvram_partition *);
extern void knvram_partition_del(struct knvram_partition *);
extern int knvram_partition_of_get_config(
	struct knvram_partition *, struct device_node *);
void knvram_partition_init_transaction(struct knvram_partition *, int);
extern knvram_handle_t _knvram_open(struct knvram_partition *, int);
extern knvram_handle_t knvram_open(const char *, int);
extern int knvram_close(knvram_handle_t);
extern int knvram_setautot(knvram_handle_t, int);
extern ssize_t knvram_read(
	knvram_handle_t, char *, size_t, loff_t *);
extern ssize_t knvram_write(
	knvram_handle_t, const char *, size_t, loff_t *);
extern int knvram_tbegin(knvram_handle_t);
extern int knvram_tcommit(knvram_handle_t);
extern int knvram_tabort(knvram_handle_t);
extern int knvram_sync(struct knvram_partition *);
extern void knvram_sync_all(void);

/* dev.c prototypes */
extern int knvram_dev_of_get_config(
	struct knvram_partition *, struct device_node *);
int knvram_dev_alloc(struct knvram_partition *);
int knvram_dev_register(struct knvram_partition *);
void knvram_dev_unregister(struct knvram_partition *);
void knvram_dev_readonly(struct knvram_partition *, int);

/* Flags for partitiontable entries flags field */
#define KNVRAM_PT_READONLY 1<<0

#else /* CONFIG_KNVRAM */

typedef void * knvram_handle_t;
#define knvram_open(name, flags) (NULL)

#endif /* CONFIG_KNVRAM */

#endif /* __KERNEL__ */

#endif /* _KNVRAM_H_ */
