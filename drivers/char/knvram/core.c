/*
 * The Linux knvram driver - core functionality
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
#include <linux/err.h>
#include <linux/slab.h>

#include <linux/uaccess.h>

#include <linux/knvram.h>
#include "knvram_int.h"

static LIST_HEAD(knvram_partitions);

/* Helper function to check if a number is a pure 2^n number.
 * Returns 1 if x is a pure 2^n number, 0 otherwise. */
static inline int
is_power_of_two(int x)
{
	int y=(~x+1);
	if (x == (y - (x ^ y)))
		return 1;
	return 0;
}

#define KNVRAM_TRANSACTION_DISABLE NULL
#define KNVRAM_TRANSACTION_ENABLE ((void *)1)
#define KNVRAM_HARDCODED_DEFAULT_PAGESIZE 128

void
knvram_partition_init_transaction(struct knvram_partition *p, int pagesize)
{
	p->transaction = KNVRAM_TRANSACTION_ENABLE;
	if (!is_power_of_two(pagesize)) {
		pr_warning("invalid transaction pagesize (%u) for %s\n",
			   pagesize, p->name);
		pagesize = KNVRAM_HARDCODED_DEFAULT_PAGESIZE;
	}
	p->transaction_pagemask = pagesize - 1;
}

int
knvram_partition_of_get_config(struct knvram_partition *p,
			       struct device_node *dn)
{
	const char *name;
	const u32 *val;
	int err=0, len;

	/* Get partition name from label property or node name */
	name = of_get_property(dn, "label", &len);
	if (!name)
		name = of_get_property(dn, "name", &len);
	if (len > KNVRAM_PARTNAME_MAXLEN) {
		len = KNVRAM_PARTNAME_MAXLEN;
		p->name[len] = '\0';
	}
	strncpy(p->name, name, len);

	val = of_get_property(dn, "transaction", &len);
	if (val) {
		int pagesize;
		if (len == sizeof(*val))
			pagesize = *val;
		else
			pagesize = CONFIG_KNVRAM_DEFAULT_PAGESIZE;
		knvram_partition_init_transaction(p, pagesize);
	}

#ifdef CONFIG_KNVRAM_DEV
	err = knvram_dev_of_get_config(p, dn);
	if (err) {
		pr_warning("knvram_dev_of_get_config failed for %s: %d\n",
			   dn->full_name, err);
		return err;
	}
#endif /* CONFIG_KNVRAM_DEV */

	return err;
}

int
knvram_partition_add(struct knvram_partition *p)
{
	int err=0;

	INIT_LIST_HEAD(&p->lh);
	list_add_tail(&p->lh, &knvram_partitions);

	p->shadow = kmalloc(p->size, GFP_KERNEL);
	if (!p->shadow) {
		pr_err("%s: out of memory\n", __func__);
		err = -ENOMEM;
		goto fail_shadow_alloc;
	}

	if (p->transaction == KNVRAM_TRANSACTION_ENABLE) {
		p->transaction = kmalloc(p->size, GFP_KERNEL);
		if (!p->transaction) {
			pr_err("%s: out of memory\n", __func__);
			err = -ENOMEM;
			goto fail_cow_alloc;
		}
	}

	err = p->hw_read(p, p->shadow, p->size, 0);
	if (err) {
		pr_err("%s: read to shadow failed: %d\n", __func__, err);
		goto fail_hw_read;
	}

#ifdef CONFIG_KNVRAM_DEV
	if (p->dev) {
		err = knvram_dev_register(p);
		if (err) {
			pr_err("%s: knvram_dev_register %s failed: %d\n",
			       __func__, p->name, err);
			goto fail_dev_register;
		}
	}
#endif /* CONFIG_KNVRAM_DEV */

	mutex_init(&p->open_lock);
	init_rwsem(&p->shadow_lock);
	if (p->transaction)
		mutex_init(&p->transaction_lock);

	return 0;

#ifdef CONFIG_KNVRAM_DEV
	knvram_dev_unregister(p);
fail_dev_register:
#endif /* CONFIG_KNVRAM_DEV */

fail_hw_read:

	if (p->transaction) {
		kfree(p->transaction);
		p->transaction = KNVRAM_TRANSACTION_ENABLE;
	}
fail_cow_alloc:

	kfree(p->shadow);
	p->shadow = NULL;
fail_shadow_alloc:

	return err;
}

