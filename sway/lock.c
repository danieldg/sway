#define _POSIX_C_SOURCE 200809L
#include "log.h"
#include "sway/input/keyboard.h"
#include "sway/input/seat.h"
#include "sway/lock.h"
#include "sway/output.h"
#include "sway/server.h"

#define PERMALOCK_CLIENT (struct wl_client *)(-1)

struct sway_session_lock_surface {
	struct wlr_session_lock_surface_v1 *lock_surface;
	struct sway_output *output;
	struct wlr_surface *surface;
	struct wl_listener map;
	struct wl_listener destroy;
	struct wl_listener surface_commit;
	struct wl_listener output_mode;
};

static void handle_surface_map(struct wl_listener *listener, void *data) {
	struct sway_session_lock_surface* surf = wl_container_of(listener, surf, map);
	sway_force_focus(surf->surface);
	output_damage_whole(surf->output);
}

static void handle_surface_commit(struct wl_listener *listener, void *data) {
	struct sway_session_lock_surface* surf = wl_container_of(listener, surf, surface_commit);
	output_damage_surface(surf->output, 0, 0, surf->surface, false);
}

static void handle_output_mode(struct wl_listener *listener, void *data) {
	struct sway_session_lock_surface* surf = wl_container_of(listener, surf, output_mode);
	wlr_session_lock_surface_v1_configure(surf->lock_surface, surf->output->width, surf->output->height);
}

static void handle_surface_destroy(struct wl_listener *listener, void *data) {
	struct sway_session_lock_surface* surf = wl_container_of(listener, surf, destroy);
	wl_list_remove(&surf->map.link);
	wl_list_remove(&surf->destroy.link);
	wl_list_remove(&surf->surface_commit.link);
	wl_list_remove(&surf->output_mode.link);
	output_damage_whole(surf->output);
	free(surf);
}

static void handle_new_surface(struct wl_listener *listener, void *data) {
	struct wlr_session_lock_surface_v1 *lock_surface = data;
	struct sway_session_lock_surface* surf = calloc(1, sizeof(*surf));
	if (surf == NULL)
		return;

	sway_log(SWAY_DEBUG, "new lock layer surface");

	struct sway_output *output = lock_surface->output->data;
	wlr_session_lock_surface_v1_configure(lock_surface, output->width, output->height);

	surf->lock_surface = lock_surface;
	surf->surface = lock_surface->surface;
	surf->output = output;
	surf->map.notify = handle_surface_map;
	wl_signal_add(&lock_surface->events.map, &surf->map);
	surf->destroy.notify = handle_surface_destroy;
	wl_signal_add(&lock_surface->events.destroy, &surf->destroy);
	surf->surface_commit.notify = handle_surface_commit;
	wl_signal_add(&surf->surface->events.commit, &surf->surface_commit);
	surf->output_mode.notify = handle_output_mode;
	wl_signal_add(&output->wlr_output->events.mode, &surf->output_mode);
}

static void handle_unlock(struct wl_listener *listener, void *data) {
	struct sway_session_lock_manager *lock_state = server.session_lock->data;

	sway_log(SWAY_DEBUG, "session unlocked");
	lock_state->locked = false;
	lock_state->lock = NULL;

	wl_list_remove(&lock_state->new_surface.link);
	wl_list_remove(&lock_state->unlock.link);
	wl_list_remove(&lock_state->abandon.link);

	struct sway_seat *seat;
	wl_list_for_each(seat, &server.input->seats, link) {
		seat_set_exclusive_client(seat, NULL);
		// copied from input_manager -- deduplicate?
		struct sway_node *previous = seat_get_focus(seat);
		if (previous) {
			// Hack to get seat to re-focus the return value of get_focus
			seat_set_focus(seat, NULL);
			seat_set_focus(seat, previous);
		}
	}


	// redraw everything
	for (int i = 0; i < root->outputs->length; ++i) {
		struct sway_output *output = root->outputs->items[i];
		output_damage_whole(output);
	}
}

static void handle_abandon(struct wl_listener *listener, void *data) {
	struct sway_session_lock_manager *lock_state = server.session_lock->data;

	sway_log(SWAY_INFO, "session lock abandoned");
	lock_state->lock = NULL;

	wl_list_remove(&lock_state->new_surface.link);
	wl_list_remove(&lock_state->unlock.link);
	wl_list_remove(&lock_state->abandon.link);

	struct sway_seat *seat;
	wl_list_for_each(seat, &server.input->seats, link) {
		seat_set_exclusive_client(seat, PERMALOCK_CLIENT);
	}

	// redraw everything
	for (int i = 0; i < root->outputs->length; ++i) {
		struct sway_output *output = root->outputs->items[i];
		output_damage_whole(output);
	}
}

static void handle_session_lock(struct wl_listener *listener, void *data) {
	struct sway_session_lock_manager *manager = wl_container_of(listener, manager, new_lock);
	struct wlr_session_lock_v1 *lock = data;
	struct wl_client *client = wl_resource_get_client(lock->resource);

	if (manager->lock) {
		wlr_session_lock_v1_destroy(lock);
		return;
	}

	sway_log(SWAY_DEBUG, "session locked");
	manager->locked = true;
	manager->lock = lock;
	lock->data = manager;

	struct sway_seat *seat;
	wl_list_for_each(seat, &server.input->seats, link) {
		seat_set_exclusive_client(seat, client);
	}

	wl_signal_add(&lock->events.new_surface, &manager->new_surface);
	wl_signal_add(&lock->events.unlock, &manager->unlock);
	wl_signal_add(&lock->events.destroy, &manager->abandon);

	wlr_session_lock_v1_send_locked(lock);

	// redraw everything
	for (int i = 0; i < root->outputs->length; ++i) {
		struct sway_output *output = root->outputs->items[i];
		output_damage_whole(output);
	}
}

static void handle_session_lock_destroy(struct wl_listener *listener, void *data) {
	struct sway_session_lock_manager *manager = wl_container_of(listener, manager, manager_destroy);
	wl_list_remove(&manager->new_lock.link);
	wl_list_remove(&manager->manager_destroy.link);
	free(manager);
}

struct wlr_session_lock_manager_v1 *sway_session_lock_manager_create(void) {
	struct wlr_session_lock_manager_v1 *session_lock = wlr_session_lock_manager_v1_create(server.wl_display);
	struct sway_session_lock_manager *lock_state = calloc(1, sizeof(*lock_state));
	if (!lock_state) {
		return NULL;
	}

	session_lock->data = lock_state;
	lock_state->new_surface.notify = handle_new_surface;
	lock_state->unlock.notify = handle_unlock;
	lock_state->abandon.notify = handle_abandon;
	lock_state->new_lock.notify = handle_session_lock;
	lock_state->manager_destroy.notify = handle_session_lock_destroy;
	wl_signal_add(&session_lock->events.new_lock, &lock_state->new_lock);
	wl_signal_add(&session_lock->events.destroy, &lock_state->manager_destroy);

	return session_lock;
}
