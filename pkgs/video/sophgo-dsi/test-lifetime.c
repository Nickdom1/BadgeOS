/* SPDX-License-Identifier: GPL-2.0-only */
/* Execute production entry points with a deterministic SRCU/framework model.
 * The blocked ioctl has already passed the generic unplug check, exactly the
 * interleaving missed by a fake that only checks unplug/shutdown call order.
 */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

struct mutex {
	pthread_mutex_t lock;
};
static void mutex_lock(struct mutex *m) { assert(!pthread_mutex_lock(&m->lock)); }
static void mutex_unlock(struct mutex *m) { assert(!pthread_mutex_unlock(&m->lock)); }
struct drm_encoder {
	bool attached;
};
struct drm_connector {
	unsigned int possible_encoders;
	void *state;
};
struct drm_device {
	void *dev_private;
	int refs, readers;
	bool unplugged, config_alive, bridge_alive;
	int shutdowns, releases, ioctls, commits;
};
struct sophgo_dsi {
	bool manual_request;
	struct {
		struct {
			const char *stage;
			int error;
		} status;
	} pipeline;
	void *host_bridge;
	struct {
		void *dev;
	} host;
	struct mutex commit_lock;
	struct drm_device *drm;
};
struct sophgo_kms {
	struct mutex release_lock;
	struct drm_encoder encoder;
	struct drm_connector connector;
	struct sophgo_dsi *dsi;
};
struct drm_minor {
	struct drm_device *dev;
};
struct drm_file {
	struct drm_minor *minor;
};
struct file {
	void *private_data;
};
struct inode {
	int unused;
};
struct drm_atomic_commit {
	int unused;
};
static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static bool entered, proceed, unplug_waiting, detached;
static struct drm_device *active;
static int freed, connector_states_released;
static bool fail_shutdown;
static bool drm_dev_enter(struct drm_device *drm, int *idx)
{
	pthread_mutex_lock(&gate);
	bool live = !drm->unplugged;
	if (live) {
		drm->readers++;
		*idx = 0;
	}
	pthread_mutex_unlock(&gate);
	return live;
}
static void drm_dev_exit(int idx)
{
	(void)idx;
	pthread_mutex_lock(&gate);
	assert(active->readers-- > 0);
	pthread_cond_broadcast(&changed);
	pthread_mutex_unlock(&gate);
}
static void drm_dev_unplug(struct drm_device *drm)
{
	pthread_mutex_lock(&gate);
	drm->unplugged = true;
	unplug_waiting = true;
	pthread_cond_broadcast(&changed);
	while (drm->readers)
		pthread_cond_wait(&changed, &gate);
	pthread_mutex_unlock(&gate);
}
static bool drm_dev_is_unplugged(struct drm_device *drm) { return drm->unplugged; }
static int sophgo_atomic_commit(struct drm_device *drm, struct drm_atomic_commit *state,
				bool nonblock);
