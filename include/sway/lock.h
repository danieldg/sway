#ifndef _SWAY_LOCK_H
#define _SWAY_LOCK_H

#include <wlr/types/wlr_buffer.h>

#define PERMALOCK_CLIENT (struct wl_client *)(-1)

struct sway_lock_state {
	// true -> if lock screen crashes, unrecoverable
	bool fail_locked;
	// if this is not NULL, screen is locked. If the lock screen crashed,
	// this may be set to PERMALOCK_CLIENT .
	struct wl_client *client;

	struct wlr_texture *permalock_message;
	struct wl_global *ext_unlocker_v1_global;

	struct wl_list locker_globals;
};

// todo: need destroy
void sway_lock_state_create(struct sway_lock_state *state,
		struct wl_display *display);

struct sway_output;
/** Create a texture which briefly explains the permalock state. */
struct wlr_texture *draw_permalock_message(struct sway_output *output);

#endif /* _SWAY_LOCK_H */
