/* SPDX-License-Identifier: GPL-2.0-only */
/* Deterministic diagnostic clock; not a model of MMIO or silicon latency. */
typedef long long s64;
typedef long long ktime_t;
static inline ktime_t ktime_get(void) {
	static ktime_t now;
	return now += 20000;
}
static inline s64 ktime_us_delta(ktime_t end, ktime_t begin) { return end - begin; }
