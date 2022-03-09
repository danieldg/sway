#ifndef _SWAY_LOCK_H
#define _SWAY_LOCK_H
#include <wlr/types/wlr_session_lock_v1.h>

struct sway_session_lock_surface {
	struct wlr_session_lock_surface_v1 *lock_surface;
	struct sway_output *output;
	struct wlr_surface *surface;
	struct wlr_scene_node *scene;
	struct wl_listener map;
	struct wl_listener destroy;
	struct wl_listener output_commit;
};

#endif
