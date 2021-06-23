#define _POSIX_C_SOURCE 200809L
#include <drm_fourcc.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include "cairo_util.h"
#include "wp-screenlocker-unstable-v1-protocol.h"
#include "log.h"
#include "pango.h"
#include "sway/input/seat.h"
#include "sway/config.h"
#include "sway/commands.h"
#include "sway/lock.h"
#include "sway/output.h"
#include "sway/server.h"
#include "util.h"

struct wlr_locker_state {
	struct wl_list link; // sway_lock_state::locker_globals
	struct wl_resource *resource; // zwp_screenlocker_v1
};

static void handle_locker_lock(struct wl_client *client, struct wl_resource *resource, uint32_t id);

static void handle_get_visibility(struct wl_client *client, struct wl_resource *resource, uint32_t id, struct wl_resource *surface);

static void locker_resource_destroy(struct wl_resource *resource) {
	struct wlr_locker_state* state = wl_resource_get_user_data(resource);
	wl_list_remove(&state->link);
	free(state);
}

static void handle_lock_unlock(struct wl_client *client,
		struct wl_resource *resource) {
	if (server.lock_screen.client != client) {
		sway_log(SWAY_ERROR, "INVALID UNLOCK, IGNORING");
		return;
	}

	// The lockscreen may now shut down on its own schedule
	wl_resource_set_user_data(resource, NULL);
	server.lock_screen.client = NULL;
	server.lock_screen.fail_locked = false;
	sway_log(SWAY_ERROR, "RECEIVED UNLOCK");

	struct wlr_locker_state *locker;
	wl_list_for_each(locker, &server.lock_screen.locker_globals, link) {
		zwp_screenlocker_v1_send_unlocked(locker->resource);
	}

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

static void handle_lock_persist(struct wl_client *client, struct wl_resource *resource)
{
	void* data = wl_resource_get_user_data(resource);
	if (!data)
		return;
	server.lock_screen.fail_locked = true;
}

static void handle_lock_temporary(struct wl_client *client, struct wl_resource *resource)
{
	void* data = wl_resource_get_user_data(resource);
	if (!data)
		return;
	server.lock_screen.fail_locked = false;
}

static void handle_set_visibility(struct wl_client *client, struct wl_resource *resource, uint32_t visibility)
{
	struct lock_surface* state = wl_resource_get_user_data(resource);
	if (!state)
		return;
	state->mode = visibility;
	// TODO double-buffer this instead of eager-applying
	// TODO force redraw of this surface
}

static void vis_resource_destroy(struct wl_resource *resource) {
	struct lock_surface* state = wl_resource_get_user_data(resource);
	if (!state)
		return;
	wl_list_remove(&state->surface_destroy.link);
	wl_list_remove(&state->link);
	wl_resource_set_user_data(state->resource, NULL);
	free(state);
}

static void vis_surface_destroy(struct wl_listener *listener, void *data) {
	struct lock_surface *state = wl_container_of(listener, state, surface_destroy);
	wl_list_remove(&state->surface_destroy.link);
	wl_list_remove(&state->link);
	wl_resource_set_user_data(state->resource, NULL);
	free(state);
}

static void resource_destroy(struct wl_client *client, struct wl_resource *resource)
{
        wl_resource_destroy(resource);
}

static const struct zwp_screenlocker_v1_interface unlock_impl = {
	.destroy = resource_destroy,
	.get_visibility = handle_get_visibility,
	.lock = handle_locker_lock,
};

static const struct zwp_screenlocker_lock_v1_interface lock_impl = {
	.unlock = handle_lock_unlock,
	.set_persistent = handle_lock_persist,
	.set_temporary = handle_lock_temporary,
	.destroy = resource_destroy,
};

static const struct zwp_screenlocker_visibility_v1_interface visibility_impl = {
	.destroy = resource_destroy,
	.set_visibility = handle_set_visibility,
};

static void screenlock_bind(struct wl_client *client, void *data,
		uint32_t version, uint32_t id) {
	struct wlr_locker_state *state = calloc(1, sizeof(*state));
	if (!state) {
		wl_client_post_no_memory(client);
		return;
	}

	state->resource = wl_resource_create(client,
		&zwp_screenlocker_v1_interface, version, id);

	if (!state->resource) {
		free(state);
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(state->resource, &unlock_impl, state, locker_resource_destroy);
	wl_list_insert(&server.lock_screen.locker_globals, &state->link);

	if (server.lock_screen.client) {
		zwp_screenlocker_v1_send_locked(state->resource);
		if (server.lock_screen.client == PERMALOCK_CLIENT) {
			zwp_screenlocker_v1_send_lock_abandoned(state->resource);
		}
	} else {
		zwp_screenlocker_v1_send_unlocked(state->resource);
	}
}


static void lock_resource_destroy(struct wl_resource *resource) {
	void* data = wl_resource_get_user_data(resource);
	// ignore inert objects
	if (!data)
		return;

	// assert(server.lock_screen.client == wl_resource_get_client(resource));

	/* client closed, but did not unlock and reset the server.lock_screen_client */
	if (server.lock_screen.fail_locked) {
		sway_log(SWAY_ERROR, "THE LOCKSCREEN CLIENT DIED, PERMALOCKING");
		server.lock_screen.client = PERMALOCK_CLIENT;

		struct wlr_locker_state *locker;
		wl_list_for_each(locker, &server.lock_screen.locker_globals, link) {
			zwp_screenlocker_v1_send_lock_abandoned(locker->resource);
		}

		struct sway_seat *seat;
		wl_list_for_each(seat, &server.input->seats, link) {
			seat_set_exclusive_client(seat, PERMALOCK_CLIENT);
		}
	} else {
		sway_log(SWAY_ERROR, "THE LOCKSCREEN CLIENT DIED, UNLOCKING");

		handle_lock_unlock(wl_resource_get_client(resource), resource);
	}

	// redraw everything
	for (int i = 0; i < root->outputs->length; ++i) {
		struct sway_output *output = root->outputs->items[i];
		output_damage_whole(output);
	}
}

void sway_lock_state_create(struct sway_lock_state *state,
		struct wl_display *display) {
	wl_list_init(&state->locker_globals);
	wl_list_init(&state->lock_surfaces);
	state->ext_unlocker_v1_global =
		wl_global_create(display, &zwp_screenlocker_v1_interface,
			1, NULL, screenlock_bind);
}

struct wlr_texture *draw_permalock_message(struct sway_output *output) {
	sway_log(SWAY_ERROR, "CREATING PERMALOCK MESSAGE");

	int width = 0;
	int height = 0;

	const char* permalock_msg = "Lock screen crashed. Can only unlock by running lock_screen again.";

	// We must use a non-nil cairo_t for cairo_set_font_options to work.
	// Therefore, we cannot use cairo_create(NULL).
	cairo_surface_t *dummy_surface = cairo_image_surface_create(
			CAIRO_FORMAT_ARGB32, 0, 0);
	cairo_t *c = cairo_create(dummy_surface);
	cairo_set_antialias(c, CAIRO_ANTIALIAS_BEST);
	cairo_font_options_t *fo = cairo_font_options_create();
	cairo_font_options_set_hint_style(fo, CAIRO_HINT_STYLE_NONE);
	cairo_set_font_options(c, fo);
	get_text_size(c, config->font, &width, &height, NULL, output->wlr_output->scale,
			config->pango_markup, "%s", permalock_msg);
	cairo_surface_destroy(dummy_surface);
	cairo_destroy(c);

	cairo_surface_t *surface = cairo_image_surface_create(
			CAIRO_FORMAT_ARGB32, width, height);
	cairo_t *cairo = cairo_create(surface);
	cairo_set_antialias(cairo, CAIRO_ANTIALIAS_BEST);
	cairo_set_font_options(cairo, fo);
	cairo_font_options_destroy(fo);
	cairo_set_source_rgba(cairo, 1.0,1.0,1.0,0.0);
	cairo_paint(cairo);
	PangoContext *pango = pango_cairo_create_context(cairo);
	cairo_set_source_rgba(cairo, 0.,0.,0.,1.0);
	cairo_move_to(cairo, 0, 0);

	pango_printf(cairo, config->font, output->wlr_output->scale, config->pango_markup,
			"%s", permalock_msg);

	cairo_surface_flush(surface);
	unsigned char *data = cairo_image_surface_get_data(surface);
	int stride = cairo_image_surface_get_stride(surface);
	struct wlr_renderer *renderer = wlr_backend_get_renderer(
			output->wlr_output->backend);
	struct wlr_texture *tex = wlr_texture_from_pixels(
			renderer, DRM_FORMAT_ARGB8888, stride, width, height, data);
	cairo_surface_destroy(surface);
	g_object_unref(pango);
	cairo_destroy(cairo);
	return tex;
}

static void handle_locker_lock(struct wl_client *client, struct wl_resource *locker_resource, uint32_t id)
{
	bool inert = server.lock_screen.client && server.lock_screen.client != PERMALOCK_CLIENT;
	struct wl_resource *lock_resource = wl_resource_create(client,
		&zwp_screenlocker_lock_v1_interface, 1, id);
	if (lock_resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}

	wl_resource_set_implementation(lock_resource,
		&lock_impl, inert ? NULL : &server.lock_screen,
		lock_resource_destroy);

	if (inert) {
		zwp_screenlocker_lock_v1_send_rejected(lock_resource, NULL);
		return;
	}

	if (!server.lock_screen.client) {
		// no re-broadcast when recovering from a permalock
		struct wlr_locker_state *locker;
		wl_list_for_each(locker, &server.lock_screen.locker_globals, link) {
			zwp_screenlocker_v1_send_locked(locker->resource);
		}
	}

	server.lock_screen.client = client;

	// only lock screen gets input; this applies immediately,
	// before the lock screen program is set up
	struct sway_seat *seat;
	wl_list_for_each(seat, &server.input->seats, link) {
		seat_set_exclusive_client(seat, client);
	}

	// TODO delay this send until next frame?
	zwp_screenlocker_lock_v1_send_locked(lock_resource);

	sway_log(SWAY_ERROR, "LOCKSCREEN STARTED");
}

static void handle_get_visibility(struct wl_client *client, struct wl_resource *resource, uint32_t id, struct wl_resource *surface_resource)
{
	struct wlr_surface *surface = wlr_surface_from_resource(surface_resource);

	struct lock_surface *state = calloc(1, sizeof(*state));
	if (state == NULL) {
		wl_client_post_no_memory(client);
		return;
	}

	state->resource = wl_resource_create(client,
		&zwp_screenlocker_visibility_v1_interface, 1, id);
	if (state->resource == NULL) {
		free(state);
		wl_client_post_no_memory(client);
		return;
	}

	state->surface = surface;
	state->mode = 0;
	wl_signal_add(&surface->events.destroy, &state->surface_destroy);
	state->surface_destroy.notify = vis_surface_destroy;
	wl_list_insert(&server.lock_screen.lock_surfaces, &state->link);

	// TODO reject non-layer-shell surfaces?

	wl_resource_set_implementation(state->resource,
		&visibility_impl, state,
		vis_resource_destroy);
}
