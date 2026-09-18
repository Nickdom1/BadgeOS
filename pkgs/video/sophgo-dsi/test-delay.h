/* SPDX-License-Identifier: GPL-2.0-only */
/* Native tests model ordering, not wall-clock delay. */
static inline void usleep_range(unsigned long low, unsigned long high)
{
	(void)low;
	(void)high;
}