int
knvram_lock(struct knvram_partition *p)
{
	if (!mutex_trylock(&p->open_lock))
		return -EAGAIN;

	if (p->handles) {
		mutex_unlock(&p->open_lock);
		return -EBUSY;
	}

	return 0;
}

void
knvram_unlock(struct knvram_partition *p)
{
	mutex_unlock(&p->open_lock);
}

void
knvram_partition_del(struct knvram_partition *p)
{
#ifdef CONFIG_KNVRAM_DEV
	knvram_dev_unregister(p);
#endif /* CONFIG_KNVRAM_DEV */

	if (p->transaction) {
		kfree(p->transaction);
		p->transaction = NULL;
		mutex_destroy(&p->transaction_lock);
	}

	kfree(p->shadow);
	p->shadow = NULL;
	list_del(&p->lh);
	mutex_destroy(&p->open_lock);
}

struct knvram_handle *
_knvram_open(struct knvram_partition *p, int flags)
{
	int err = 0;
	struct knvram_handle * h = NULL;

	if (KNVRAM_NONBLOCK & flags) {
		if (!mutex_trylock(&p->open_lock))
			return ERR_PTR(-EAGAIN);
	} else
		mutex_lock(&p->open_lock);

	if ((KNVRAM_WRITE & flags) && p->writer) {
		err = -EBUSY;
		goto out;
	}

	if ((KNVRAM_AUTOT & flags) &&
	    p->transaction == KNVRAM_TRANSACTION_DISABLE) {
		pr_warning("knvram_open: autot and transactions disabled\n");
		err = -EPERM;
		goto out;
	}

	h = kzalloc(sizeof(*h), GFP_ATOMIC);
	if (unlikely(!h)) {
		err = -ENOMEM;
		goto out;
	}
	h->p = p;
	h->flags = flags;

	p->handles++;
	if (KNVRAM_WRITE & flags)
		p->writer = 1;
out:
	mutex_unlock(&p->open_lock);
	return err ? ERR_PTR(err) : h;
}

struct knvram_handle *
knvram_open(const char *name, int flags)
{
	struct knvram_partition *p;
	int maxlen = KNVRAM_PARTNAME_MAXLEN + 1;

	if (strnlen(name, maxlen) == maxlen) {
		pr_err("%s: partition name to long: %s\n", __func__, name);
		return ERR_PTR(-EINVAL);
	}

	list_for_each_entry(p, &knvram_partitions, lh)
		if (strncmp(name, p->name, maxlen) == 0)
			return _knvram_open(p, flags);

	pr_warning("%s: partition not found: %s\n", __func__, name);
	return ERR_PTR(-ENODEV);
}

int
knvram_close(struct knvram_handle *h)
{
	struct knvram_partition *p = h->p;
	int err=0;

	if (KNVRAM_NONBLOCK & h->flags) {
		if (!mutex_trylock(&p->open_lock))
			return -EAGAIN;
	} else
		mutex_lock(&p->open_lock);

	if (p->transaction != KNVRAM_TRANSACTION_DISABLE) {
		err = knvram_tabort(h);
		if (err)
			goto out;
	}

	if (KNVRAM_WRITE & h->flags)
		p->writer = 0;
	p->handles--;

	/* Write to underlying hardware on last close */
	if (p->handles == 0) {
		err = knvram_sync(p);
		if (err) {
			pr_warning("%s: knvram_sync failed: %d\n",
				   __func__, err);
			err = 0;
		}
	}

out:
	mutex_unlock(&p->open_lock);
	return err;
}

int
knvram_setautot(struct knvram_handle *h, int autot)
{
	if (autot) {
		if (!h->p->transaction)
			return -EPERM;
		h->flags |= KNVRAM_AUTOT;
	} else {
		h->flags &= ~KNVRAM_AUTOT;
	}

	return 0;
}

