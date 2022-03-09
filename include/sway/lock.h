#ifndef _SWAY_LOCK_H
#define _SWAY_LOCK_H
#include <wlr/types/wlr_session_lock_v1.h>

struct sway_session_lock_manager {
	bool locked;

	struct wlr_session_lock_v1 *lock;
	struct wl_listener new_surface;
	struct wl_listener unlock;
	struct wl_listener abandon;

	struct wl_listener new_lock;
	struct wl_listener manager_destroy;
};

#endif