static void drm_atomic_helper_shutdown(struct drm_device *drm)
{
	assert(!drm->readers && drm->bridge_alive && drm->config_alive);
	/* Real shutdown commits through mode_config.funcs after unplug. */
	if (!fail_shutdown)
		assert(!sophgo_atomic_commit(drm, NULL, false));
	drm->shutdowns++;
}
static void drm_atomic_helper_connector_destroy_state(struct drm_connector *connector, void *state)
{
	assert(state && state == connector->state && !active->readers);
	connector_states_released++; /* Includes a routing self-reference. */
}
static void drm_encoder_cleanup(struct drm_encoder *encoder)
{
	assert(active->bridge_alive && !active->readers && encoder->attached);
	assert(!((struct sophgo_kms *)active->dev_private)->connector.state);
	encoder->attached = false;
}
static void drm_dev_get(struct drm_device *drm) { assert(drm->refs++ > 0); }
static void drm_dev_put(struct drm_device *drm)
{
	assert(drm->refs > 0);
	if (!--drm->refs) {
		struct sophgo_kms *kms = drm->dev_private;
		assert(drm->config_alive && !kms->encoder.attached && !kms->dsi);
		drm->config_alive = false; /* drmm_mode_config_init cleanup */
		assert(!pthread_mutex_destroy(&kms->release_lock.lock));
		free(kms);
		drm->dev_private = NULL;
		freed++;
	}
}
static long drm_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct drm_device *drm = ((struct drm_file *)file->private_data)->minor->dev;
	(void)cmd;
	(void)arg;
	pthread_mutex_lock(&gate);
	/* Model generic drm_ioctl's initial non-SRCU check. */
	assert(!drm->unplugged);
	entered = true;
	pthread_cond_broadcast(&changed);
	while (!proceed)
		pthread_cond_wait(&changed, &gate);
	assert(drm->config_alive && drm->bridge_alive);
	assert(((struct sophgo_kms *)drm->dev_private)->dsi);
	drm->ioctls++;
	pthread_mutex_unlock(&gate);
	return 17;
}
static long drm_compat_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	return drm_ioctl(file, cmd, arg);
}
static int drm_release(struct inode *inode, struct file *file)
{
	struct drm_device *drm = ((struct drm_file *)file->private_data)->minor->dev;
	(void)inode;
	/* drm_file_free uses mode config to release file-owned FBs and blobs. */
	assert(drm->config_alive);
	drm->releases++;
	drm_dev_put(drm);
	return 0;
}
static int drm_atomic_helper_commit(struct drm_device *drm, struct drm_atomic_commit *state,
				    bool nonblock)
{
	(void)state;
	assert(!nonblock && drm->bridge_alive);
	drm->commits++;
	return 0;
}
#define dev_err(dev, ...) ((void)(dev))
static int sophgo_pipeline_complete(void *p, void *b)
{
	(void)p;
	(void)b;
	return 0;
}
static void sophgo_pipeline_shutdown(struct sophgo_dsi *dsi) { (void)dsi; }
#include "lifetime.inc"
static void *operation(void *arg)
{
	assert(sophgo_ioctl(arg, 0, 0) == 17);
	return NULL;
}
static void *detach(void *arg)
{
	sophgo_kms_destroy(arg);
	pthread_mutex_lock(&gate);
	detached = true;
	pthread_cond_broadcast(&changed);
	pthread_mutex_unlock(&gate);
	return NULL;
}
static void init(struct drm_device *drm, struct sophgo_dsi *dsi)
{
	struct sophgo_kms *kms = calloc(1, sizeof(*kms));
	assert(kms && !pthread_mutex_init(&kms->release_lock.lock, NULL));
	kms->dsi = dsi;
	kms->encoder.attached = true;
	kms->connector.possible_encoders = 1;
	kms->connector.state = kms; /* model current routing/connector state */
	*drm = (struct drm_device){
	    .dev_private = kms, .refs = 2, .config_alive = true, .bridge_alive = true};
	dsi->drm = drm; /* one owner ref and one retained file */
}
int main(void)
{
	struct sophgo_dsi *host = calloc(1, sizeof(*host));
	struct drm_device old, fresh;
	struct drm_minor minor = {&old};
	struct drm_file priv = {&minor};
	struct file file = {&priv};
	pthread_t op, teardown;
	init(&old, host);
	active = &old;
	host->manual_request = true;
	assert(sophgo_atomic_commit(&old, NULL, false) == -EBUSY && !old.commits);
	host->manual_request = false;
	assert(!sophgo_atomic_commit(&old, NULL, true));
	assert(old.commits == 1 && !old.readers);
	assert(!pthread_create(&op, NULL, operation, &file));
	pthread_mutex_lock(&gate);
	while (!entered)
		pthread_cond_wait(&changed, &gate);
	pthread_mutex_unlock(&gate);
	assert(!pthread_create(&teardown, NULL, detach, host));
	pthread_mutex_lock(&gate);
	while (!unplug_waiting)
		pthread_cond_wait(&changed, &gate);
	assert(!detached && old.readers == 1 && !old.shutdowns);
	assert(old.config_alive && old.bridge_alive);
	proceed = true;
	pthread_cond_broadcast(&changed);
	pthread_mutex_unlock(&gate);
	assert(!pthread_join(op, NULL) && !pthread_join(teardown, NULL));
	assert(!host->drm && old.refs == 1 && old.config_alive && old.shutdowns == 1);
	old.bridge_alive = false; /* I2C devres can now disappear */
	init(&fresh, host);	  /* fresh attachment cannot revive an old FD */
	assert(sophgo_ioctl(&file, 0, 0) == -ENODEV);
	assert(sophgo_compat_ioctl(&file, 0, 0) == -ENODEV);
	assert(sophgo_atomic_commit(&old, NULL, true) == -ENODEV);
	active = &fresh;
	fail_shutdown = true; /* Atomic-state allocation failure on second unbind. */
	sophgo_kms_destroy(host);
	assert(connector_states_released == 2);
	fresh.bridge_alive = false;
	free(host); /* platform devres gone before either file closes */
	assert(!sophgo_release(NULL, &file));
	assert(old.releases == 1 && !old.refs && !old.config_alive && freed == 1);
	minor.dev = &fresh;
	assert(!sophgo_release(NULL, &file));
	assert(fresh.releases == 1 && !fresh.refs && freed == 2);
	puts("PASS: in-flight ioctl drains before detach; retained FD survives reattach/I2C and "
	     "host removal; final release owns mode cleanup; commits synchronous");
}
