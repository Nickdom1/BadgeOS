/* SPDX-License-Identifier: GPL-2.0-only */
/* Reuse the bridge's framework fakes and actual extracted callbacks. */
#define main bridge_leaf_main
#include "test.c"
#undef main
static struct lt8912 fixture;
void pipeline_bridge_setup(struct lt8912b_pipeline *status, int fault)
{
	fixture = fresh();
	fixture.pipeline = status;
	if (fault > 0)
		fail_write = fault;
	if (fault < 0)
		fail_rail = -fault - 1;
}
void pipeline_bridge_pre(void)
{
	host_phy = fixture.pipeline->host_prepared;
	lt8912_bridge_pre_enable(&fixture.bridge);
}
void pipeline_bridge_enable(void)
{
	host_video = fixture.pipeline && fixture.pipeline->host_started;
	lt8912_bridge_enable(&fixture.bridge);
}
void pipeline_bridge_disable(void) { lt8912_bridge_disable(&fixture.bridge); }
void pipeline_bridge_post(void)
{
	host_video = fixture.pipeline && fixture.pipeline->host_started;
	lt8912_bridge_post_disable(&fixture.bridge);
}
unsigned int pipeline_bridge_writes(void) { return writes; }
void pipeline_bridge_stopped(void) { assert(!rails && (reset || rc_board) && !fixture.video_on); }

struct drm_bridge *pipeline_bridge_get(void) { return &fixture.bridge; }

void pipeline_bridge_fail_disable(bool fail) { fail_disable = fail; }

void pipeline_bridge_rc_board(void)
{
	fixture.rc_reset = rc_board = true;
	fixture.gp_reset = NULL;
	reset = false;
}
void pipeline_bridge_fail_receiver_stop(bool fail) { fail_receiver_stop = fail; }
