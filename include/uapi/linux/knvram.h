/*
 * The Linux knvram driver
 *
 * Copyright 2010 DoréDevelopment ApS.
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

#ifndef _UAPI_LINUX_KNVRAM_H_
#define _UAPI_LINUX_KNVRAM_H_

#include <linux/ioctl.h>

#define KNVRAM_IOCTL_BASE	'K'

#define KNVRAMIOC_SYNC		_IO(KNVRAM_IOCTL_BASE, 0)
#define KNVRAMIOC_TBEGIN	_IO(KNVRAM_IOCTL_BASE, 1)
#define KNVRAMIOC_TCOMMIT	_IO(KNVRAM_IOCTL_BASE, 2)
#define KNVRAMIOC_TABORT	_IO(KNVRAM_IOCTL_BASE, 3)
#define KNVRAMIOC_SETAUTOT	_IOW(KNVRAM_IOCTL_BASE, 4, int)
#define KNVRAMIOC_GETAUTOT	_IOR(KNVRAM_IOCTL_BASE, 5, int)

#endif /* _UAPI_LINUX_KNVRAM_H_ */