static inline void
_knvram_tbegin(struct knvram_handle *h)
{
	BUG_ON(h->p->transaction == KNVRAM_TRANSACTION_DISABLE);
	h->flags |= KNVRAM_TRANSACTION;
	h->p->cow_bottom = -1; /* will always be different from new_bottom */
	h->p->cow_top = 0; /* will always be different from new_top */
}

int
knvram_tbegin(struct knvram_handle *h)
{
	struct knvram_partition *p = h->p;
	int err=0;

	if (KNVRAM_NONBLOCK & h->flags) {
		if (!mutex_trylock(&p->transaction_lock))
			return -EAGAIN;
	} else
		mutex_lock(&p->transaction_lock);

	if (p->transaction == KNVRAM_TRANSACTION_DISABLE) {
		err = -EPERM;
		goto out;
	}
	BUG_ON(p->transaction == KNVRAM_TRANSACTION_ENABLE);

	if (KNVRAM_TRANSACTION & h->flags) {
		err = -EBUSY;
		goto out;
	}

	_knvram_tbegin(h);

out:
	mutex_unlock(&p->transaction_lock);
	return err;
}

void
_knvram_tabort(struct knvram_handle *h)
{
	h->flags &= ~KNVRAM_TRANSACTION;
}

int
knvram_tabort(struct knvram_handle *h)
{
	struct knvram_partition *p = h->p;
	int err=0;

	if (KNVRAM_NONBLOCK & h->flags) {
		if (!mutex_trylock(&p->transaction_lock))
			return -EAGAIN;
	} else
		mutex_lock(&p->transaction_lock);

	if (p->transaction == KNVRAM_TRANSACTION_DISABLE) {
		err = -EPERM;
		goto out;
	}
	BUG_ON(p->transaction == KNVRAM_TRANSACTION_ENABLE);

	if (!(KNVRAM_TRANSACTION & h->flags)) {
		goto out;
	}

	_knvram_tabort(h);

out:
	mutex_unlock(&p->transaction_lock);
	return err;
}

void
_knvram_tcommit(struct knvram_handle *h)
{
	struct knvram_partition *p = h->p;

	memcpy(p->shadow + p->cow_bottom, p->transaction + p->cow_bottom,
	       p->cow_top - p->cow_bottom);

	h->flags &= ~KNVRAM_TRANSACTION;
}

int
knvram_tcommit(struct knvram_handle *h)
{
	struct knvram_partition *p = h->p;
	int err=0;

	if (KNVRAM_NONBLOCK & h->flags) {
		if (!mutex_trylock(&p->transaction_lock))
			return -EAGAIN;
	} else
		mutex_lock(&p->transaction_lock);

	if (p->transaction == KNVRAM_TRANSACTION_DISABLE) {
		err = -EPERM;
		goto out;
	}
	BUG_ON(p->transaction == KNVRAM_TRANSACTION_ENABLE);

	if (!(KNVRAM_TRANSACTION & h->flags)) {
		goto out;
	}

	if (p->cow_top == 0) {
		_knvram_tabort(h);
		goto out;
	}

	if (KNVRAM_NONBLOCK & h->flags) {
		if (!down_write_trylock(&p->shadow_lock)) {
			err = -EAGAIN;
			goto out;
		}
	} else
		down_write(&p->shadow_lock);

	_knvram_tcommit(h);

	up_write(&p->shadow_lock);

out:
	mutex_unlock(&p->transaction_lock);
	return err;
}

static int
knvram_read_shadow(struct knvram_handle *h, char *buf,
			size_t size, loff_t offset)
{
	if (KNVRAM_USER & h->flags) {
		if (copy_to_user(buf, (char __user *)h->p->shadow
				 + offset, size)) {
			pr_err("%s: copy_to_user failed\n", __func__);
			return -EFAULT;
		}
	} else {
		memcpy(buf, h->p->shadow + offset, size);
	}
	return 0;
}

