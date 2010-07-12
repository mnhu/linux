/*
 * Gallega reset counters support.
 *
 * Author: Esben Haabendal (eha@doredevelopment.dk.dk)
 *
 * Copyright 2009 Focon Electronic Systems A/S.
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 */

#ifndef MPC8XXX_RSTE_H
#define MPC8XXX_RSTE_H

#ifdef CONFIG_MPC8xxx_RSTE

/* Set reset event */
void mpc8xxx_rste_cause(unsigned);
void mpc8xxx_rste_panic(char *);

/* Bits for rste in struct gallega_reset_counters */
#define RESET_CAUSE_UBOOT_RESET		(1 << 0)
#define RESET_CAUSE_UBOOT_RESET_MASK	(RESET_CAUSE_UBOOT_RESET)

#define RESET_CAUSE_LINUX_RESET		(1 << 1)
#define RESET_CAUSE_LINUX_RESET_MASK	(RESET_CAUSE_LINUX_RESET |\
					 RESET_CAUSE_LINUX_PANIC |\
					 RESET_CAUSE_BOOT_TIMEOUT |\
					 RESET_CAUSE_APP_TIMEOUT |\
					 RESET_CAUSE_REBOOT_TIMEOUT)

#define RESET_CAUSE_BOOT_TIMEOUT	(1 << 2)
#define RESET_CAUSE_BOOT_TIMEOUT_MASK	(RESET_CAUSE_BOOT_TIMEOUT |\
					 RESET_CAUSE_LINUX_RESET |\
					 RESET_CAUSE_LINUX_PANIC |\
					 RESET_CAUSE_REBOOT_TIMEOUT)

#define RESET_CAUSE_APP_TIMEOUT		(1 << 3)
#define RESET_CAUSE_APP_TIMEOUT_MASK	(RESET_CAUSE_APP_TIMEOUT |\
					 RESET_CAUSE_LINUX_RESET |\
					 RESET_CAUSE_LINUX_PANIC |\
					 RESET_CAUSE_REBOOT_TIMEOUT)

#define RESET_CAUSE_REBOOT_TIMEOUT	(1 << 4)
#define RESET_CAUSE_REBOOT_TIMEOUT_MASK	(RESET_CAUSE_REBOOT_TIMEOUT |\
					 RESET_CAUSE_BOOT_TIMEOUT |\
					 RESET_CAUSE_APP_TIMEOUT |\
					 RESET_CAUSE_LINUX_RESET |\
					 RESET_CAUSE_LINUX_PANIC)

#define RESET_CAUSE_LINUX_PANIC		(1 << 5)
#define RESET_CAUSE_LINUX_PANIC_MASK	(RESET_CAUSE_LINUX_PANIC |\
					 RESET_CAUSE_LINUX_RESET |\
					 RESET_CAUSE_REBOOT_TIMEOUT |\
					 RESET_CAUSE_BOOT_TIMEOUT |\
					 RESET_CAUSE_APP_TIMEOUT)

#define RESET_CAUSE_MASK		(RESET_CAUSE_UBOOT_RESET |\
					 RESET_CAUSE_LINUX_RESET |\
					 RESET_CAUSE_BOOT_TIMEOUT |\
					 RESET_CAUSE_APP_TIMEOUT |\
					 RESET_CAUSE_REBOOT_TIMEOUT |\
					 RESET_CAUSE_LINUX_PANIC)

#else /* CONFIG_MPC8xxx_RSTE */

#define mpc8xxx_rste_cause(unsigned) do {} while (0)
#define mpc8xxx_rste_panic (NULL)

#endif /* CONFIG_MPC8xxx_RSTE */

#endif /* _MPC8XXX_RSTE_H */
