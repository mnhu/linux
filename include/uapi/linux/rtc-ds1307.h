/*
 * Extended interface for some mostly-compatible I2C chips.
 *
 * Copyright (C) 2010 DoreDevelopment ApS
 */
#ifndef _LINUX_RTC_DS1307_H_
#define _LINUX_RTC_DS1307_H_

#include <linux/ioctl.h>

/*
 * ioctl calls that are permitted to the /dev/ds1307_temp interface
 */
#define RTC_DS1307_IOCBASE	'P'
#define DS1307IOC_GETTEMP	_IOR(RTC_DS1307_IOCBASE, 0x01, char)

#endif /* _LINUX_RTC_DS1307_H_ */