static int
_knvram_read_transaction(struct knvram_handle *h, char *buf,
			      size_t size, loff_t offset)
{
	if (KNVRAM_USER & h->flags) {
		if (copy_to_user(buf, (char __user *)h->p->transaction
				 + offset, size)) {
			pr_err("%s: copy_to_user failed\n", __func__);
			return -EFAULT;
		}
	} else {
		memcpy(buf, h->p->transaction + offset, size);
	}
	return 0;
}

static inline int
knvram_read_transaction(struct knvram_handle *h, char *buf,
			size_t size, loff_t *offset)
{
	struct knvram_partition *p = h->p;
	size_t a, b, c, d;
	int err;

	a = *offset;
	d = *offset + size;

	if (p->cow_top == 0) {
		b = c = d;
	}

	else if (d <= p->cow_bottom) {
		b = c = d;
	}

	else if (a > p->cow_top) {
		b = c = a;
	}

	else {
		if (a < p->cow_bottom) // and implicit: d > p->cow_bottom
			b = p->cow_bottom;
		else
			b = a;

		if (d <= p->cow_top) // and implicit: a >= p->cow_top
			c = d;
		else
			c = p->cow_top;
	}

	if (a < b) {
		err = knvram_read_shadow(h, buf, b - a, a);
		if (err)
			return err;
	}

	if (b < c) {
		err = _knvram_read_transaction(h, buf, c - b, b);
		if (err)
			return err;
	}

	if (c < d) {
		err = knvram_read_shadow(h, buf, d - c, c);
		if (err)
			return err;
	}

	return 0;
}

ssize_t
knvram_read(struct knvram_handle *h, char *buf,
	    size_t size, loff_t *offset)
{
	struct knvram_partition *p = h->p;
	int err = 0;

	BUG_ON(p->shadow == NULL);

	/* Report EOF */
	if (unlikely(*offset == p->size))
		return 0;

	if (unlikely(*offset > p->size))
		return -EINVAL;

	if (unlikely((*offset + size) > p->size))
		size = p->size - *offset;

	if (KNVRAM_NONBLOCK & h->flags) {
		if (!down_read_trylock(&p->shadow_lock))
			return -EAGAIN;
	} else
		down_read(&p->shadow_lock);

	if (KNVRAM_TRANSACTION & h->flags) {

		if (KNVRAM_NONBLOCK & h->flags) {
			if (!mutex_trylock(&p->transaction_lock)) {
				err = -EAGAIN;
				goto out;
			}
		} else
			mutex_lock(&p->transaction_lock);

		err = knvram_read_transaction(h, buf, size, offset);

		mutex_unlock(&p->transaction_lock);

	} else { /* not KNVRAM_TRANSACTION */
		err = knvram_read_shadow(h, buf, size, *offset);
	}

	if (err)
		goto out;

	*offset += size;

out:
	up_read(&p->shadow_lock);
	return err ? err : size;
}

static inline int
knvram_write_shadow(struct knvram_handle *h,
		    const char *buf, size_t size, loff_t offset)
{
	if (KNVRAM_USER & h->flags) {
		if (copy_from_user((h->p->shadow + offset), buf, size)) {
			pr_err("%s: copy_from_user failed\n", __func__);
			return -EFAULT;
		}
	} else {
		memcpy(h->p->shadow + offset, buf, size);
	}
	return 0;
}

static inline int
_knvram_write_transaction(struct knvram_handle *h,
			  const char *buf, size_t size, loff_t offset)
{
	if (KNVRAM_USER & h->flags) {
		if (copy_from_user((h->p->transaction + offset), buf, size)) {
			pr_err("%s: copy_from_user failed\n", __func__);
			return -EFAULT;
		}
	} else {
		memcpy(h->p->transaction + offset, buf, size);
	}
	return 0;
}

static inline void
knvram_transaction_cow(struct knvram_partition *p, size_t first, size_t last)
{
	BUG_ON(last <= first);
	BUG_ON(last >= p->size);
	memcpy(p->transaction + first, p->shadow + first, (last - first));
}

