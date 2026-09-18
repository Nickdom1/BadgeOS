/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <errno.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include "sophgo-trace.h"

struct device {
};
struct device_attribute {
};
struct sophgo_dsi {
	struct {
		struct {
			struct sophgo_trace trace;
		} link;
	} pipeline;
	struct {
		void *dev;
	} host;
};
static struct sophgo_dsi owner;
static int messages, calls;
#define READ_ONCE(x) __atomic_load_n(&(x), __ATOMIC_RELAXED)
#define WRITE_ONCE(x, v) __atomic_store_n(&(x), (v), __ATOMIC_RELAXED)
#define dev_err(dev, ...)                                                                          \
	do {                                                                                       \
		(void)(dev);                                                                       \
		messages++;                                                                        \
	} while (0)
#define sysfs_emit(buf, ...) snprintf(buf, 4096, __VA_ARGS__)
static void *dev_get_drvdata(struct device *dev)
{
	(void)dev;
	return &owner;
}
/* Exact production sink and show: deliberately no mutex/MMIO shim exists. */
#include "trace.inc"
static jmp_buf blocked;
static int operation(bool hang)
{
	struct sophgo_trace *trace = &owner.pipeline.link.trace;
	char buf[4096];
	calls++;
	assert(!strcmp(trace->entered, "operation"));
	assert(!strcmp(trace->returned, "previous"));
	assert(pinstripe_progress_show(NULL, NULL, buf) > 0);
	assert(strstr(buf, "entered=operation returned=previous"));
	if (hang)
		longjmp(blocked, 1); /* model a primitive that never returns */
	return -EIO;
}
int main(void)
{
	struct sophgo_trace *trace = &owner.pipeline.link.trace;
	char buf[4096];
	assert(pinstripe_progress_show(NULL, NULL, buf) > 0);
	assert(!strcmp(buf, "enabled=0 entered=idle returned=none\n"));
	assert(SOPHGO_TRACE_VALUE(trace, "quiet", ++calls) == 1);
	assert(!messages && !trace->entered && !trace->returned);
	trace->ctx = &owner;
	trace->emit = sophgo_pinstripe_trace;
	SOPHGO_TRACE_VOID(trace, "previous", calls++);
	assert(messages == 2 && calls == 2);
	if (!setjmp(blocked)) {
		(void)SOPHGO_TRACE_VALUE(trace, "operation", operation(true));
		assert(false);
	}
	assert(messages == 3 && calls == 3);
	assert(!strcmp(trace->returned, "previous"));
	assert(SOPHGO_TRACE_VALUE(trace, "operation", operation(false)) == -EIO);
	assert(messages == 5 && calls == 4);
	assert(!strcmp(trace->returned, "operation"));
	assert(pinstripe_progress_show(NULL, NULL, buf) > 0);
	assert(strstr(buf, "entered=operation returned=operation"));
	puts("PASS: quiet default, before-call visibility, non-return retains last completion, "
	     "error result and single evaluation");
}
