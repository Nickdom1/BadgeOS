/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
struct mutex {
	bool held;
};
static void mutex_lock(struct mutex *m)
{
	assert(!m->held);
	m->held = true;
}
static void mutex_unlock(struct mutex *m)
{
	assert(m->held);
	m->held = false;
}
struct drm_atomic_commit;
struct drm_bridge;
struct drm_bridge_funcs {
	void (*pre_enable)(struct drm_bridge *);
	void (*enable)(struct drm_bridge *);
	void (*disable)(struct drm_bridge *);
	void (*post_disable)(struct drm_bridge *);
	void (*atomic_pre_enable)(struct drm_bridge *, struct drm_atomic_commit *);
	void (*atomic_enable)(struct drm_bridge *, struct drm_atomic_commit *);
	void (*atomic_disable)(struct drm_bridge *, struct drm_atomic_commit *);
	void (*atomic_post_disable)(struct drm_bridge *, struct drm_atomic_commit *);
};
struct list_head {
	struct drm_bridge *first, *last;
};
struct drm_encoder {
	struct mutex bridge_chain_mutex;
	struct list_head bridge_chain;
};
struct drm_bridge {
	struct drm_encoder *encoder;
	const struct drm_bridge_funcs *funcs;
	bool pre_enable_prev_first;
	struct drm_bridge *prev, *next;
	struct {
		struct drm_bridge *owner;
	} chain_node;
};
#define list_for_each_entry_reverse(p, h, n) for ((p) = (h)->last; (p); (p) = (p)->prev)
#define list_for_each_entry_from(p, h, n) for (; (p); (p) = (p)->next)
#define list_for_each_entry_from_reverse(p, h, n) for (; (p); (p) = (p)->prev)
#define list_next_entry(p, n) ((p)->next)
#define list_prev_entry(p, n) ((p)->prev)
#define list_is_last(n, h) ((n)->owner == (h)->last)
#define drm_for_each_bridge_in_chain_from(first, p)                                                \
	for (struct drm_bridge *p = (first); p; p = p->next)
#include "chain.inc"
void pipeline_host_pre(void);
void pipeline_host_enable(void);
void pipeline_host_disable(void);
void pipeline_host_post(void);
void pipeline_bridge_pre(void);
void pipeline_bridge_enable(void);
void pipeline_bridge_disable(void);
void pipeline_bridge_post(void);
void pipeline_boundary(char);
static void host_pre(struct drm_bridge *b)
{
	(void)b;
	pipeline_host_pre();
}
static void host_enable(struct drm_bridge *b)
{
	(void)b;
	pipeline_host_enable();
}
static void host_disable(struct drm_bridge *b)
{
	(void)b;
	pipeline_host_disable();
}
static void host_post(struct drm_bridge *b)
{
	(void)b;
	pipeline_host_post();
}
static void bridge_pre(struct drm_bridge *b)
{
	(void)b;
	pipeline_boundary('p');
	pipeline_bridge_pre();
}
static void bridge_enable(struct drm_bridge *b)
{
	(void)b;
	pipeline_boundary('e');
	pipeline_bridge_enable();
}
static void bridge_disable(struct drm_bridge *b)
{
	(void)b;
	pipeline_boundary('d');
	pipeline_bridge_disable();
}
static void bridge_post(struct drm_bridge *b)
{
	(void)b;
	pipeline_boundary('u');
	pipeline_bridge_post();
}
static const struct drm_bridge_funcs host_ops = {host_pre, host_enable, host_disable, host_post};
static const struct drm_bridge_funcs bridge_ops = {bridge_pre, bridge_enable, bridge_disable,
						   bridge_post};
static struct drm_encoder encoder;
static struct drm_bridge host, bridge;
void pipeline_chain_setup(void)
{
	host = (struct drm_bridge){.encoder = &encoder, .funcs = &host_ops, .next = &bridge};
	bridge = (struct drm_bridge){.encoder = &encoder,
				     .funcs = &bridge_ops,
				     .prev = &host,
				     .pre_enable_prev_first = true};
	host.chain_node.owner = &host;
	bridge.chain_node.owner = &bridge;
	encoder.bridge_chain = (struct list_head){&host, &bridge};
}
void pipeline_chain_start(void)
{
	pipeline_chain_setup();
	drm_atomic_bridge_chain_pre_enable(&host, NULL);
	drm_atomic_bridge_chain_enable(&host, NULL);
}
void pipeline_chain_stop(void)
{
	drm_atomic_bridge_chain_disable(&host, NULL);
	drm_atomic_bridge_chain_post_disable(&host, NULL);
}

struct drm_bridge *pipeline_chain_bridge(void) { return &host; }
