#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include "log.h"
#include "sway/input/keyboard.h"
#include "sway/input/seat.h"
#include "sway/lock.h"
#include "sway/desktop/transaction.h"
#include "sway/output.h"
#include "sway/server.h"

static void desktop_enable(bool enable) {
	wlr_scene_node_set_enabled(root->layers.shell_background, enable);
	wlr_scene_node_set_enabled(root->layers.shell_bottom, enable);
	wlr_scene_node_set_enabled(root->layers.tiling, enable);
	wlr_scene_node_set_enabled(root->layers.floating, enable);
	wlr_scene_node_set_enabled(root->layers.shell_top, enable);
	wlr_scene_node_set_enabled(root->layers.fullscreen, enable);
	wlr_scene_node_set_enabled(root->layers.shell_overlay, enable);
	wlr_scene_node_set_enabled(root->layers.lockscreen, !enable);
}

static void handle_surface_map(struct wl_listener *listener, void *data) {
	struct sway_session_lock_surface* surf = wl_container_of(listener, surf, map);
	struct sway_output *output = surf->output;
	wlr_scene_node_set_position(surf->scene, output->lx, output->ly);
	wlr_scene_node_reparent(surf->scene, root->layers.lockscreen);
	sway_force_focus(surf->surface);
}

static void handle_output_commit(struct wl_listener *listener, void *data) {
	struct wlr_output_event_commit *event = data;
	struct sway_session_lock_surface *surf = wl_container_of(listener, surf, output_commit);
	if (event->committed & (
			WLR_OUTPUT_STATE_MODE |
			WLR_OUTPUT_STATE_SCALE |
			WLR_OUTPUT_STATE_TRANSFORM)) {
		wlr_session_lock_surface_v1_configure(surf->lock_surface,
			surf->output->width, surf->output->height);
	}
}

static void handle_surface_destroy(struct wl_listener *listener, void *data) {
	struct sway_session_lock_surface *surf = wl_container_of(listener, surf, destroy);
	wl_list_remove(&surf->map.link);
	wl_list_remove(&surf->destroy.link);
	wl_list_remove(&surf->output_commit.link);
	wlr_scene_node_destroy(surf->scene);
	free(surf);
}

static void handle_new_surface(struct wl_listener *listener, void *data) {
	struct wlr_session_lock_surface_v1 *lock_surface = data;
	struct sway_session_lock_surface *surf = calloc(1, sizeof(*surf));
	if (surf == NULL) {
		return;
	}

	sway_log(SWAY_DEBUG, "new lock layer surface");

	struct sway_output *output = lock_surface->output->data;
	wlr_session_lock_surface_v1_configure(lock_surface, output->width, output->height);

	lock_surface->data = surf;
	surf->lock_surface = lock_surface;
	surf->surface = lock_surface->surface;
	surf->output = output;
	surf->scene = wlr_scene_subsurface_tree_create(root->staging, surf->surface);

	surf->map.notify = handle_surface_map;
	wl_signal_add(&lock_surface->events.map, &surf->map);
	surf->destroy.notify = handle_surface_destroy;
	wl_signal_add(&lock_surface->events.destroy, &surf->destroy);
	surf->output_commit.notify = handle_output_commit;
	wl_signal_add(&output->wlr_output->events.commit, &surf->output_commit);
}

static void handle_unlock(struct wl_listener *listener, void *data) {
	sway_log(SWAY_DEBUG, "session unlocked");
	server.session_lock.locked = false;
	server.session_lock.lock = NULL;

	wl_list_remove(&server.session_lock.lock_new_surface.link);
	wl_list_remove(&server.session_lock.lock_unlock.link);
	wl_list_remove(&server.session_lock.lock_destroy.link);

	struct sway_seat *seat;
	wl_list_for_each(seat, &server.input->seats, link) {
		seat_set_exclusive_client(seat, NULL);
		// copied from seat_set_focus_layer -- deduplicate?
		struct sway_node *previous = seat_get_focus_inactive(seat, &root->node);
		if (previous) {
			// Hack to get seat to re-focus the return value of get_focus
			seat_set_focus(seat, NULL);
			seat_set_focus(seat, previous);
		}
	}

	desktop_enable(true);
}

static void handle_abandon(struct wl_listener *listener, void *data) {
	sway_log(SWAY_INFO, "session lock abandoned");
	server.session_lock.lock = NULL;

	wl_list_remove(&server.session_lock.lock_new_surface.link);
	wl_list_remove(&server.session_lock.lock_unlock.link);
	wl_list_remove(&server.session_lock.lock_destroy.link);

	struct sway_seat *seat;
	wl_list_for_each(seat, &server.input->seats, link) {
		seat->exclusive_client = NULL;
	}

	// abandoned -> red BG
	static const float color[4] = { 1.0, 0.0, 0.0, 1.0 };
	wlr_scene_rect_set_color(server.session_lock.base_rect, color);
}

static void handle_session_lock(struct wl_listener *listener, void *data) {
	struct wlr_session_lock_v1 *lock = data;
	struct wl_client *client = wl_resource_get_client(lock->resource);

	if (server.session_lock.lock) {
		wlr_session_lock_v1_destroy(lock);
		return;
	}

	sway_log(SWAY_DEBUG, "session locked");
	server.session_lock.locked = true;
	server.session_lock.lock = lock;

	struct sway_seat *seat;
	wl_list_for_each(seat, &server.input->seats, link) {
		seat_set_exclusive_client(seat, client);
	}

	wl_signal_add(&lock->events.new_surface, &server.session_lock.lock_new_surface);
	wl_signal_add(&lock->events.unlock, &server.session_lock.lock_unlock);
	wl_signal_add(&lock->events.destroy, &server.session_lock.lock_destroy);

	static const float color[4] = { 0.0, 0.0, 0.0, 1.0 };
	if (server.session_lock.base_rect) {
		wlr_scene_rect_set_color(server.session_lock.base_rect, color);
	} else {
		// Note: using INT_MAX here causes BAD_RECT
		// "one million pixels should be enough for anybody"
		server.session_lock.base_rect = wlr_scene_rect_create(root->layers.lockscreen, 1000000, 1000000, color);
	}
	desktop_enable(false);

	wlr_session_lock_v1_send_locked(lock);

	transaction_commit_dirty();
}

static void handle_session_lock_destroy(struct wl_listener *listener, void *data) {
	assert(server.session_lock.lock == NULL);
	wl_list_remove(&server.session_lock.new_lock.link);
	wl_list_remove(&server.session_lock.manager_destroy.link);
}

void sway_session_lock_init(void) {
	server.session_lock.manager = wlr_session_lock_manager_v1_create(server.wl_display);

	server.session_lock.lock_new_surface.notify = handle_new_surface;
	server.session_lock.lock_unlock.notify = handle_unlock;
	server.session_lock.lock_destroy.notify = handle_abandon;
	server.session_lock.new_lock.notify = handle_session_lock;
	server.session_lock.manager_destroy.notify = handle_session_lock_destroy;
	wl_signal_add(&server.session_lock.manager->events.new_lock,
		&server.session_lock.new_lock);
	wl_signal_add(&server.session_lock.manager->events.destroy,
		&server.session_lock.manager_destroy);
}