static inline int
knvram_write_transaction(struct knvram_handle *h,
			 const char *buf, size_t size, loff_t *offset)
{
	struct knvram_partition *p = h->p;
	size_t first = *offset;
	size_t last = *offset + size - 1;
	size_t new_bottom, new_top;
	int err;

	BUG_ON(size < 1);

	/* First, copy in necessary pages from shadow to transaction
	 * before writing. This requires holding a read lock on
	 * shadow_lock. */

	if (KNVRAM_NONBLOCK & h->flags) {
		if (!down_read_trylock(&p->shadow_lock))
			return -EAGAIN;
	} else
		down_read(&p->shadow_lock);

	/* Determine new page-aligned cow boundaries */
	new_bottom = first & ~p->transaction_pagemask;
	new_top = last | p->transaction_pagemask;
	if (p->cow_top != 0) {
		if (new_bottom > p->cow_bottom)
			new_bottom = p->cow_bottom;
		if (new_top < p->cow_top)
			new_top = p->cow_top;
	}

	/* Fill hole between data (to come) and (old) cow bottom */
	if (p->cow_top != 0 && last < p->cow_bottom)
		knvram_transaction_cow(p, last, p->cow_bottom);

	/* Fill hole between data (to come) and (old) cow top */
	if (p->cow_top != 0 && first > p->cow_top)
		knvram_transaction_cow(p, p->cow_top, first);

	/* Fill in data for page aligning (new) cow bottom */
	if ((new_bottom != p->cow_bottom) && (first > new_bottom))
		knvram_transaction_cow(p, new_bottom, first);

	/* Fill in data for page aligning (new) cow top */
	if ((new_top != p->cow_top) && (last < new_top))
		knvram_transaction_cow(p, last, new_top);

	up_read(&p->shadow_lock);

	err = _knvram_write_transaction(h, buf, size, *offset);
	if (err)
		return err;

	p->cow_top = new_top;
	p->cow_bottom = new_bottom;

	return 0;
}

ssize_t
knvram_write(struct knvram_handle *h, const char *buf,
	     size_t size, loff_t *offset)
{
	struct knvram_partition *p = h->p;
	int err=0;

	BUG_ON(p->shadow == NULL);

	/* Report EOF */
	if (unlikely(*offset == p->size))
		return 0;

	if (unlikely(*offset > p->size))
		return -EINVAL;

	if (unlikely((*offset + size) > p->size))
		size = p->size - *offset;

	if ((KNVRAM_TRANSACTION | KNVRAM_AUTOT) & h->flags) {

		if (KNVRAM_NONBLOCK & h->flags) {
			if (!mutex_trylock(&p->transaction_lock))
				return -EAGAIN;
		} else
			mutex_lock(&p->transaction_lock);

		if (! (KNVRAM_TRANSACTION & h->flags))
			_knvram_tbegin(h);

		err = knvram_write_transaction(h, buf, size, offset);

		mutex_unlock(&p->transaction_lock);

	} else { /* not KNVRAM_TRANSACTION */

		if (KNVRAM_NONBLOCK & h->flags) {
			if (!down_write_trylock(&p->shadow_lock))
				return -EAGAIN;
		} else
			down_write(&p->shadow_lock);

		err = knvram_write_shadow(h, buf, size, *offset);

		up_write(&p->shadow_lock);
	}

	if (err)
		return err;

	*offset += size;
	return size;
}

int
knvram_sync(struct knvram_partition *p)
{
	int err;

	err = p->hw_write(p, p->shadow, p->size, 0);
	if (err) {
		pr_err("%s: write to hw failed: %d\n", __func__, err);
		return err;
	}

	return 0;
}

void
knvram_flush(void)
{
	struct knvram_partition *p;
	int err;
	list_for_each_entry(p, &knvram_partitions, lh) {
		err = knvram_sync(p);
		if (err)
			pr_alert("sync of knvram partition %s failed: %d",
				 p->name, err);
	}
}

MODULE_VERSION(DRV_VERSION);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Kernel non-volatile RAM driver");
