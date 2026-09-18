/* SPDX-License-Identifier: GPL-2.0-only */
/* Deterministic polling edge for the userspace lifecycle test; no sleep. */
#define read_poll_timeout(op, val, cond, delay, timeout, sleep, args...)                           \
	({                                                                                         \
		(void)(delay);                                                                     \
		(void)(timeout);                                                                   \
		(void)(sleep);                                                                     \
		(val) = op(args);                                                                  \
		(cond) ? 0 : -ETIMEDOUT;                                                           \
	})
