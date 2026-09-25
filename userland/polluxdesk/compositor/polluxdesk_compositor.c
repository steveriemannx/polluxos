/* SPDX-License-Identifier: MIT
 * Derived from wlroots tinywl (MIT). wlroots / Wayland are MIT-licensed.
 */
#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
#include <libdrm/drm_fourcc.h>
#include <libinput.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/libinput.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_shm.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_virtual_pointer_v1.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>

/* For brevity's sake, struct members are annotated where they are used. */
#define POLLUXDESK_TITLEBAR_HEIGHT 28
#define POLLUXDESK_BUTTON_SIZE 16
#define POLLUXDESK_WINDOW_RADIUS 14     /* rounded window corner radius */
#define POLLUXDESK_BACKGROUND_STRIPES 128
#define POLLUXDESK_SHADOW_BLUR 32
#define POLLUXDESK_SHADOW_MAX_ALPHA 96   /* 96/255 at the window edge */
#define POLLUXDESK_SHADOW_CORNER 14      /* rounded shadow corners */
/* Edge/corner drag hotspot, measured either side of the window edge. The
 * band reaches only a few pixels into the window: whatever a client draws
 * flush against its own edge -- a scrollbar above all -- lives there, and a
 * deeper inward band put it under the compositor instead of under the app,
 * so the pointer showed a resize cursor and the bar could not be clicked. */
#define POLLUXDESK_RESIZE_BORDER_IN 3    /* how far inside the edge it grabs */
#define POLLUXDESK_RESIZE_BORDER_OUT 9   /* and how far outside */

enum polluxdesk_cursor_mode {
	POLLUXDESK_CURSOR_PASSTHROUGH,
	POLLUXDESK_CURSOR_MOVE,
	POLLUXDESK_CURSOR_RESIZE,
};

struct polluxdesk_server {
	struct wl_display *wl_display;
	struct wlr_backend *backend;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;
	struct wlr_scene *scene;
	struct wlr_scene_output_layout *scene_layout;

	struct wlr_xdg_shell *xdg_shell;
	struct wlr_xdg_decoration_manager_v1 *xdg_decoration_manager;
	struct wlr_screencopy_manager_v1 *screencopy_manager;
	struct wlr_xdg_output_manager_v1 *xdg_output_manager;
	struct wl_listener new_xdg_toplevel;
	struct wl_listener new_xdg_popup;
	struct wl_listener new_xdg_decoration;
	struct wl_list toplevels;

	struct wlr_cursor *cursor;
	struct wlr_xcursor_manager *cursor_mgr;
	struct wlr_buffer *resize_cursor_h;      /* horizontal  <->  */
	struct wlr_buffer *resize_cursor_v;      /* vertical    ^  v */
	struct wlr_buffer *resize_cursor_nwse;   /* diagonal    \  / */
	struct wlr_buffer *resize_cursor_nesw;   /* diagonal    /  \ */
	struct wl_listener cursor_motion;
	struct wl_listener cursor_motion_absolute;
	struct wl_listener cursor_button;
	struct wl_listener cursor_axis;
	struct wl_listener cursor_frame;

	struct wlr_seat *seat;
	struct wlr_virtual_pointer_manager_v1 *virtual_pointer_mgr;
	struct wlr_virtual_keyboard_manager_v1 *virtual_keyboard_mgr;
	struct wl_listener new_input;
	struct wl_listener new_virtual_pointer;
	struct wl_listener new_virtual_keyboard;
	struct wl_listener request_cursor;
	struct wl_listener pointer_focus_change;
	struct wl_listener request_set_selection;
	struct wl_list keyboards;
	enum polluxdesk_cursor_mode cursor_mode;
	struct polluxdesk_toplevel *grabbed_toplevel;
	double grab_x, grab_y;
	struct wlr_box grab_geobox;
	uint32_t resize_edges;

	struct wlr_output_layout *output_layout;
	struct wl_list outputs;
	struct wl_listener new_output;

	/* Bumped by every write_window_state(); lets the shell tell a live
	 * window list from one left behind by an exited compositor. */
	unsigned int state_generation;
	bool cursor_warped;
	/* True while the pointer is wearing one of the compositor's own resize
	 * cursors. Moving off a border does not change pointer focus when it stays
	 * over the same surface, and focus is the only other place the cursor is
	 * reset -- so without this the resize arrow stayed up across the whole
	 * window. */
	bool resize_cursor_shown;
};

struct polluxdesk_output {
	struct wl_list link;
	struct polluxdesk_server *server;
	struct wlr_output *wlr_output;
	struct wlr_scene_tree *background_tree;
	struct wlr_scene_rect *background_stripes[POLLUXDESK_BACKGROUND_STRIPES];
	int background_width;
	int background_height;
	struct wl_listener frame;
	struct wl_listener request_state;
	struct wl_listener destroy;
};

enum polluxdesk_titlebar_button {
	POLLUXDESK_BUTTON_CLOSE,
	POLLUXDESK_BUTTON_MINIMIZE,
	POLLUXDESK_BUTTON_MAXIMIZE,
};

/* A one-shot, malloc-backed ARGB8888 wlr_buffer used for the soft window
 * shadows. wlroots renderers can consume DATA_PTR buffers through
 * wlr_buffer_begin_data_ptr_access(), so no GPU upload path is needed. */
struct polluxdesk_shadow_buffer {
	struct wlr_buffer base;
	uint32_t *pixels;
};

struct polluxdesk_toplevel {
	struct wl_list link;
	struct polluxdesk_server *server;
	struct wlr_xdg_toplevel *xdg_toplevel;
	struct wlr_scene_tree *scene_tree;
	struct wlr_scene_tree *content_tree;
	struct wlr_scene_rect *titlebar;
	struct wlr_scene_buffer *titlebar_buttons[3];
	struct wlr_scene_buffer *corners[4];   /* rounded-corner wallpaper masks */
	int corner_abs_y;                      /* gradient key of the corner masks */
	struct wlr_scene_buffer *shadow;
	int shadow_width;
	int shadow_height;
	bool is_desktop;
	bool is_borderless;
	bool is_overlay;
	bool overlay_positioned;
	bool client_side_decorated;
	bool alpha_debugged;
	bool minimized;
	/* Set between a minimize and the shell reporting that it has grabbed
	 * the window's pixels; the window stays on screen for that long. */
	bool pending_thumb;
	struct timespec thumb_since;
	struct wlr_xdg_toplevel_decoration_v1 *decoration;
	bool maximized;
	bool menu_open;      /* desktop shell dropdown is visible */
	struct wlr_box restore_geo;
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener commit;
	struct wl_listener set_title;
	struct wl_listener destroy;
	struct wl_listener request_move;
	struct wl_listener request_resize;
	struct wl_listener request_maximize;
	struct wl_listener request_fullscreen;
};

struct polluxdesk_popup {
	struct wlr_xdg_popup *xdg_popup;
	struct wl_listener commit;
	struct wl_listener set_title;
	struct wl_listener destroy;
};

struct polluxdesk_keyboard {
	struct wl_list link;
	struct polluxdesk_server *server;
	struct wlr_keyboard *wlr_keyboard;

	struct wl_listener modifiers;
	struct wl_listener key;
	struct wl_listener destroy;
};


#ifndef BTN_LEFT
#define BTN_LEFT 0x110
#endif

/* macOS traffic-light colors (red / yellow / green). */
static const float kButtonColors[3][4] = {
	{ 1.00f, 0.37f, 0.34f, 1.0f },
	{ 1.00f, 0.74f, 0.18f, 1.0f },
	{ 0.16f, 0.79f, 0.25f, 1.0f },
};

/* Frosted titlebar / light Big Sur gradient. */
static const float kTitlebarColor[4]    = { 0.93f, 0.94f, 0.96f, 0.94f };
static const float kTitlebarLineColor[4]= { 0.00f, 0.00f, 0.00f, 0.14f };
static float gBackgroundTop[4]           = { 0.44f, 0.66f, 0.97f, 1.0f };
static float gBackgroundBottom[4]        = { 0.91f, 0.93f, 0.98f, 1.0f };
static volatile sig_atomic_t g_settings_reload = 0;

static void settings_signal_handler(int signal_number) {
	(void)signal_number;
	g_settings_reload = 1;
}

static void shadow_buffer_destroy(struct wlr_buffer *wlr_buffer) {
	struct polluxdesk_shadow_buffer *shadow =
		wl_container_of(wlr_buffer, shadow, base);
	free(shadow->pixels);
	free(shadow);
}

static bool shadow_buffer_begin_data_ptr_access(struct wlr_buffer *wlr_buffer,
		uint32_t flags, void **data, uint32_t *format, size_t *stride) {
	(void)flags;
	struct polluxdesk_shadow_buffer *shadow =
		wl_container_of(wlr_buffer, shadow, base);
	*data = shadow->pixels;
	*format = DRM_FORMAT_ARGB8888;
	*stride = (size_t)wlr_buffer->width * 4;
	return true;
}

static void shadow_buffer_end_data_ptr_access(struct wlr_buffer *wlr_buffer) {
	(void)wlr_buffer;
}

static const struct wlr_buffer_impl shadow_buffer_impl = {
	.destroy = shadow_buffer_destroy,
	.begin_data_ptr_access = shadow_buffer_begin_data_ptr_access,
	.end_data_ptr_access = shadow_buffer_end_data_ptr_access,
};

/* Signed distance to the rounded-rectangle window outline. Negative inside,
 * positive outside. */
static float rounded_rect_distance(float x, float y,
		float cx, float cy, float half_w, float half_h, float radius) {
	float qx = fabsf(x - cx) - (half_w - radius);
	float qy = fabsf(y - cy) - (half_h - radius);
	float outside_x = fmaxf(qx, 0.0f);
	float outside_y = fmaxf(qy, 0.0f);
	float outside = sqrtf(outside_x * outside_x + outside_y * outside_y);
	float inside = fminf(fmaxf(qx, qy), 0.0f);
	return inside + outside - radius;
}

/* Soft, compositor-owned macOS shadow. The pixels form a transparent center
 * and a quadratic alpha falloff around the outline; there are no nested
 * rectangles, so no black banding at the shadow edge. */
static struct wlr_buffer *shadow_buffer_create(int width, int height) {
	const int blur = POLLUXDESK_SHADOW_BLUR;
	const int corner = POLLUXDESK_SHADOW_CORNER;
	const int bw = width + blur * 2;
	const int bh = height + blur * 2;
	if (width <= 0 || height <= 0) {
		return NULL;
	}

	struct polluxdesk_shadow_buffer *shadow = calloc(1, sizeof(*shadow));
	if (shadow == NULL) {
		return NULL;
	}
	shadow->pixels = calloc((size_t)bw * bh, sizeof(uint32_t));
	if (shadow->pixels == NULL) {
		free(shadow);
		return NULL;
	}

	const float cx = (float)blur + (float)width * 0.5f;
	const float cy = (float)blur + (float)height * 0.5f;
	const float half_w = (float)width * 0.5f;
	const float half_h = (float)height * 0.5f;
	const float max_alpha = (float)POLLUXDESK_SHADOW_MAX_ALPHA;

	for (int y = 0; y < bh; ++y) {
		for (int x = 0; x < bw; ++x) {
			float d = rounded_rect_distance((float)x, (float)y,
				cx, cy, half_w, half_h, (float)corner);
			uint8_t a = 0;
			if (d > 0.0f && d < (float)blur) {
				float t = d / (float)blur;
				float falloff = (1.0f - t) * (1.0f - t);
				a = (uint8_t)(max_alpha * falloff + 0.5f);
			}
			/* Premultiplied black: ARGB = 0xAARRGGBB with A in the top
			 * byte. Keeping the center fully transparent lets the client
			 * surface and titlebar remain the only content inside. */
			shadow->pixels[y * bw + x] = (uint32_t)a << 24;
		}
	}

	wlr_buffer_init(&shadow->base, &shadow_buffer_impl, bw, bh);
	return &shadow->base;
}

/* Wallpaper gradient color at absolute output coordinate `y` (the Big Sur
 * background is a pure vertical linear gradient between two stops). */
static struct wlr_output *server_get_primary_output(struct polluxdesk_server *server);
static void server_get_output_box(struct polluxdesk_server *server, struct wlr_box *box);

static uint32_t background_color_at(struct polluxdesk_server *server, int y) {
	struct wlr_box box;
	server_get_output_box(server, &box);
	float t = box.height > 1
		? (float)(y - box.y) / (float)(box.height - 1) : 0.0f;
	if (t < 0.0f) { t = 0.0f; }
	if (t > 1.0f) { t = 1.0f; }
	uint32_t r = (uint32_t)((gBackgroundTop[0] +
		(gBackgroundBottom[0] - gBackgroundTop[0]) * t) * 255.0f + 0.5f);
	uint32_t g = (uint32_t)((gBackgroundTop[1] +
		(gBackgroundBottom[1] - gBackgroundTop[1]) * t) * 255.0f + 0.5f);
	uint32_t b = (uint32_t)((gBackgroundTop[2] +
		(gBackgroundBottom[2] - gBackgroundTop[2]) * t) * 255.0f + 0.5f);
	return 0xFF000000u | (r << 16) | (g << 8) | b;
}

/* Circular macOS traffic light: an anti-aliased filled disc of `color`
 * (RGBA 0..1) in a size x size pixmap. */
static struct wlr_buffer *circle_button_buffer_create(
		const float color[4], int size) {
	struct polluxdesk_shadow_buffer *buf = calloc(1, sizeof(*buf));
	if (buf == NULL) {
		return NULL;
	}
	buf->pixels = calloc((size_t)size * size, sizeof(uint32_t));
	if (buf->pixels == NULL) {
		free(buf);
		return NULL;
	}
	const float c = (float)size * 0.5f;
	const float radius = c - 0.5f;
	for (int y = 0; y < size; ++y) {
		for (int x = 0; x < size; ++x) {
			float dx = (float)x + 0.5f - c;
			float dy = (float)y + 0.5f - c;
			float d = sqrtf(dx * dx + dy * dy);
			float a = fminf(fmaxf(radius - d + 0.5f, 0.0f), 1.0f)
				* color[3];
			uint8_t va = (uint8_t)(a * 255.0f + 0.5f);
			/* Premultiplied ARGB8888. */
			uint8_t vr = (uint8_t)(color[0] * a * 255.0f + 0.5f);
			uint8_t vg = (uint8_t)(color[1] * a * 255.0f + 0.5f);
			uint8_t vb = (uint8_t)(color[2] * a * 255.0f + 0.5f);
			buf->pixels[y * size + x] =
				((uint32_t)va << 24) | ((uint32_t)vr << 16) |
				((uint32_t)vg << 8) | vb;
		}
	}
	wlr_buffer_init(&buf->base, &shadow_buffer_impl, size, size);
	return &buf->base;
}

enum polluxdesk_resize_cursor {
	POLLUXDESK_RESIZE_CURSOR_H,
	POLLUXDESK_RESIZE_CURSOR_V,
	POLLUXDESK_RESIZE_CURSOR_NWSE,
	POLLUXDESK_RESIZE_CURSOR_NESW,
};

static void cursor_put_pixel(uint32_t *pixels, int size, int x, int y,
		uint32_t color) {
	if (x >= 0 && x < size && y >= 0 && y < size) {
		pixels[y * size + x] = color;
	}
}

static void cursor_draw_line(uint32_t *pixels, int size,
		int x0, int y0, int x1, int y1, int thickness, uint32_t color) {
	int dx = abs(x1 - x0);
	int dy = abs(y1 - y0);
	int sx = x0 < x1 ? 1 : -1;
	int sy = y0 < y1 ? 1 : -1;
	int err = dx - dy;
	int half = thickness / 2;
	while (true) {
		for (int oy = -half; oy <= half; ++oy) {
			for (int ox = -half; ox <= half; ++ox) {
				cursor_put_pixel(pixels, size, x0 + ox, y0 + oy, color);
			}
		}
		if (x0 == x1 && y0 == y1) {
			break;
		}
		int e2 = 2 * err;
		if (e2 > -dy) {
			err -= dy;
			x0 += sx;
		}
		if (e2 < dx) {
			err += dx;
			y0 += sy;
		}
	}
}

static void cursor_fill_triangle(uint32_t *pixels, int size,
		int x0, int y0, int x1, int y1, int x2, int y2, uint32_t color) {
	int min_x = x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2);
	int max_x = x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2);
	int min_y = y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2);
	int max_y = y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2);
	for (int y = min_y; y <= max_y; ++y) {
		for (int x = min_x; x <= max_x; ++x) {
			int d1 = (x - x0) * (y1 - y0) - (y - y0) * (x1 - x0);
			int d2 = (x - x1) * (y2 - y1) - (y - y1) * (x2 - x1);
			int d3 = (x - x2) * (y0 - y2) - (y - y2) * (x0 - x2);
			bool has_neg = (d1 < 0) || (d2 < 0) || (d3 < 0);
			bool has_pos = (d1 > 0) || (d2 > 0) || (d3 > 0);
			if (!(has_neg && has_pos)) {
				cursor_put_pixel(pixels, size, x, y, color);
			}
		}
	}
}

/* A transparent-background bold double-arrow resize cursor. No square
 * background/border: just the arrow so it looks like a normal resize cursor. */
static struct wlr_buffer *resize_cursor_buffer_create(int direction) {
	const int size = 28;
	struct polluxdesk_shadow_buffer *buf = calloc(1, sizeof(*buf));
	if (buf == NULL) {
		return NULL;
	}
	buf->pixels = calloc((size_t)size * size, sizeof(uint32_t));
	if (buf->pixels == NULL) {
		free(buf);
		return NULL;
	}
	uint32_t *p = buf->pixels;
	const uint32_t black = 0xFF000000u;

	if (direction == POLLUXDESK_RESIZE_CURSOR_H) {
		cursor_draw_line(p, size, 6, 14, 21, 14, 3, black);
		cursor_fill_triangle(p, size, 4, 14, 10, 9, 10, 19, black);
		cursor_fill_triangle(p, size, 23, 14, 17, 9, 17, 19, black);
	} else if (direction == POLLUXDESK_RESIZE_CURSOR_V) {
		cursor_draw_line(p, size, 14, 6, 14, 21, 3, black);
		cursor_fill_triangle(p, size, 14, 4, 9, 10, 19, 10, black);
		cursor_fill_triangle(p, size, 14, 23, 9, 17, 19, 17, black);
	} else if (direction == POLLUXDESK_RESIZE_CURSOR_NWSE) {
		cursor_draw_line(p, size, 6, 6, 21, 21, 3, black);
		cursor_fill_triangle(p, size, 4, 4, 11, 6, 6, 11, black);
		cursor_fill_triangle(p, size, 23, 23, 16, 21, 21, 16, black);
	} else {
		cursor_draw_line(p, size, 21, 6, 6, 21, 3, black);
		cursor_fill_triangle(p, size, 23, 4, 21, 11, 16, 6, black);
		cursor_fill_triangle(p, size, 4, 23, 6, 16, 11, 21, black);
	}

	wlr_buffer_init(&buf->base, &shadow_buffer_impl, size, size);
	return &buf->base;
}

/* One rounded-window corner mask. Each pixel reproduces what would actually
 * be visible behind the window at that spot - the wallpaper gradient dimmed
 * by the window's own drop shadow (same blur/max-alpha/falloff model as
 * shadow_buffer_create) - so the rounded corners blend seamlessly with the
 * soft shadow instead of showing flat wallpaper squares.
 * corner: 0=TL, 1=TR, 2=BL, 3=BR. abs_y is the absolute output Y of the
 * corner patch (the wallpaper is a vertical gradient). */
static struct wlr_buffer *corner_mask_buffer_create(
		struct polluxdesk_server *server, int radius, int corner, int abs_y) {
	struct polluxdesk_shadow_buffer *buf = calloc(1, sizeof(*buf));
	if (buf == NULL) {
		return NULL;
	}
	buf->pixels = calloc((size_t)radius * radius, sizeof(uint32_t));
	if (buf->pixels == NULL) {
		free(buf);
		return NULL;
	}
	/* Quarter-circle center in texture-local coordinates. */
	float cx = (corner == 0 || corner == 2) ? (float)radius : 0.0f;
	float cy = (corner == 0 || corner == 1) ? (float)radius : 0.0f;
	uint32_t bg = background_color_at(server, abs_y + radius / 2);
	const float blur = (float)POLLUXDESK_SHADOW_BLUR;
	const float max_shadow_a = (float)POLLUXDESK_SHADOW_MAX_ALPHA / 255.0f;
	for (int y = 0; y < radius; ++y) {
		for (int x = 0; x < radius; ++x) {
			float dx = (float)x + 0.5f - cx;
			float dy = (float)y + 0.5f - cy;
			float dist = sqrtf(dx * dx + dy * dy);
			/* Signed distance to the rounded outline: negative inside the
			 * window, positive in the hidden sliver outside it. */
			float d_out = dist - (float)radius;
			/* Coverage: anti-aliased ramp across the arc edge. */
			float cov = fminf(fmaxf(d_out + 0.5f, 0.0f), 1.0f);
			if (cov <= 0.0f) {
				continue;   /* inside the window: keep transparent */
			}
			/* Own-shadow alpha at this pixel (quadratic falloff). */
			float sa = 0.0f;
			if (d_out > 0.0f && d_out < blur) {
				float t = d_out / blur;
				sa = max_shadow_a * (1.0f - t) * (1.0f - t);
			}
			/* Opaque replacement of the backdrop ("wallpaper under black
			 * shadow") so the real shadow beneath is never blended twice:
			 * only the 1px arc-edge ramp stays translucent for AA. */
			uint8_t va = (uint8_t)(cov * 255.0f + 0.5f);
			uint8_t vr = (uint8_t)(((bg >> 16) & 0xFF) * (1.0f - sa) * cov + 0.5f);
			uint8_t vg = (uint8_t)(((bg >> 8) & 0xFF) * (1.0f - sa) * cov + 0.5f);
			uint8_t vb = (uint8_t)((bg & 0xFF) * (1.0f - sa) * cov + 0.5f);
			buf->pixels[y * radius + x] =
				((uint32_t)va << 24) | ((uint32_t)vr << 16) |
				((uint32_t)vg << 8) | vb;
		}
	}
	wlr_buffer_init(&buf->base, &shadow_buffer_impl, radius, radius);
	return &buf->base;
}

/* Prefer the internal panel.  The list is filled in connector-discovery order
 * (wl_list_insert puts each new output at the front), so an external monitor
 * could otherwise decide the size of the desktop shell -- a 1280x800 HDMI
 * panel made the shell draw itself at 1280x800 on a 1920x1080 laptop panel,
 * leaving the titlebar and dock laid out for the wrong screen.  Fall back to
 * the list order when there is no eDP. */
static struct wlr_output *server_get_primary_output(struct polluxdesk_server *server) {
	if (wl_list_empty(&server->outputs)) {
		return NULL;
	}
	struct polluxdesk_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		const char *name = output->wlr_output->name;
		if (name != NULL && strncmp(name, "eDP", 3) == 0) {
			return output->wlr_output;
		}
	}
	output = wl_container_of(server->outputs.next, output, link);
	return output->wlr_output;
}

static void server_get_output_box(struct polluxdesk_server *server, struct wlr_box *box) {
	box->x = 0;
	box->y = 0;
	box->width = 1280;
	box->height = 720;
	struct wlr_output *output = server_get_primary_output(server);
	if (output != NULL) {
		box->width = output->width;
		box->height = output->height;
	}
}

static bool toplevel_is_desktop_shell(struct polluxdesk_toplevel *toplevel) {
	const char *title = toplevel->xdg_toplevel->title;
	return title != NULL && strncmp(title, "PolluxOS Desktop", 16) == 0;
}

static bool toplevel_is_login_shell(struct polluxdesk_toplevel *toplevel) {
	const char *title = toplevel->xdg_toplevel->title;
	return title != NULL && strcmp(title, "PolluxOS") == 0;
}

/* The Launchpad app grid: a borderless overlay window that is kept centered
 * and pinned above all regular app windows (like a real macOS overlay). */
static bool toplevel_is_overlay(struct polluxdesk_toplevel *toplevel) {
	const char *title = toplevel->xdg_toplevel->title;
	return title != NULL && strcmp(title, "PolluxOS Launchpad") == 0;
}

/* Currently-mapped Launchpad overlay in this server, if any. */
static struct polluxdesk_toplevel *server_get_overlay(
		struct polluxdesk_server *server) {
	struct polluxdesk_toplevel *toplevel;
	wl_list_for_each(toplevel, &server->toplevels, link) {
		if (toplevel->is_overlay && !toplevel->minimized) {
			return toplevel;
		}
	}
	return NULL;
}

/* Temporary client-side decoration hook: the dui client does not speak the
 * xdg-decoration protocol yet, so use the example window title to identify
 * windows that draw their own title bar. The compositor still provides the
 * system shadow for these windows, but no server-side traffic-light row. */
static bool toplevel_is_client_side_decorated(struct polluxdesk_toplevel *toplevel) {
	const char *title = toplevel->xdg_toplevel->title;
	return title != NULL && strcmp(title, "basic") == 0;
}

static void toplevel_update_shell_flags(struct polluxdesk_toplevel *toplevel) {
	toplevel->is_desktop = toplevel_is_desktop_shell(toplevel);
	toplevel->is_overlay = toplevel_is_overlay(toplevel);
	toplevel->is_borderless = toplevel->is_desktop ||
		toplevel->is_overlay || toplevel_is_login_shell(toplevel);
	toplevel->client_side_decorated =
		toplevel_is_client_side_decorated(toplevel);
}

static void arrange_toplevel(struct polluxdesk_toplevel *toplevel) {
	struct wlr_xdg_toplevel *xdg_toplevel = toplevel->xdg_toplevel;
	struct wlr_box geo = xdg_toplevel->base->geometry;
	int width = geo.width;
	int height = geo.height;
	if (width <= 0) {
		width = xdg_toplevel->base->surface->current.width;
	}
	if (height <= 0) {
		height = xdg_toplevel->base->surface->current.height;
	}
	/* xdg window geometry can include an invisible client-side titlebar
	 * inset above the root surface. It shifts the surface down by -geo.y, so
	 * exclude that inset from the visible client height as well. */
	if (geo.y < 0 && height > -geo.y) {
		height += geo.y;
	}
	if (width < 1) {
		width = 1;
	}
	if (height < 1) {
		height = 1;
	}

	bool no_server_titlebar = toplevel->is_borderless ||
		toplevel->client_side_decorated;
	int titlebar_height = no_server_titlebar ? 0 : POLLUXDESK_TITLEBAR_HEIGHT;
	bool show_shadows = !toplevel->is_borderless && !toplevel->minimized &&
		!toplevel->maximized;

	/* App content is usually a separate scene subtree below the server
	 * titlebar. Client-side decorated windows keep their content at y=0 but
	 * still receive the compositor's system shadow. */
	/* Some Wayland clients (including winit before it receives the server-side
	 * decoration configure) report a negative geometry.y for their invisible
	 * client-side titlebar inset. Cancel that inset so the content starts at
	 * the compositor titlebar instead of leaving a strip of wallpaper below it. */
	int geometry_inset_y = geo.y < 0 ? geo.y : 0;
	wlr_scene_node_set_position(&toplevel->content_tree->node,
		0, titlebar_height + geometry_inset_y);
	wlr_scene_node_set_enabled(&toplevel->titlebar->node, !no_server_titlebar);
	for (int i = 0; i < 3; ++i) {
		wlr_scene_node_set_enabled(&toplevel->titlebar_buttons[i]->node,
			!no_server_titlebar);
	}

	/* Compositor-owned macOS drop shadow: one pixmap with a soft
	 * quadratic falloff. It is regenerated only when the window size
	 * changes; maximize/minimize just toggles the scene node. */
	int window_h = titlebar_height + height;
	wlr_scene_node_set_enabled(&toplevel->shadow->node, show_shadows);
	if (show_shadows) {
		wlr_scene_node_set_position(&toplevel->shadow->node,
			-POLLUXDESK_SHADOW_BLUR, -POLLUXDESK_SHADOW_BLUR);
		if (toplevel->shadow_width != width ||
				toplevel->shadow_height != window_h) {
			struct wlr_buffer *buffer = shadow_buffer_create(width, window_h);
			if (buffer != NULL) {
				wlr_scene_buffer_set_buffer(toplevel->shadow, buffer);
				wlr_buffer_drop(buffer);
				toplevel->shadow_width = width;
				toplevel->shadow_height = window_h;
			}
		}
	}

	if (!no_server_titlebar) {
		wlr_scene_rect_set_size(toplevel->titlebar, width, POLLUXDESK_TITLEBAR_HEIGHT);
		for (int i = 0; i < 3; ++i) {
			wlr_scene_node_set_position(&toplevel->titlebar_buttons[i]->node,
				14 + i * 22, (POLLUXDESK_TITLEBAR_HEIGHT - POLLUXDESK_BUTTON_SIZE) / 2);
		}
	}

	/* Rounded window corners: four tiny wallpaper-colored masks that cover
	 * the square slivers outside the quarter-circle outline. The mask color
	 * is sampled from the wallpaper gradient at the corner's absolute Y, so
	 * the patches are regenerated when the window moves vertically. */
	const int r = POLLUXDESK_WINDOW_RADIUS;
	bool show_corners = !toplevel->is_borderless && !toplevel->maximized &&
		width > r * 2 && window_h > r * 2;
	int corner_abs_y = -1;
	if (show_corners) {
		corner_abs_y = toplevel->scene_tree->node.y;
	}
	for (int i = 0; i < 4; ++i) {
		wlr_scene_node_set_enabled(&toplevel->corners[i]->node, show_corners);
	}
	if (show_corners && toplevel->corner_abs_y != corner_abs_y) {
		const int cx[4] = { 0, width - r, 0, width - r };
		const int cy[4] = { 0, 0, window_h - r, window_h - r };
		for (int i = 0; i < 4; ++i) {
			struct wlr_buffer *buffer = corner_mask_buffer_create(
				toplevel->server, r, i,
				toplevel->scene_tree->node.y + cy[i]);
			if (buffer != NULL) {
				wlr_scene_buffer_set_buffer(toplevel->corners[i], buffer);
				wlr_buffer_drop(buffer);
			}
			wlr_scene_node_set_position(&toplevel->corners[i]->node, cx[i], cy[i]);
		}
		toplevel->corner_abs_y = corner_abs_y;
	} else if (show_corners) {
		/* Same gradient band: only reposition (e.g. after a resize). */
		const int cx[4] = { 0, width - r, 0, width - r };
		const int cy[4] = { 0, 0, window_h - r, window_h - r };
		for (int i = 0; i < 4; ++i) {
			wlr_scene_node_set_position(&toplevel->corners[i]->node, cx[i], cy[i]);
		}
	}

	/* The Launchpad overlay: keep it centered on the output and pinned
	 * above every regular app window while it exists. Re-centered on every
	 * arrange so a client-side resize does not leave it misplaced. */
	if (toplevel->is_overlay && width > 1 && window_h > 1) {
		struct wlr_box box;
		server_get_output_box(toplevel->server, &box);
		wlr_scene_node_set_position(&toplevel->scene_tree->node,
			box.x + (box.width - width) / 2,
			box.y + (box.height - window_h) / 2);
		wlr_scene_node_raise_to_top(&toplevel->scene_tree->node);
	}
}

/* Height of the compositor-drawn titlebar for this window (0 when the client
 * draws its own chrome / the window is borderless). */
static int toplevel_titlebar_height(struct polluxdesk_toplevel *toplevel) {
	if (toplevel->is_borderless || toplevel->client_side_decorated) {
		return 0;
	}
	return POLLUXDESK_TITLEBAR_HEIGHT;
}

/* Full visible window rectangle in output coordinates, including the
 * server-drawn titlebar. Used both for resize hit-testing and for the
 * resize drag math (content size = window size minus titlebar). */
static void toplevel_visible_box(struct polluxdesk_toplevel *toplevel,
		struct wlr_box *box) {
	struct wlr_xdg_toplevel *xdg_toplevel = toplevel->xdg_toplevel;
	struct wlr_box geo = xdg_toplevel->base->geometry;
	int width = geo.width > 0 ? geo.width :
		(xdg_toplevel->base->surface->current.width > 0
			? xdg_toplevel->base->surface->current.width : 1);
	int height = geo.height > 0 ? geo.height :
		(xdg_toplevel->base->surface->current.height > 0
			? xdg_toplevel->base->surface->current.height : 1);
	if (geo.y < 0 && height > -geo.y) {
		height += geo.y;
	}
	int titlebar = toplevel_titlebar_height(toplevel);
	box->x = (int)toplevel->scene_tree->node.x;
	box->y = (int)toplevel->scene_tree->node.y;
	box->width = width;
	box->height = height + titlebar;
}

/* Returns the WLR_EDGE_* combination for a pointer over the four edges /
 * four corners of a server-decorated window, or 0 when it is not a resize
 * hotspot. Borderless/overlay/desktop windows are never resize targets. */
static uint32_t toplevel_resize_edges_at(struct polluxdesk_toplevel *toplevel,
		double lx, double ly) {
	if (toplevel->is_desktop || toplevel->is_borderless ||
			toplevel->is_overlay || toplevel->maximized) {
		return 0;
	}
	struct wlr_box box;
	toplevel_visible_box(toplevel, &box);
	const int in = POLLUXDESK_RESIZE_BORDER_IN;
	const int out = POLLUXDESK_RESIZE_BORDER_OUT;

	bool left = lx >= box.x - out && lx < box.x + in;
	bool right = lx > box.x + box.width - in && lx <= box.x + box.width + out;
	bool top = ly >= box.y - out && ly < box.y + in;
	bool bottom = ly > box.y + box.height - in && ly <= box.y + box.height + out;

	uint32_t edges = 0;
	if (left) {
		edges |= WLR_EDGE_LEFT;
	}
	if (right) {
		edges |= WLR_EDGE_RIGHT;
	}
	if (top) {
		edges |= WLR_EDGE_TOP;
	}
	if (bottom) {
		edges |= WLR_EDGE_BOTTOM;
	}
	return edges;
}

static void set_resize_cursor(struct polluxdesk_server *server, uint32_t edges) {
	struct wlr_buffer *buffer = NULL;
	if (edges == (WLR_EDGE_LEFT | WLR_EDGE_TOP) ||
			edges == (WLR_EDGE_RIGHT | WLR_EDGE_BOTTOM)) {
		buffer = server->resize_cursor_nwse;
	} else if (edges == (WLR_EDGE_RIGHT | WLR_EDGE_TOP) ||
			edges == (WLR_EDGE_LEFT | WLR_EDGE_BOTTOM)) {
		buffer = server->resize_cursor_nesw;
	} else if (edges == WLR_EDGE_LEFT || edges == WLR_EDGE_RIGHT) {
		buffer = server->resize_cursor_h;
	} else if (edges == WLR_EDGE_TOP || edges == WLR_EDGE_BOTTOM) {
		buffer = server->resize_cursor_v;
	}
	if (buffer != NULL) {
		wlr_cursor_set_buffer(server->cursor, buffer, 14, 14, 1.0f);
		server->resize_cursor_shown = true;
	}
}

/* Keep the desktop shell's dropdown menus above regular app windows while a
 * menu is open. When the menu closes, restore the normal desktop order: apps
 * back above the shell, Launchpad overlay on top. */
static void server_update_desktop_menu_layer(struct polluxdesk_server *server,
		struct polluxdesk_toplevel *desktop, bool open) {
	if (desktop == NULL || !desktop->is_desktop ||
			desktop->menu_open == open) {
		return;
	}
	desktop->menu_open = open;

	if (open) {
		wlr_scene_node_raise_to_top(&desktop->scene_tree->node);
		return;
	}

	/* Raise every ordinary window and the Launchpad overlay above the
	 * desktop shell again so apps receive input and remain visible above the
	 * wallpaper/dock when no dropdown is open. */
	struct polluxdesk_toplevel *toplevel;
	wl_list_for_each(toplevel, &server->toplevels, link) {
		if (toplevel == desktop || toplevel->is_desktop ||
				toplevel->is_borderless || toplevel->minimized) {
			continue;
		}
		wlr_scene_node_raise_to_top(&toplevel->scene_tree->node);
	}
	struct polluxdesk_toplevel *overlay = server_get_overlay(server);
	if (overlay != NULL) {
		wlr_scene_node_raise_to_top(&overlay->scene_tree->node);
	}
}

/* ---------------------------------------------------------------------------
 * Window state, published for the desktop shell.
 *
 * The shell's only way of talking to the compositor is its window title, and
 * that channel runs one way. "Which programs are running, and which of their
 * windows are minimized" is the other direction, so the compositor writes it
 * to a file instead and the shell picks it up on the one-second timer it
 * already runs. No signal, no new protocol, and a hand-edited file is just
 * as valid as anything written here.
 *
 * Written atomically: the shell reads on a timer and must never catch half a
 * record.
 *
 * Each record carries app_id, title, the minimized and focused flags, the
 * window size and the client's process id. The pid is the only usable
 * identity: dui hard-codes app_id, and the title changes with the document.
 * ------------------------------------------------------------------------ */

/* '|' separates the fields and '\n' ends the record, so a title containing
 * either would forge fields for whoever parses the file. */
static void state_sanitize(const char *src, char *dst, size_t size) {
	size_t i = 0;
	for (; src != NULL && src[i] != '\0' && i + 1 < size; ++i) {
		char c = src[i];
		dst[i] = (c == '|' || c == '\n' || c == '\r') ? ' ' : c;
	}
	dst[i] = '\0';
}

static void write_window_state(struct polluxdesk_server *server) {
	const char *home = getenv("HOME");
	if (home == NULL) {
		return;
	}
	char dir[1024];
	char path[1088];
	char tmp[1120];
	snprintf(dir, sizeof(dir), "%s/.config/polluxdesk", home);
	mkdir(dir, 0755);   /* already existing is the normal case */
	snprintf(path, sizeof(path), "%s/state.conf", dir);
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);

	FILE *file = fopen(tmp, "w");
	if (file == NULL) {
		return;
	}

	server->state_generation++;
	fprintf(file, "gen=%u\n", server->state_generation);
	fprintf(file, "desktop_pid=%ld\n", (long)getpid());

	struct wlr_surface *focused = server->seat != NULL
		? server->seat->keyboard_state.focused_surface : NULL;

	struct polluxdesk_toplevel *toplevel;
	wl_list_for_each(toplevel, &server->toplevels, link) {
		/* The desktop shell and the launchpad are not applications: they
		 * must never collect a running indicator or a window thumbnail. */
		if (toplevel->is_desktop || toplevel->is_overlay) {
			continue;
		}
		char app_id[256];
		char title[512];
		state_sanitize(toplevel->xdg_toplevel->app_id, app_id, sizeof(app_id));
		state_sanitize(toplevel->xdg_toplevel->title, title, sizeof(title));

		/* dui hard-codes its app_id to one value for every window, so the
		 * app_id cannot say which program a window belongs to. The client's
		 * process id can: the reader resolves it to an executable name. */
		pid_t pid = 0;
		uid_t uid = 0;
		gid_t gid = 0;
		struct wl_resource *resource =
			toplevel->xdg_toplevel->base->surface->resource;
		if (resource != NULL) {
			struct wl_client *client = wl_resource_get_client(resource);
			if (client != NULL) {
				wl_client_get_credentials(client, &pid, &uid, &gid);
			}
		}

		if (toplevel->pending_thumb) {
			/* Where the shell should point grim. The window is still on
			 * screen at this moment, which is the whole point. */
			struct wlr_box box;
			toplevel_visible_box(toplevel, &box);
			fprintf(file, "thumb=%lu|%d|%d|%d|%d\n",
				(unsigned long)toplevel,
				box.x, box.y, box.width, box.height);
		}
		fprintf(file, "win=%lu|%s|%s|%d|%d|%d|%d|%ld\n",
			(unsigned long)toplevel,
			app_id,
			title,
			toplevel->minimized ? 1 : 0,
			toplevel->xdg_toplevel->base->surface == focused ? 1 : 0,
			toplevel->xdg_toplevel->base->geometry.width,
			toplevel->xdg_toplevel->base->geometry.height,
			(long)pid);
	}

	fflush(file);
	fsync(fileno(file));
	fclose(file);
	rename(tmp, path);
}

static void toggle_maximize(struct polluxdesk_toplevel *toplevel) {
	if (toplevel->is_desktop) {
		return;
	}
	struct polluxdesk_server *server = toplevel->server;
	if (!toplevel->maximized) {
		toplevel->restore_geo.x = toplevel->scene_tree->node.x;
		toplevel->restore_geo.y = toplevel->scene_tree->node.y;
		toplevel->restore_geo.width = toplevel->xdg_toplevel->base->geometry.width;
		toplevel->restore_geo.height = toplevel->xdg_toplevel->base->geometry.height;

		struct wlr_box output_box;
		server_get_output_box(server, &output_box);
		wlr_scene_node_set_position(&toplevel->scene_tree->node,
			output_box.x, output_box.y);
		wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel,
			output_box.width, output_box.height - POLLUXDESK_TITLEBAR_HEIGHT);
		wlr_xdg_toplevel_set_maximized(toplevel->xdg_toplevel, true);
		toplevel->maximized = true;
	} else {
		wlr_scene_node_set_position(&toplevel->scene_tree->node,
			toplevel->restore_geo.x, toplevel->restore_geo.y);
		wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel,
			toplevel->restore_geo.width, toplevel->restore_geo.height);
		wlr_xdg_toplevel_set_maximized(toplevel->xdg_toplevel, false);
		toplevel->maximized = false;
	}
	write_window_state(server);
}

static void minimize_toplevel(struct polluxdesk_toplevel *toplevel) {
	if (toplevel->is_desktop || toplevel->minimized) {
		return;
	}
	toplevel->minimized = true;
	/* The window stays on screen for one exchange. Its thumbnail has to come
	 * from somewhere: handing the shell a texture would need a protocol
	 * neither side has, and rendering the window offscreen here would mean
	 * reading pixels back off the GPU -- a much bigger job for the same
	 * picture. So the shell grabs the window's rectangle out of the
	 * composited output with grim and says when it has it, and only then is
	 * the window hidden. */
	toplevel->pending_thumb = true;
	clock_gettime(CLOCK_MONOTONIC, &toplevel->thumb_since);
	wlr_xdg_toplevel_set_activated(toplevel->xdg_toplevel, false);
	write_window_state(toplevel->server);
}

/* Hide a minimized window whose thumbnail has been taken. */
static void toplevel_finish_thumb(struct polluxdesk_toplevel *toplevel) {
	toplevel->pending_thumb = false;
	wlr_scene_node_set_enabled(&toplevel->scene_tree->node, false);
}

static void finish_pending_thumb(struct polluxdesk_server *server,
		unsigned long id) {
	struct polluxdesk_toplevel *toplevel;
	wl_list_for_each(toplevel, &server->toplevels, link) {
		if ((unsigned long)toplevel != id || !toplevel->pending_thumb) {
			continue;
		}
		toplevel_finish_thumb(toplevel);
		write_window_state(server);
		return;
	}
}

static void update_background(struct polluxdesk_output *output) {
	struct wlr_scene_tree *tree = output->background_tree;
	int width = output->wlr_output->width;
	int height = output->wlr_output->height;
	if (width <= 0 || height <= 0) {
		return;
	}
	if (width == output->background_width && height == output->background_height) {
		return;
	}
	output->background_width = width;
	output->background_height = height;
	int stripe_height = height / POLLUXDESK_BACKGROUND_STRIPES;
	for (int i = 0; i < POLLUXDESK_BACKGROUND_STRIPES; ++i) {
		float t = (float)i / (float)(POLLUXDESK_BACKGROUND_STRIPES - 1);
		float color[4] = {
			gBackgroundTop[0] + (gBackgroundBottom[0] - gBackgroundTop[0]) * t,
			gBackgroundTop[1] + (gBackgroundBottom[1] - gBackgroundTop[1]) * t,
			gBackgroundTop[2] + (gBackgroundBottom[2] - gBackgroundTop[2]) * t,
			1.0f,
		};
		if (output->background_stripes[i] == NULL) {
			output->background_stripes[i] = wlr_scene_rect_create(tree, 0, 0, color);
		} else {
			wlr_scene_rect_set_color(output->background_stripes[i], color);
		}
		int stripe_h = (i == POLLUXDESK_BACKGROUND_STRIPES - 1)
			? height - stripe_height * i : stripe_height;
		wlr_scene_rect_set_size(output->background_stripes[i], width, stripe_h);
		wlr_scene_node_set_position(&output->background_stripes[i]->node,
			0, stripe_height * i);
	}
	wlr_scene_node_lower_to_bottom(&tree->node);
}

static void begin_interactive(struct polluxdesk_toplevel *toplevel,
		enum polluxdesk_cursor_mode mode, uint32_t edges);

static void focus_toplevel(struct polluxdesk_toplevel *toplevel) {
	/* Note: this function only deals with keyboard focus. */
	if (toplevel == NULL || toplevel->is_desktop) {
		/* The desktop wallpaper shell never takes keyboard focus; the
		 * borderless login shell does. */
		return;
	}
	if (toplevel->minimized) {
		toplevel->minimized = false;
		/* Restored before the shell got round to the grab: the window is
		 * wanted back, so there is nothing left to hide. */
		toplevel->pending_thumb = false;
		wlr_scene_node_set_enabled(&toplevel->scene_tree->node, true);
	}
	struct polluxdesk_server *server = toplevel->server;
	struct wlr_seat *seat = server->seat;
	struct wlr_surface *prev_surface = seat->keyboard_state.focused_surface;
	struct wlr_surface *surface = toplevel->xdg_toplevel->base->surface;

	/* Launchpad overlay dismissal: focusing ANY other toplevel (clicking
	 * another window, Alt-Tab, ...) closes the overlay. */
	if (toplevel != NULL && !toplevel->is_overlay) {
		struct polluxdesk_toplevel *overlay = server_get_overlay(server);
		if (overlay != NULL && overlay != toplevel) {
			wlr_xdg_toplevel_send_close(overlay->xdg_toplevel);
		}
	}

	if (prev_surface == surface) {
		/* Don't re-focus an already focused surface. Restoring a minimized
		 * window lands here, so the state still has to be published. */
		write_window_state(server);
		return;
	}
	if (prev_surface) {
		/*
		 * Deactivate the previously focused surface. This lets the client know
		 * it no longer has focus and the client will repaint accordingly, e.g.
		 * stop displaying a caret.
		 */
		struct wlr_xdg_toplevel *prev_toplevel =
			wlr_xdg_toplevel_try_from_wlr_surface(prev_surface);
		if (prev_toplevel != NULL) {
			wlr_xdg_toplevel_set_activated(prev_toplevel, false);
		}
	}
	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
	/* Move the toplevel to the front */
	wlr_scene_node_raise_to_top(&toplevel->scene_tree->node);
	wl_list_remove(&toplevel->link);
	wl_list_insert(&server->toplevels, &toplevel->link);
	/* Activate the new surface */
	wlr_xdg_toplevel_set_activated(toplevel->xdg_toplevel, true);
	/*
	 * Tell the seat to have the keyboard enter this surface. wlroots will keep
	 * track of this and automatically send key events to the appropriate
	 * clients without additional work on your part.
	 */
	if (keyboard != NULL) {
		wlr_seat_keyboard_notify_enter(seat, surface,
			keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
	}
	write_window_state(server);
}

/* Bring back a window the shell asked for by id, which is the pointer value
 * published in state.conf. The shell has no protocol of its own to call, so
 * the request arrives as a window title; see xdg_toplevel_set_title. */
static void restore_minimized(struct polluxdesk_server *server,
		unsigned long id) {
	struct polluxdesk_toplevel *toplevel;
	wl_list_for_each(toplevel, &server->toplevels, link) {
		if ((unsigned long)toplevel != id) {
			continue;
		}
		/* focus_toplevel() clears the minimized flag, re-enables the scene
		 * node and raises the window, which is exactly a restore. */
		focus_toplevel(toplevel);
		return;
	}
}

static void keyboard_handle_modifiers(
		struct wl_listener *listener, void *data) {
	/* This event is raised when a modifier key, such as shift or alt, is
	 * pressed. We simply communicate this to the client. */
	struct polluxdesk_keyboard *keyboard =
		wl_container_of(listener, keyboard, modifiers);
	/*
	 * A seat can only have one keyboard, but this is a limitation of the
	 * Wayland protocol - not wlroots. We assign all connected keyboards to the
	 * same seat. You can swap out the underlying wlr_keyboard like this and
	 * wlr_seat handles this transparently.
	 */
	wlr_seat_set_keyboard(keyboard->server->seat, keyboard->wlr_keyboard);
	/* Send modifiers to the client. */
	wlr_seat_keyboard_notify_modifiers(keyboard->server->seat,
		&keyboard->wlr_keyboard->modifiers);
}

static bool handle_keybinding(struct polluxdesk_server *server, xkb_keysym_t sym) {
	/*
	 * Here we handle compositor keybindings. This is when the compositor is
	 * processing keys, rather than passing them on to the client for its own
	 * processing.
	 *
	 * This function assumes Alt is held down.
	 */
	switch (sym) {
	case XKB_KEY_Escape:
		wl_display_terminate(server->wl_display);
		break;
	case XKB_KEY_Tab:
		/* Cycle to the next toplevel; minimized windows are restored. */
		if (wl_list_length(&server->toplevels) < 2) {
			break;
		}
		struct polluxdesk_toplevel *next_toplevel =
			wl_container_of(server->toplevels.prev, next_toplevel, link);
		focus_toplevel(next_toplevel);
		break;
	default:
		return false;
	}
	return true;
}

static void keyboard_handle_key(
		struct wl_listener *listener, void *data) {
	/* This event is raised when a key is pressed or released. */
	struct polluxdesk_keyboard *keyboard =
		wl_container_of(listener, keyboard, key);
	struct polluxdesk_server *server = keyboard->server;
	struct wlr_keyboard_key_event *event = data;
	struct wlr_seat *seat = server->seat;

	/* Translate libinput keycode -> xkbcommon */
	uint32_t keycode = event->keycode + 8;
	/* Get a list of keysyms based on the keymap for this keyboard */
	const xkb_keysym_t *syms;
	int nsyms = xkb_state_key_get_syms(
			keyboard->wlr_keyboard->xkb_state, keycode, &syms);

	bool handled = false;
	uint32_t modifiers = wlr_keyboard_get_modifiers(keyboard->wlr_keyboard);
	if ((modifiers & WLR_MODIFIER_ALT) &&
			event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		/* If alt is held down and this button was _pressed_, we attempt to
		 * process it as a compositor keybinding. */
		for (int i = 0; i < nsyms; i++) {
			handled = handle_keybinding(server, syms[i]);
		}
	}

	/* Launchpad overlay dismissal: Escape closes it while it has keyboard
	 * focus (no Alt needed). */
	if (!handled && event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		struct wlr_surface *focused = seat->keyboard_state.focused_surface;
		if (focused != NULL) {
			struct wlr_xdg_toplevel *focused_toplevel =
				wlr_xdg_toplevel_try_from_wlr_surface(focused);
			if (focused_toplevel != NULL &&
					focused_toplevel->base != NULL &&
					focused_toplevel->base->data != NULL) {
				struct wlr_scene_tree *content =
					(struct wlr_scene_tree *)focused_toplevel->base->data;
				struct polluxdesk_toplevel *toplevel =
					(struct polluxdesk_toplevel *)content->node.data;
				if (toplevel != NULL && toplevel->is_overlay) {
					for (int i = 0; i < nsyms; i++) {
						if (syms[i] == XKB_KEY_Escape) {
							wlr_xdg_toplevel_send_close(focused_toplevel);
							handled = true;
							break;
						}
					}
				}
			}
		}
	}

	if (!handled) {
		/* Otherwise, we pass it along to the client. */
		wlr_seat_set_keyboard(seat, keyboard->wlr_keyboard);
		wlr_seat_keyboard_notify_key(seat, event->time_msec,
			event->keycode, event->state);
	}
}

/* The seat's capabilities are what the clients are told about; they have to
 * be recomputed when a keyboard goes away as well as when one arrives. */
static void update_seat_capabilities(struct polluxdesk_server *server) {
	uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
	if (!wl_list_empty(&server->keyboards)) {
		caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	}
	wlr_seat_set_capabilities(server->seat, caps);
}

static void keyboard_handle_destroy(struct wl_listener *listener, void *data) {
	/* This event is raised by the keyboard base wlr_input_device to signal
	 * the destruction of the wlr_keyboard. It will no longer receive events
	 * and should be destroyed.
	 */
	struct polluxdesk_keyboard *keyboard =
		wl_container_of(listener, keyboard, destroy);
	wl_list_remove(&keyboard->modifiers.link);
	wl_list_remove(&keyboard->key.link);
	wl_list_remove(&keyboard->destroy.link);
	wl_list_remove(&keyboard->link);

	/* wlroots clears the seat's keyboard when the keyboard it points at is
	 * destroyed, and nothing here used to put another one back.  That left
	 * the seat with no keyboard at all -- after which focus_toplevel()
	 * stopped sending the keyboard-enter, and every window on the desktop
	 * became unable to receive a single keystroke.  It is reachable by
	 * unplugging a USB keyboard, and it is exactly what happens when the
	 * wlrctl test hook's virtual keyboard disconnects.
	 *
	 * The focused client has to be told about the replacement: as far as it
	 * knows its keyboard vanished with the old one. */
	struct polluxdesk_server *server = keyboard->server;
	struct wlr_seat *seat = server->seat;
	struct wlr_surface *focused = seat != NULL
		? seat->keyboard_state.focused_surface : NULL;

	if (!wl_list_empty(&server->keyboards)) {
		struct polluxdesk_keyboard *next =
			wl_container_of(server->keyboards.next, next, link);
		wlr_seat_set_keyboard(seat, next->wlr_keyboard);
		if (focused != NULL) {
			wlr_seat_keyboard_notify_enter(seat, focused,
				next->wlr_keyboard->keycodes,
				next->wlr_keyboard->num_keycodes,
				&next->wlr_keyboard->modifiers);
		}
	}
	update_seat_capabilities(server);

	free(keyboard);
}

static void server_new_keyboard(struct polluxdesk_server *server,
		struct wlr_input_device *device) {
	struct wlr_keyboard *wlr_keyboard = wlr_keyboard_from_input_device(device);

	struct polluxdesk_keyboard *keyboard = calloc(1, sizeof(*keyboard));
	keyboard->server = server;
	keyboard->wlr_keyboard = wlr_keyboard;

	/* A real keyboard arrives without a keymap and needs one; this assumes
	 * the defaults (e.g. layout = "us").  A *virtual* keyboard arrives with
	 * the keymap its client sent, and that keymap is what its keycodes mean:
	 * overwriting it translates every key through the wrong layout, so the
	 * typed text comes out as something else entirely.  Only fill in what is
	 * missing. */
	if (wlr_keyboard->keymap == NULL) {
		struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
		struct xkb_keymap *keymap = xkb_keymap_new_from_names(context, NULL,
			XKB_KEYMAP_COMPILE_NO_FLAGS);

		wlr_keyboard_set_keymap(wlr_keyboard, keymap);
		xkb_keymap_unref(keymap);
		xkb_context_unref(context);
	}
	wlr_keyboard_set_repeat_info(wlr_keyboard, 25, 600);

	/* Here we set up listeners for keyboard events. */
	keyboard->modifiers.notify = keyboard_handle_modifiers;
	wl_signal_add(&wlr_keyboard->events.modifiers, &keyboard->modifiers);
	keyboard->key.notify = keyboard_handle_key;
	wl_signal_add(&wlr_keyboard->events.key, &keyboard->key);
	keyboard->destroy.notify = keyboard_handle_destroy;
	wl_signal_add(&device->events.destroy, &keyboard->destroy);

	wlr_seat_set_keyboard(server->seat, keyboard->wlr_keyboard);

	/* And add the keyboard to our list of keyboards */
	wl_list_insert(&server->keyboards, &keyboard->link);
}

static void server_new_pointer(struct polluxdesk_server *server,
		struct wlr_input_device *device) {
	/* Pointer handling is proxied through wlr_cursor, but the touchpad needs
	 * one thing configured that nothing else can do for it.
	 *
	 * This laptop's clickpad (an ELAN I2C touchpad, reported through hmt and
	 * evdev) has no separate buttons: the whole pad is the button.  Tapping
	 * it lightly is a gesture that has to be recognised in software, and
	 * libinput leaves that recognition off until someone asks for it -- so
	 * the pad only reacted to a press hard enough to click.  wlroots proxies
	 * every pointer and configures nothing itself, which is why enabling it
	 * is the compositor's job.  The tap API lives on libinput's own device
	 * object, reachable from here through the libinput backend.
	 *
	 * A device with no tap support -- a mouse, a trackpoint -- reports zero
	 * fingers and is left exactly as it was. */
	struct libinput_device *libinput_device = wlr_libinput_get_device_handle(device);
	if (libinput_device == NULL) {
		wlr_cursor_attach_input_device(server->cursor, device);
		return;
	}

	const int fingers = libinput_device_config_tap_get_finger_count(libinput_device);
	if (fingers > 0) {
		if (libinput_device_config_tap_set_enabled(libinput_device,
				LIBINPUT_CONFIG_TAP_ENABLED) == LIBINPUT_CONFIG_STATUS_SUCCESS) {
			/* Tap-and-drag comes with tapping: a tap followed by a finger
			 * held down drags, which is what everyone expects a tap to do
			 * once it works at all. */
			if (libinput_device_config_tap_set_drag_enabled(libinput_device,
					LIBINPUT_CONFIG_DRAG_ENABLED) != LIBINPUT_CONFIG_STATUS_SUCCESS) {
				wlr_log(WLR_INFO, "tap and drag not supported on %s", device->name);
			}
			wlr_log(WLR_INFO, "tap to click enabled on %s (%d fingers)",
				device->name, fingers);
		} else {
			wlr_log(WLR_ERROR, "could not enable tap to click on %s", device->name);
		}
	} else {
		wlr_log(WLR_INFO, "pointer %s: no tap support", device->name);
	}

	wlr_cursor_attach_input_device(server->cursor, device);
}

static void server_new_virtual_pointer(struct wl_listener *listener,
		void *data) {
	/* Test hook companion to POLLUX_VIRTUAL_INPUT.  A virtual pointer arrives
	 * as an input device that nobody has attached to the cursor yet, so
	 * without this its motion and button events are delivered to the
	 * protocol and then dropped: wlrctl would report success while the
	 * cursor never moves.  Attaching it makes the injected events follow the
	 * same path as a real mouse. */
	struct polluxdesk_server *server =
		wl_container_of(listener, server, new_virtual_pointer);
	struct wlr_virtual_pointer_v1_new_pointer_event *event = data;
	wlr_cursor_attach_input_device(server->cursor,
		&event->new_pointer->pointer.base);
}

static void server_new_virtual_keyboard(struct wl_listener *listener,
		void *data) {
	/* Test hook companion to POLLUX_VIRTUAL_INPUT.  wlrctl can drive the
	 * cursor without this, but not the keyboard, and a text field cannot be
	 * verified by clicking alone.  A virtual keyboard arrives as an input
	 * device like any other, so it goes through the same setup a real one
	 * does: xkb keymap, key handling, and a place in the seat. */
	struct polluxdesk_server *server =
		wl_container_of(listener, server, new_virtual_keyboard);
	struct wlr_virtual_keyboard_v1 *virtual_keyboard = data;
	server_new_keyboard(server, &virtual_keyboard->keyboard.base);
}

static void server_new_input(struct wl_listener *listener, void *data) {
	/* This event is raised by the backend when a new input device becomes
	 * available. */
	struct polluxdesk_server *server =
		wl_container_of(listener, server, new_input);
	struct wlr_input_device *device = data;
	switch (device->type) {
	case WLR_INPUT_DEVICE_KEYBOARD:
		server_new_keyboard(server, device);
		break;
	case WLR_INPUT_DEVICE_POINTER:
		server_new_pointer(server, device);
		break;
	default:
		break;
	}
	/* We need to let the wlr_seat know what our capabilities are, which is
	 * communiciated to the client. In TinyWL we always have a cursor, even if
	 * there are no pointer devices, so we always include that capability. */
	update_seat_capabilities(server);
}

static void seat_request_cursor(struct wl_listener *listener, void *data) {
	struct polluxdesk_server *server = wl_container_of(
			listener, server, request_cursor);
	/* This event is raised by the seat when a client provides a cursor image */
	struct wlr_seat_pointer_request_set_cursor_event *event = data;
	struct wlr_seat_client *focused_client =
		server->seat->pointer_state.focused_client;
	/* This can be sent by any client, so we check to make sure this one is
	 * actually has pointer focus first. */
	if (focused_client == event->seat_client) {
		/* Once we've vetted the client, we can tell the cursor to use the
		 * provided surface as the cursor image. It will set the hardware cursor
		 * on the output that it's currently on and continue to do so as the
		 * cursor moves between outputs. */
		wlr_cursor_set_surface(server->cursor, event->surface,
				event->hotspot_x, event->hotspot_y);
	}
}

static void seat_pointer_focus_change(struct wl_listener *listener, void *data) {
	struct polluxdesk_server *server = wl_container_of(
			listener, server, pointer_focus_change);
	/* This event is raised when the pointer focus is changed, including when the
	 * client is closed. We set the cursor image to its default if target surface
	 * is NULL */
	struct wlr_seat_pointer_focus_change_event *event = data;
	/* Keep a visible default cursor whenever focus changes; clients can
	 * replace it afterwards via request_set_cursor. */
	wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");
}

static void seat_request_set_selection(struct wl_listener *listener, void *data) {
	/* This event is raised by the seat when a client wants to set the selection,
	 * usually when the user copies something. wlroots allows compositors to
	 * ignore such requests if they so choose, but in polluxdesk we always honor
	 */
	struct polluxdesk_server *server = wl_container_of(
			listener, server, request_set_selection);
	struct wlr_seat_request_set_selection_event *event = data;
	wlr_seat_set_selection(server->seat, event->source, event->serial);
}

static struct polluxdesk_toplevel *scene_node_get_toplevel(struct wlr_scene_node *node) {
	/* Window chrome nodes and the window tree itself all carry the toplevel
	 * pointer in node.data; popups / background stripes don't. */
	while (node != NULL && node->data == NULL) {
		node = &node->parent->node;
	}
	return node != NULL ? node->data : NULL;
}

static struct polluxdesk_toplevel *desktop_toplevel_at(
		struct polluxdesk_server *server, double lx, double ly,
		struct wlr_surface **surface, double *sx, double *sy) {
	/* This returns the topmost surface node in the scene at the given layout
	 * coordinates. Server-drawn titlebar rects are intentionally NOT surfaces:
	 * the compositor owns them, the client never receives pointer events for
	 * them, and the app content stays a separate subtree. */
	struct wlr_scene_node *node = wlr_scene_node_at(
		&server->scene->tree.node, lx, ly, sx, sy);
	if (node == NULL || node->type != WLR_SCENE_NODE_BUFFER) {
		return NULL;
	}
	struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);
	struct wlr_scene_surface *scene_surface =
		wlr_scene_surface_try_from_buffer(scene_buffer);
	if (!scene_surface) {
		return NULL;
	}

	*surface = scene_surface->surface;
	return scene_node_get_toplevel(node);
}

static int titlebar_button_at(struct polluxdesk_toplevel *toplevel,
		struct wlr_scene_node *node) {
	if (node == NULL ||
		(node->type != WLR_SCENE_NODE_RECT &&
		 node->type != WLR_SCENE_NODE_BUFFER)) {
		return -1;
	}
	for (int i = 0; i < 3; ++i) {
		if (node == &toplevel->titlebar_buttons[i]->node) {
			return i;
		}
	}
	return -1;
}

static void reset_cursor_mode(struct polluxdesk_server *server) {
	/* Reset the cursor mode to passthrough. */
	server->cursor_mode = POLLUXDESK_CURSOR_PASSTHROUGH;
	server->grabbed_toplevel = NULL;
}

static void process_cursor_move(struct polluxdesk_server *server) {
	/* Move the grabbed toplevel to the new position. */
	struct polluxdesk_toplevel *toplevel = server->grabbed_toplevel;
	wlr_scene_node_set_position(&toplevel->scene_tree->node,
		server->cursor->x - server->grab_x,
		server->cursor->y - server->grab_y);
}

static void process_cursor_resize(struct polluxdesk_server *server) {
	/*
	 * Resizing the grabbed toplevel can be a little bit complicated, because we
	 * could be resizing from any corner or edge. This not only resizes the
	 * toplevel on one or two axes, but can also move the toplevel if you resize
	 * from the top or left edges (or top-left corner).
	 *
	 * Note that some shortcuts are taken here. In a more fleshed-out
	 * compositor, you'd wait for the client to prepare a buffer at the new
	 * size, then commit any movement that was prepared.
	 */
	struct polluxdesk_toplevel *toplevel = server->grabbed_toplevel;
	double border_x = server->cursor->x - server->grab_x;
	double border_y = server->cursor->y - server->grab_y;
	int new_left = server->grab_geobox.x;
	int new_right = server->grab_geobox.x + server->grab_geobox.width;
	int new_top = server->grab_geobox.y;
	int new_bottom = server->grab_geobox.y + server->grab_geobox.height;

	if (server->resize_edges & WLR_EDGE_TOP) {
		new_top = border_y;
		if (new_top >= new_bottom) {
			new_top = new_bottom - 1;
		}
	} else if (server->resize_edges & WLR_EDGE_BOTTOM) {
		new_bottom = border_y;
		if (new_bottom <= new_top) {
			new_bottom = new_top + 1;
		}
	}
	if (server->resize_edges & WLR_EDGE_LEFT) {
		new_left = border_x;
		if (new_left >= new_right) {
			new_left = new_right - 1;
		}
	} else if (server->resize_edges & WLR_EDGE_RIGHT) {
		new_right = border_x;
		if (new_right <= new_left) {
			new_right = new_left + 1;
		}
	}

	int titlebar = toplevel_titlebar_height(toplevel);
	int new_window_width = new_right - new_left;
	int new_window_height = new_bottom - new_top;
	int new_content_width = new_window_width;
	int new_content_height = new_window_height - titlebar;
	if (new_content_width < 1) {
		new_content_width = 1;
	}
	if (new_content_height < 1) {
		new_content_height = 1;
	}
	wlr_scene_node_set_position(&toplevel->scene_tree->node,
		new_left, new_top);
	wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel,
		new_content_width, new_content_height);
}

static void process_cursor_motion(struct polluxdesk_server *server, uint32_t time) {
	/* If the mode is non-passthrough, delegate to those functions. */
	if (server->cursor_mode == POLLUXDESK_CURSOR_MOVE) {
		process_cursor_move(server);
		return;
	} else if (server->cursor_mode == POLLUXDESK_CURSOR_RESIZE) {
		process_cursor_resize(server);
		return;
	}

	/* Otherwise, find the toplevel under the pointer and send the event along. */
	double sx, sy;
	struct wlr_seat *seat = server->seat;
	struct wlr_surface *surface = NULL;
	struct polluxdesk_toplevel *toplevel = desktop_toplevel_at(server,
			server->cursor->x, server->cursor->y, &surface, &sx, &sy);

	/* Server-drawn borders: show a resize cursor and keep the border events
	 * inside the compositor so clients only see content-area pointer input. */
	struct wlr_scene_node *node = wlr_scene_node_at(
			&server->scene->tree.node, server->cursor->x,
			server->cursor->y, &sx, &sy);
	struct polluxdesk_toplevel *hover_toplevel = node ?
		scene_node_get_toplevel(node) : NULL;
	uint32_t edges = hover_toplevel ?
		toplevel_resize_edges_at(hover_toplevel,
			server->cursor->x, server->cursor->y) : 0;
	if (edges != 0) {
		/* Do NOT clear pointer focus here: the focus-change listener resets
		 * the cursor to "default", which would immediately hide the resize
		 * cursor. Leaving focus untouched still lets border clicks start an
		 * interactive resize (server_cursor_button owns those presses). */
		set_resize_cursor(server, edges);
		return;
	}

	/* Off the border again: put the arrow back. Staying over the same surface
	 * means no pointer focus change, so the focus-change listener above never
	 * fires and the resize cursor would otherwise be left up -- an arrow
	 * pointing at both window edges with the pointer nowhere near either. */
	if (server->resize_cursor_shown) {
		server->resize_cursor_shown = false;
		wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");
	}

	if (!toplevel) {
		/* If there's no toplevel under the cursor, set the cursor image to a
		 * default. This is what makes the cursor image appear when you move it
		 * around the screen, not over any toplevels. */
		wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");
	}
	if (surface) {
		/*
		 * Send pointer enter and motion events.
		 *
		 * The enter event gives the surface "pointer focus", which is distinct
		 * from keyboard focus. You get pointer focus by moving the pointer over
		 * a window.
		 *
		 * Note that wlroots will avoid sending duplicate enter/motion events if
		 * the surface has already has pointer focus or if the client is already
		 * aware of the coordinates passed.
		 */
		wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
		wlr_seat_pointer_notify_motion(seat, time, sx, sy);
	} else {
		/* Clear pointer focus so future button events and such are not sent to
		 * the last client to have the cursor over it. */
		wlr_seat_pointer_clear_focus(seat);
	}
}

static void server_cursor_motion(struct wl_listener *listener, void *data) {
	/* This event is forwarded by the cursor when a pointer emits a _relative_
	 * pointer motion event (i.e. a delta) */
	struct polluxdesk_server *server =
		wl_container_of(listener, server, cursor_motion);
	struct wlr_pointer_motion_event *event = data;
	/* The cursor doesn't move unless we tell it to. The cursor automatically
	 * handles constraining the motion to the output layout, as well as any
	 * special configuration applied for the specific input device which
	 * generated the event. You can pass NULL for the device if you want to move
	 * the cursor around without any input. */
	wlr_cursor_move(server->cursor, &event->pointer->base,
			event->delta_x, event->delta_y);
	process_cursor_motion(server, event->time_msec);
}

static void server_cursor_motion_absolute(
		struct wl_listener *listener, void *data) {
	/* This event is forwarded by the cursor when a pointer emits an _absolute_
	 * motion event, from 0..1 on each axis. This happens, for example, when
	 * wlroots is running under a Wayland window rather than KMS+DRM, and you
	 * move the mouse over the window. You could enter the window from any edge,
	 * so we have to warp the mouse there. There is also some hardware which
	 * emits these events. */
	struct polluxdesk_server *server =
		wl_container_of(listener, server, cursor_motion_absolute);
	struct wlr_pointer_motion_absolute_event *event = data;
	wlr_cursor_warp_absolute(server->cursor, &event->pointer->base, event->x,
		event->y);
	process_cursor_motion(server, event->time_msec);
}

static void server_cursor_button(struct wl_listener *listener, void *data) {
	/* This event is forwarded by the cursor when a pointer emits a button
	 * event. Button presses on the server-drawn titlebar stay in the
	 * compositor: titlebar = move, traffic lights = close/minimize/maximize. */
	struct polluxdesk_server *server =
		wl_container_of(listener, server, cursor_button);
	struct wlr_pointer_button_event *event = data;

	if (event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
		wlr_seat_pointer_notify_button(server->seat,
			event->time_msec, event->button, event->state);
		reset_cursor_mode(server);
		return;
	}

	if (event->button != BTN_LEFT) {
		wlr_seat_pointer_notify_button(server->seat,
			event->time_msec, event->button, event->state);
		return;
	}

	double sx, sy;
	struct wlr_scene_node *node = wlr_scene_node_at(
		&server->scene->tree.node, server->cursor->x, server->cursor->y, &sx, &sy);
	struct polluxdesk_toplevel *toplevel = scene_node_get_toplevel(node);
	if (toplevel == NULL) {
		wlr_seat_pointer_notify_button(server->seat,
			event->time_msec, event->button, event->state);
		return;
	}

	/* Server-side four-edge / four-corner resize: pressing on the visible
	 * border starts an interactive resize instead of passing the click to
	 * the client. */
	uint32_t resize_edges = toplevel_resize_edges_at(toplevel,
		server->cursor->x, server->cursor->y);
	if (resize_edges != 0) {
		focus_toplevel(toplevel);
		begin_interactive(toplevel, POLLUXDESK_CURSOR_RESIZE, resize_edges);
		return;
	}

	/* The soft shadow belongs to the compositor chrome, not to the
	 * window: clicks on it must not focus or move the window. Same for
	 * the rounded-corner wallpaper masks. */
	bool is_corner = false;
	for (int i = 0; i < 4; ++i) {
		if (node == &toplevel->corners[i]->node) {
			is_corner = true;
			break;
		}
	}
	if (is_corner || node == &toplevel->shadow->node) {
		wlr_seat_pointer_notify_button(server->seat,
			event->time_msec, event->button, event->state);
		return;
	}
	/* A click on the desktop wallpaper is outside the overlay. The desktop
	 * shell deliberately never takes keyboard focus, so focus_toplevel() alone
	 * cannot trigger the normal overlay dismissal path. */
	if (toplevel->is_desktop) {
		struct polluxdesk_toplevel *overlay = server_get_overlay(server);
		if (overlay != NULL) {
			wlr_xdg_toplevel_send_close(overlay->xdg_toplevel);
		}
	}

	focus_toplevel(toplevel);

	if (node == &toplevel->titlebar->node) {
		/* Drag the window by its compositor titlebar, macOS style. */
		begin_interactive(toplevel, POLLUXDESK_CURSOR_MOVE, 0);
		return;
	}

	int button = titlebar_button_at(toplevel, node);
	wlr_log(WLR_INFO, "titlebar click type=%d button=%d title=%s at %.0f,%.0f",
		node->type, button,
		toplevel->xdg_toplevel->title ? toplevel->xdg_toplevel->title : "(null)",
		server->cursor->x, server->cursor->y);
	if (button == POLLUXDESK_BUTTON_CLOSE) {
		wlr_log(WLR_INFO, "sending xdg close to %s",
			toplevel->xdg_toplevel->title ? toplevel->xdg_toplevel->title : "(null)");
		wlr_xdg_toplevel_send_close(toplevel->xdg_toplevel);
		return;
	} else if (button == POLLUXDESK_BUTTON_MINIMIZE) {
		minimize_toplevel(toplevel);
		return;
	} else if (button == POLLUXDESK_BUTTON_MAXIMIZE) {
		toggle_maximize(toplevel);
		return;
	}

	/* App content: pass the event through to the client surface. */
	struct wlr_surface *surface = NULL;
	desktop_toplevel_at(server, server->cursor->x, server->cursor->y,
		&surface, &sx, &sy);
	wlr_seat_pointer_notify_button(server->seat,
		event->time_msec, event->button, event->state);
}

static void server_cursor_axis(struct wl_listener *listener, void *data) {
	/* This event is forwarded by the cursor when a pointer emits an axis event,
	 * for example when you move the scroll wheel. */
	struct polluxdesk_server *server =
		wl_container_of(listener, server, cursor_axis);
	struct wlr_pointer_axis_event *event = data;
	/* Notify the client with pointer focus of the axis event. */
	wlr_seat_pointer_notify_axis(server->seat,
			event->time_msec, event->orientation, event->delta,
			event->delta_discrete, event->source, event->relative_direction);
}

static void server_cursor_frame(struct wl_listener *listener, void *data) {
	/* This event is forwarded by the cursor when a pointer emits an frame
	 * event. Frame events are sent after regular pointer events to group
	 * multiple events together. For instance, two axis events may happen at the
	 * same time, in which case a frame event won't be sent in between. */
	struct polluxdesk_server *server =
		wl_container_of(listener, server, cursor_frame);
	/* Notify the client with pointer focus of the frame event. */
	wlr_seat_pointer_notify_frame(server->seat);
}

/* Reload the small user configuration written by polluxdesk_settings. This is
 * deliberately file-based: the settings client remains a normal dui app and
 * does not need a private Wayland protocol just to change the wallpaper. */
static void reload_user_settings(struct polluxdesk_server *server) {
	const char *home = getenv("HOME");
	if (home == NULL) {
		return;
	}
	char path[1024];
	snprintf(path, sizeof(path), "%s/.config/polluxdesk/settings.conf", home);
	FILE *file = fopen(path, "r");
	if (file == NULL) {
		return;
	}
	char wallpaper[32] = "";
	int wanted_width = 0;
	int wanted_height = 0;
	char line[128];
	while (fgets(line, sizeof(line), file) != NULL) {
		if (strncmp(line, "wallpaper=", 10) == 0) {
			sscanf(line + 10, "%31s", wallpaper);
		} else if (strncmp(line, "resolution=", 11) == 0) {
			sscanf(line + 11, "%dx%d", &wanted_width, &wanted_height);
		}
	}
	fclose(file);

	if (strcmp(wallpaper, "purple") == 0) {
		gBackgroundTop[0] = 0.48f; gBackgroundTop[1] = 0.35f; gBackgroundTop[2] = 0.78f;
		gBackgroundBottom[0] = 0.92f; gBackgroundBottom[1] = 0.70f; gBackgroundBottom[2] = 0.88f;
	} else if (strcmp(wallpaper, "dark") == 0) {
		gBackgroundTop[0] = 0.06f; gBackgroundTop[1] = 0.08f; gBackgroundTop[2] = 0.14f;
		gBackgroundBottom[0] = 0.20f; gBackgroundBottom[1] = 0.23f; gBackgroundBottom[2] = 0.32f;
	} else if (strcmp(wallpaper, "green") == 0) {
		gBackgroundTop[0] = 0.20f; gBackgroundTop[1] = 0.55f; gBackgroundTop[2] = 0.48f;
		gBackgroundBottom[0] = 0.78f; gBackgroundBottom[1] = 0.91f; gBackgroundBottom[2] = 0.70f;
	} else {
		gBackgroundTop[0] = 0.44f; gBackgroundTop[1] = 0.66f; gBackgroundTop[2] = 0.97f;
		gBackgroundBottom[0] = 0.91f; gBackgroundBottom[1] = 0.93f; gBackgroundBottom[2] = 0.98f;
	}

	struct polluxdesk_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		output->background_width = 0;
		if (wanted_width <= 0 || wanted_height <= 0) {
			continue;
		}
		struct wlr_output_mode *mode;
		wl_list_for_each(mode, &output->wlr_output->modes, link) {
			if (mode->width != wanted_width || mode->height != wanted_height) {
				continue;
			}
			struct wlr_output_state state;
			wlr_output_state_init(&state);
			wlr_output_state_set_mode(&state, mode);
			wlr_output_commit_state(output->wlr_output, &state);
			wlr_output_state_finish(&state);
			break;
		}
	}
}

static void output_frame(struct wl_listener *listener, void *data) {
	/* This function is called every time an output is ready to display a frame,
	 * generally at the output's refresh rate (e.g. 60Hz). */
	struct polluxdesk_output *output = wl_container_of(listener, output, frame);
	struct wlr_scene *scene = output->server->scene;
	/* A shell that died mid-grab would otherwise leave the window stranded
	 * on screen forever, so the wait is bounded. */
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	struct polluxdesk_toplevel *waiting;
	wl_list_for_each(waiting, &output->server->toplevels, link) {
		if (!waiting->pending_thumb) {
			continue;
		}
		if (now.tv_sec - waiting->thumb_since.tv_sec >= 2) {
			toplevel_finish_thumb(waiting);
			write_window_state(output->server);
		}
	}

	if (g_settings_reload) {
		g_settings_reload = 0;
		reload_user_settings(output->server);
	}

	struct wlr_scene_output *scene_output = wlr_scene_get_scene_output(
		scene, output->wlr_output);

	/* Keep the Big Sur gradient wallpaper in sync with output resizes. */
	update_background(output);

	/* Render the scene only when something actually changed, and send
	 * frame_done only for rendered frames. Sending frame_done on every vblank
	 * made the dui shell repaint continuously at 100% CPU. */
	if (wlr_scene_output_needs_frame(scene_output)) {
		wlr_scene_output_commit(scene_output, NULL);
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		wlr_scene_output_send_frame_done(scene_output, &now);
	}
}

static void output_request_state(struct wl_listener *listener, void *data) {
	/* This function is called when the backend requests a new state for
	 * the output. For example, Wayland and X11 backends request a new mode
	 * when the output window is resized. */
	struct polluxdesk_output *output = wl_container_of(listener, output, request_state);
	const struct wlr_output_event_request_state *event = data;
	wlr_output_commit_state(output->wlr_output, event->state);
	update_background(output);
}

static void output_destroy(struct wl_listener *listener, void *data) {
	struct polluxdesk_output *output = wl_container_of(listener, output, destroy);

	wl_list_remove(&output->frame.link);
	wl_list_remove(&output->request_state.link);
	wl_list_remove(&output->destroy.link);
	wl_list_remove(&output->link);
	if (output->background_tree != NULL) {
		wlr_scene_node_destroy(&output->background_tree->node);
	}
	free(output);
}

static void server_new_output(struct wl_listener *listener, void *data) {
	/* This event is raised by the backend when a new output (aka a display or
	 * monitor) becomes available. */
	struct polluxdesk_server *server =
		wl_container_of(listener, server, new_output);
	struct wlr_output *wlr_output = data;

	/* Configures the output created by the backend to use our allocator
	 * and our renderer. Must be done once, before committing the output */
	wlr_output_init_render(wlr_output, server->allocator, server->renderer);

	/* The output may be disabled, switch it on. */
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);

	/* Some backends don't have modes. DRM+KMS does, and we need to set a mode
	 * before we can use the output. The mode is a tuple of (width, height,
	 * refresh rate), and each monitor supports only a specific set of modes. We
	 * just pick the monitor's preferred mode, a more sophisticated compositor
	 * would let the user configure it. */
	struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
	if (mode != NULL) {
		wlr_output_state_set_mode(&state, mode);
	}

	/* Atomically applies the new output state. */
	wlr_output_commit_state(wlr_output, &state);
	wlr_output_state_finish(&state);

	/* Allocates and configures our state for this output */
	struct polluxdesk_output *output = calloc(1, sizeof(*output));
	output->wlr_output = wlr_output;
	output->server = server;

	/* Sets up a listener for the frame event. */
	output->frame.notify = output_frame;
	wl_signal_add(&wlr_output->events.frame, &output->frame);

	/* Sets up a listener for the state request event. */
	output->request_state.notify = output_request_state;
	wl_signal_add(&wlr_output->events.request_state, &output->request_state);

	/* Sets up a listener for the destroy event. */
	output->destroy.notify = output_destroy;
	wl_signal_add(&wlr_output->events.destroy, &output->destroy);

	wl_list_insert(&server->outputs, &output->link);

	/* Adds this to the output layout. The add_auto function arranges outputs
	 * from left-to-right in the order they appear. A more sophisticated
	 * compositor would let the user configure the arrangement of outputs in the
	 * layout.
	 *
	 * The output layout utility automatically adds a wl_output global to the
	 * display, which Wayland clients can see to find out information about the
	 * output (such as DPI, scale factor, manufacturer, etc).
	 */
	struct wlr_output_layout_output *l_output = wlr_output_layout_add_auto(server->output_layout,
		wlr_output);
	struct wlr_scene_output *scene_output = wlr_scene_output_create(server->scene, wlr_output);
	wlr_scene_output_layout_add_output(server->scene_layout, l_output, scene_output);

	/* Default mouse position: the middle of the primary output, so the
	 * greeter/desktop never starts with the cursor parked at (0,0). */
	if (!server->cursor_warped && wlr_output->width > 0 && wlr_output->height > 0) {
		struct wlr_box box;
		wlr_output_layout_get_box(server->output_layout, wlr_output, &box);
		wlr_cursor_warp(server->cursor, NULL,
			box.x + box.width / 2.0, box.y + box.height / 2.0);
		server->cursor_warped = true;
	}

	/* Big Sur gradient wallpaper drawn by the compositor (MIT-licensed
	 * wlroots scene rects), independent from the dui shell surface. */
	output->background_tree = wlr_scene_tree_create(&server->scene->tree);
	update_background(output);
}

static void xdg_toplevel_set_title(struct wl_listener *listener, void *data) {
	/* The dui Wayland client sets its title slightly after creation. Detect
	 * the desktop/login shell as soon as the title is known. */
	(void)data;
	struct polluxdesk_toplevel *toplevel = wl_container_of(listener, toplevel, set_title);
	toplevel_update_shell_flags(toplevel);
	const char *title = toplevel->xdg_toplevel->title;
	bool menu_open = toplevel->is_desktop && title != NULL &&
		strcmp(title, "PolluxOS Desktop (menu)") == 0;
	server_update_desktop_menu_layer(toplevel->server, toplevel, menu_open);

	/* The same one-way channel carries restore requests from the dock's
	 * minimized-window shelf: the shell retitles itself to
	 * "PolluxOS Desktop (restore:<id>)" and the compositor brings that window
	 * back. The shell drops the marker on its next timer tick, so this runs
	 * once per request rather than once per title change. */
	if (toplevel->is_desktop && title != NULL &&
			strncmp(title, "PolluxOS Desktop (restore:", 26) == 0) {
		restore_minimized(toplevel->server,
			strtoul(title + 26, NULL, 10));
	}
	/* The shell has the thumbnail; the window can come down now. */
	if (toplevel->is_desktop && title != NULL &&
			strncmp(title, "PolluxOS Desktop (thumb-ready:", 30) == 0) {
		finish_pending_thumb(toplevel->server,
			strtoul(title + 30, NULL, 10));
	}
	if (toplevel->is_borderless && toplevel->xdg_toplevel->base->initialized) {
		struct wlr_box output_box;
		server_get_output_box(toplevel->server, &output_box);
		wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel,
			output_box.width, output_box.height);
		wlr_scene_node_set_position(&toplevel->scene_tree->node,
			output_box.x, output_box.y);
	}
	arrange_toplevel(toplevel);
	write_window_state(toplevel->server);
}

static void xdg_toplevel_map(struct wl_listener *listener, void *data) {
	/* Called when the surface is mapped, or ready to display on-screen. */
	struct polluxdesk_toplevel *toplevel = wl_container_of(listener, toplevel, map);

	/* The dui shell is the desktop background: keep it at the bottom and never
	 * focus it. Regular apps cascade and get keyboard focus. */
	if (toplevel->is_desktop) {
		struct wlr_box output_box;
		server_get_output_box(toplevel->server, &output_box);
		wlr_scene_node_set_position(&toplevel->scene_tree->node,
			output_box.x, output_box.y);
	} else if (toplevel->is_borderless) {
		struct wlr_box output_box;
		server_get_output_box(toplevel->server, &output_box);
		wlr_scene_node_set_position(&toplevel->scene_tree->node,
			output_box.x, output_box.y);
	} else {
		int cascade = (int)(wl_list_length(&toplevel->server->toplevels) % 6);
		wlr_scene_node_set_position(&toplevel->scene_tree->node,
			80 + cascade * 40, 86 + cascade * 40);
		/* New app windows open on top of everything (macOS behavior),
		 * not buried under already-running windows. */
		wlr_scene_node_raise_to_top(&toplevel->scene_tree->node);
	}

	if (wl_list_empty(&toplevel->link)) {
		wl_list_insert(&toplevel->server->toplevels, &toplevel->link);
	}
	arrange_toplevel(toplevel);
	write_window_state(toplevel->server);
	focus_toplevel(toplevel);
}

static void xdg_toplevel_unmap(struct wl_listener *listener, void *data) {
	/* Called when the surface is unmapped, and should no longer be shown. */
	struct polluxdesk_toplevel *toplevel = wl_container_of(listener, toplevel, unmap);

	/* When the desktop shell exits (logout/restart), close every app window
	 * and overlay so they never leak onto the greeter or the next session. */
	if (toplevel->is_desktop) {
		struct polluxdesk_toplevel *app;
		struct polluxdesk_toplevel *tmp;
		wl_list_for_each_safe(app, tmp, &toplevel->server->toplevels, link) {
			if (app == toplevel || app->is_desktop ||
					(app->is_borderless && !app->is_overlay)) {
				continue;
			}
			wlr_xdg_toplevel_send_close(app->xdg_toplevel);
		}
	}

	/* Reset the cursor mode if the grabbed toplevel was unmapped. */
	if (toplevel == toplevel->server->grabbed_toplevel) {
		reset_cursor_mode(toplevel->server);
	}

	if (!wl_list_empty(&toplevel->link)) {
		wl_list_remove(&toplevel->link);
	}
	/* An xdg toplevel may be destroyed without ever mapping, or be unmapped
	 * more than once while the client tears down. Keep the list node in the
	 * canonical empty state so destroy/remap paths never unlink a zeroed node. */
	wl_list_init(&toplevel->link);
	write_window_state(toplevel->server);
}

static void xdg_toplevel_commit(struct wl_listener *listener, void *data) {
	/* Called when a new surface state is committed. */
	struct polluxdesk_toplevel *toplevel = wl_container_of(listener, toplevel, commit);

	toplevel_update_shell_flags(toplevel);

	if (toplevel->xdg_toplevel->base->initial_commit) {
		/* When an xdg_surface performs an initial commit, the compositor must
		 * reply with a configure so the client can map the surface. The dui
		 * shells are configured to the output size; normal apps and the
		 * Launchpad overlay get 0x0 so the client chooses its own dimensions. */
		if (toplevel->is_desktop ||
				(toplevel->is_borderless && !toplevel->is_overlay)) {
			struct wlr_box output_box;
			server_get_output_box(toplevel->server, &output_box);
			wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel,
				output_box.width, output_box.height);
		} else {
			wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, 0, 0);
		}
		return;
	}

	toplevel_update_shell_flags(toplevel);

	if (toplevel->is_desktop && !toplevel->alpha_debugged) {
		struct wlr_surface *surface = toplevel->xdg_toplevel->base->surface;
		struct wlr_buffer *buffer = surface->current.buffer;
		if (buffer != NULL) {
			void *data = NULL;
			uint32_t format = 0;
			size_t stride = 0;
			if (wlr_buffer_begin_data_ptr_access(buffer,
					WLR_BUFFER_DATA_PTR_ACCESS_READ, &data, &format, &stride)) {
				int nonzero = 0;
				int opaque = 0;
				int total = surface->current.width * surface->current.height;
				uint32_t *pixels = data;
				for (int i = 0; i < total; ++i) {
					uint8_t a = (pixels[i] >> 24) & 0xFF;
					if (a != 0) nonzero++;
					if (a == 0xFF) opaque++;
				}
				wlr_log(WLR_INFO, "PolluxOS Desktop buffer %dx%d fmt=0x%x "
					"nonzero_alpha=%d opaque=%d first=0x%08x",
					surface->current.width, surface->current.height, format,
					nonzero, opaque, pixels[0]);
				wlr_buffer_end_data_ptr_access(buffer);
			}
			toplevel->alpha_debugged = true;
		}
	}

	if (toplevel->decoration != NULL && toplevel->xdg_toplevel->base->initialized) {
		wlr_xdg_toplevel_decoration_v1_set_mode(toplevel->decoration,
			toplevel->client_side_decorated
				? WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE
				: WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
	}
	arrange_toplevel(toplevel);
}

static void xdg_toplevel_destroy(struct wl_listener *listener, void *data) {
	/* Called when the xdg_toplevel is destroyed. */
	struct polluxdesk_toplevel *toplevel = wl_container_of(listener, toplevel, destroy);

	wl_list_remove(&toplevel->map.link);
	wl_list_remove(&toplevel->unmap.link);
	wl_list_remove(&toplevel->commit.link);
	wl_list_remove(&toplevel->set_title.link);
	wl_list_remove(&toplevel->destroy.link);
	wl_list_remove(&toplevel->request_move.link);
	wl_list_remove(&toplevel->request_resize.link);
	wl_list_remove(&toplevel->request_maximize.link);
	wl_list_remove(&toplevel->request_fullscreen.link);
	if (!wl_list_empty(&toplevel->link)) {
		wl_list_remove(&toplevel->link);
	}
	wl_list_init(&toplevel->link);
	wlr_scene_node_destroy(&toplevel->scene_tree->node);
	/* After the node has left the list, so the walk above cannot reach it,
	 * and before the free. */
	write_window_state(toplevel->server);
	free(toplevel);
}

static void begin_interactive(struct polluxdesk_toplevel *toplevel,
		enum polluxdesk_cursor_mode mode, uint32_t edges) {
	/* This function sets up an interactive move or resize operation, where the
	 * compositor stops propagating pointer events to clients and instead
	 * consumes them itself, to move or resize windows. */
	struct polluxdesk_server *server = toplevel->server;

	server->grabbed_toplevel = toplevel;
	server->cursor_mode = mode;

	if (mode == POLLUXDESK_CURSOR_MOVE) {
		server->grab_x = server->cursor->x - toplevel->scene_tree->node.x;
		server->grab_y = server->cursor->y - toplevel->scene_tree->node.y;
	} else {
		struct wlr_box vis;
		toplevel_visible_box(toplevel, &vis);

		double border_x = vis.x +
			((edges & WLR_EDGE_RIGHT) ? vis.width : 0);
		double border_y = vis.y +
			((edges & WLR_EDGE_BOTTOM) ? vis.height : 0);
		server->grab_x = server->cursor->x - border_x;
		server->grab_y = server->cursor->y - border_y;

		server->grab_geobox = vis;
		server->resize_edges = edges;
	}
}

static void xdg_toplevel_request_move(
		struct wl_listener *listener, void *data) {
	/* This event is raised when a client would like to begin an interactive
	 * move, typically because the user clicked on their client-side
	 * decorations. Note that a more sophisticated compositor should check the
	 * provided serial against a list of button press serials sent to this
	 * client, to prevent the client from requesting this whenever they want. */
	struct polluxdesk_toplevel *toplevel = wl_container_of(listener, toplevel, request_move);
	begin_interactive(toplevel, POLLUXDESK_CURSOR_MOVE, 0);
}

static void xdg_toplevel_request_resize(
		struct wl_listener *listener, void *data) {
	/* This event is raised when a client would like to begin an interactive
	 * resize, typically because the user clicked on their client-side
	 * decorations. Note that a more sophisticated compositor should check the
	 * provided serial against a list of button press serials sent to this
	 * client, to prevent the client from requesting this whenever they want. */
	struct wlr_xdg_toplevel_resize_event *event = data;
	struct polluxdesk_toplevel *toplevel = wl_container_of(listener, toplevel, request_resize);
	begin_interactive(toplevel, POLLUXDESK_CURSOR_RESIZE, event->edges);
}

static void xdg_toplevel_request_maximize(
		struct wl_listener *listener, void *data) {
	/* This event is raised when a client would like to maximize itself,
	 * typically because the user clicked on the maximize button on client-side
	 * decorations. polluxdesk doesn't support maximization, but to conform to
	 * xdg-shell protocol we still must send a configure.
	 * wlr_xdg_surface_schedule_configure() is used to send an empty reply.
	 * However, if the request was sent before an initial commit, we don't do
	 * anything and let the client finish the initial surface setup. */
	struct polluxdesk_toplevel *toplevel =
		wl_container_of(listener, toplevel, request_maximize);
	if (toplevel->xdg_toplevel->base->initialized) {
		wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base);
	}
}

static void xdg_toplevel_request_fullscreen(
		struct wl_listener *listener, void *data) {
	/* Just as with request_maximize, we must send a configure here. */
	struct polluxdesk_toplevel *toplevel =
		wl_container_of(listener, toplevel, request_fullscreen);
	if (toplevel->xdg_toplevel->base->initialized) {
		wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base);
	}
}

static void server_new_xdg_toplevel(struct wl_listener *listener, void *data) {
	/* This event is raised when a client creates a new toplevel (application window). */
	struct polluxdesk_server *server = wl_container_of(listener, server, new_xdg_toplevel);
	struct wlr_xdg_toplevel *xdg_toplevel = data;

	/* Allocate a polluxdesk_toplevel for this surface. The window scene tree
	 * has two separated children: the compositor-drawn titlebar (server side,
	 * like macOS/Windows) and the client-drawn content surface. */
	struct polluxdesk_toplevel *toplevel = calloc(1, sizeof(*toplevel));
	wl_list_init(&toplevel->link);
	toplevel->server = server;
	toplevel->xdg_toplevel = xdg_toplevel;
	toplevel_update_shell_flags(toplevel);

	toplevel->scene_tree = wlr_scene_tree_create(&server->scene->tree);
	toplevel->scene_tree->node.data = toplevel;

	/* Compositor-owned soft shadow, painted before chrome and content.
	 * The actual pixmap is generated on the first arrange_toplevel(). */
	toplevel->shadow = wlr_scene_buffer_create(toplevel->scene_tree, NULL);
	toplevel->shadow->node.data = toplevel;
	wlr_scene_node_set_enabled(&toplevel->shadow->node, false);

	toplevel->content_tree =
		wlr_scene_xdg_surface_create(toplevel->scene_tree, xdg_toplevel->base);
	toplevel->content_tree->node.data = toplevel;
	xdg_toplevel->base->data = toplevel->content_tree;

	toplevel->titlebar = wlr_scene_rect_create(toplevel->scene_tree, 0, 0,
		kTitlebarColor);
	toplevel->titlebar->node.data = toplevel;
	for (int i = 0; i < 3; ++i) {
		/* Circular macOS traffic lights: small pre-rendered disc textures. */
		toplevel->titlebar_buttons[i] =
			wlr_scene_buffer_create(toplevel->scene_tree, NULL);
		struct wlr_buffer *buffer =
			circle_button_buffer_create(kButtonColors[i], POLLUXDESK_BUTTON_SIZE);
		if (buffer != NULL) {
			wlr_scene_buffer_set_buffer(toplevel->titlebar_buttons[i], buffer);
			wlr_buffer_drop(buffer);
		}
		toplevel->titlebar_buttons[i]->node.data = toplevel;
	}

	/* Rounded-corner masks; painted last so they cover the titlebar and the
	 * client content edges. The pixmaps are generated in arrange_toplevel(). */
	for (int i = 0; i < 4; ++i) {
		toplevel->corners[i] = wlr_scene_buffer_create(toplevel->scene_tree, NULL);
		toplevel->corners[i]->node.data = toplevel;
		wlr_scene_node_set_enabled(&toplevel->corners[i]->node, false);
	}

	/* Listen to the various events it can emit */
	toplevel->map.notify = xdg_toplevel_map;
	wl_signal_add(&xdg_toplevel->base->surface->events.map, &toplevel->map);
	toplevel->unmap.notify = xdg_toplevel_unmap;
	wl_signal_add(&xdg_toplevel->base->surface->events.unmap, &toplevel->unmap);
	toplevel->commit.notify = xdg_toplevel_commit;
	wl_signal_add(&xdg_toplevel->base->surface->events.commit, &toplevel->commit);
	toplevel->set_title.notify = xdg_toplevel_set_title;
	wl_signal_add(&xdg_toplevel->events.set_title, &toplevel->set_title);

	toplevel->destroy.notify = xdg_toplevel_destroy;
	wl_signal_add(&xdg_toplevel->events.destroy, &toplevel->destroy);

	/* cotd */
	toplevel->request_move.notify = xdg_toplevel_request_move;
	wl_signal_add(&xdg_toplevel->events.request_move, &toplevel->request_move);
	toplevel->request_resize.notify = xdg_toplevel_request_resize;
	wl_signal_add(&xdg_toplevel->events.request_resize, &toplevel->request_resize);
	toplevel->request_maximize.notify = xdg_toplevel_request_maximize;
	wl_signal_add(&xdg_toplevel->events.request_maximize, &toplevel->request_maximize);
	toplevel->request_fullscreen.notify = xdg_toplevel_request_fullscreen;
	wl_signal_add(&xdg_toplevel->events.request_fullscreen, &toplevel->request_fullscreen);
}

static void server_new_xdg_decoration(struct wl_listener *listener, void *data) {
	/* All windows use compositor-drawn (server-side) decorations: the
	 * titlebar and traffic lights belong to the compositor, and the client
	 * surface is strictly the application content area. The mode is applied
	 * after the xdg surface is initialized (setting it here would schedule a
	 * configure on an uninitialized surface and crash). */
	struct wlr_xdg_toplevel_decoration_v1 *decoration = data;
	struct wlr_xdg_toplevel *xdg_toplevel = decoration->toplevel;
	if (xdg_toplevel->base->data != NULL) {
		struct wlr_scene_tree *content = xdg_toplevel->base->data;
		struct polluxdesk_toplevel *toplevel = content->node.data;
		toplevel->decoration = decoration;
	}
}

static void xdg_popup_commit(struct wl_listener *listener, void *data) {
	/* Called when a new surface state is committed. */
	struct polluxdesk_popup *popup = wl_container_of(listener, popup, commit);

	if (popup->xdg_popup->base->initial_commit) {
		/* When an xdg_surface performs an initial commit, the compositor must
		 * reply with a configure so the client can map the surface.
		 * polluxdesk sends an empty configure. A more sophisticated compositor
		 * might change an xdg_popup's geometry to ensure it's not positioned
		 * off-screen, for example. */
		wlr_xdg_surface_schedule_configure(popup->xdg_popup->base);
	}
}

static void xdg_popup_destroy(struct wl_listener *listener, void *data) {
	/* Called when the xdg_popup is destroyed. */
	struct polluxdesk_popup *popup = wl_container_of(listener, popup, destroy);

	wl_list_remove(&popup->commit.link);
	wl_list_remove(&popup->destroy.link);

	free(popup);
}

static void server_new_xdg_popup(struct wl_listener *listener, void *data) {
	/* This event is raised when a client creates a new popup. */
	struct wlr_xdg_popup *xdg_popup = data;

	struct polluxdesk_popup *popup = calloc(1, sizeof(*popup));
	popup->xdg_popup = xdg_popup;

	/* We must add xdg popups to the scene graph so they get rendered. The
	 * wlroots scene graph provides a helper for this, but to use it we must
	 * provide the proper parent scene node of the xdg popup. To enable this,
	 * we always set the user data field of xdg_surfaces to the corresponding
	 * scene node. */
	struct wlr_xdg_surface *parent = wlr_xdg_surface_try_from_wlr_surface(xdg_popup->parent);
	assert(parent != NULL);
	struct wlr_scene_tree *parent_tree = parent->data;
	xdg_popup->base->data = wlr_scene_xdg_surface_create(parent_tree, xdg_popup->base);

	popup->commit.notify = xdg_popup_commit;
	wl_signal_add(&xdg_popup->base->surface->events.commit, &popup->commit);

	popup->destroy.notify = xdg_popup_destroy;
	wl_signal_add(&xdg_popup->events.destroy, &popup->destroy);
}

int main(int argc, char *argv[]) {
	wlr_log_init(WLR_DEBUG, NULL);
	signal(SIGUSR1, settings_signal_handler);
	char *startup_cmd = NULL;

	int c;
	while ((c = getopt(argc, argv, "s:h")) != -1) {
		switch (c) {
		case 's':
			startup_cmd = optarg;
			break;
		default:
			printf("Usage: %s [-s startup command]\n", argv[0]);
			return 0;
		}
	}
	if (optind < argc) {
		printf("Usage: %s [-s startup command]\n", argv[0]);
		return 0;
	}

	struct polluxdesk_server server = {0};
	/* The Wayland display is managed by libwayland. It handles accepting
	 * clients from the Unix socket, managing Wayland globals, and so on. */
	server.wl_display = wl_display_create();
	/* The backend is a wlroots feature which abstracts the underlying input and
	 * output hardware. The autocreate option will choose the most suitable
	 * backend based on the current environment, such as opening an X11 window
	 * if an X11 server is running. */
	server.backend = wlr_backend_autocreate(wl_display_get_event_loop(server.wl_display), NULL);
	if (server.backend == NULL) {
		wlr_log(WLR_ERROR, "failed to create wlr_backend");
		return 1;
	}

	/* Autocreates a renderer, either Pixman, GLES2 or Vulkan for us. The user
	 * can also specify a renderer using the WLR_RENDERER env var.
	 * The renderer is responsible for defining the various pixel formats it
	 * supports for shared memory, this configures that for clients. */
	server.renderer = wlr_renderer_autocreate(server.backend);
	if (server.renderer == NULL) {
		wlr_log(WLR_ERROR, "failed to create wlr_renderer");
		return 1;
	}

	wlr_renderer_init_wl_display(server.renderer, server.wl_display);

	/* Autocreates an allocator for us.
	 * The allocator is the bridge between the renderer and the backend. It
	 * handles the buffer creation, allowing wlroots to render onto the
	 * screen */
	server.allocator = wlr_allocator_autocreate(server.backend,
		server.renderer);
	if (server.allocator == NULL) {
		wlr_log(WLR_ERROR, "failed to create wlr_allocator");
		return 1;
	}

	/* This creates some hands-off wlroots interfaces. The compositor is
	 * necessary for clients to allocate surfaces, the subcompositor allows to
	 * assign the role of subsurfaces to surfaces and the data device manager
	 * handles the clipboard. Each of these wlroots interfaces has room for you
	 * to dig your fingers in and play with their behavior if you want. Note that
	 * the clients cannot set the selection directly without compositor approval,
	 * see the handling of the request_set_selection event below.*/
	wlr_compositor_create(server.wl_display, 5, server.renderer);
	wlr_subcompositor_create(server.wl_display);
	wlr_data_device_manager_create(server.wl_display);

	/* Creates an output layout, which a wlroots utility for working with an
	 * arrangement of screens in a physical layout. */
	server.output_layout = wlr_output_layout_create(server.wl_display);

	/* Configure a listener to be notified when new outputs are available on the
	 * backend. */
	wl_list_init(&server.outputs);
	server.new_output.notify = server_new_output;
	wl_signal_add(&server.backend->events.new_output, &server.new_output);

	/* Create a scene graph. This is a wlroots abstraction that handles all
	 * rendering and damage tracking. All the compositor author needs to do
	 * is add things that should be rendered to the scene graph at the proper
	 * positions and then call wlr_scene_output_commit() to render a frame if
	 * necessary.
	 */
	server.scene = wlr_scene_create();
	server.scene_layout = wlr_scene_attach_output_layout(server.scene, server.output_layout);

	/* Set up xdg-shell version 3. The xdg-shell is a Wayland protocol which is
	 * used for application windows. For more detail on shells, refer to
	 * https://drewdevault.com/2018/07/29/Wayland-shells.html.
	 */
	wl_list_init(&server.toplevels);
	server.xdg_shell = wlr_xdg_shell_create(server.wl_display, 3);
	server.new_xdg_toplevel.notify = server_new_xdg_toplevel;
	wl_signal_add(&server.xdg_shell->events.new_toplevel, &server.new_xdg_toplevel);
	server.new_xdg_popup.notify = server_new_xdg_popup;
	wl_signal_add(&server.xdg_shell->events.new_popup, &server.new_xdg_popup);

	/* Tell clients the compositor draws the window chrome (macOS/Windows
	 * style). This is the protocol-level enforcement of the separated
	 * titlebar/content model. */
	server.xdg_decoration_manager = wlr_xdg_decoration_manager_v1_create(server.wl_display);
	server.new_xdg_decoration.notify = server_new_xdg_decoration;
	wl_signal_add(&server.xdg_decoration_manager->events.new_toplevel_decoration,
		&server.new_xdg_decoration);

	/* Screenshots (grim) for testing/debugging. */
	server.screencopy_manager = wlr_screencopy_manager_v1_create(server.wl_display);
	server.xdg_output_manager =
		wlr_xdg_output_manager_v1_create(server.wl_display, server.output_layout);

	/* Shared-memory client buffers. */
	wlr_shm_create_with_renderer(server.wl_display, 1, server.renderer);

	/*
	 * Creates a cursor, which is a wlroots utility for tracking the cursor
	 * image shown on screen.
	 */
	server.cursor = wlr_cursor_create();
	wlr_cursor_attach_output_layout(server.cursor, server.output_layout);

	/* Creates an xcursor manager, another wlroots utility which loads up
	 * Xcursor themes to source cursor images from and makes sure that cursor
	 * images are available at all scale factors on the screen (necessary for
	 * HiDPI support). */
	server.cursor_mgr = wlr_xcursor_manager_create(NULL, 24);
	/* Make sure a default cursor image exists immediately, even before the
	 * first pointer motion (fixes an invisible mouse on some backends). */
	wlr_cursor_set_xcursor(server.cursor, server.cursor_mgr, "default");

	/* Custom resize cursors: white badge + bold double-arrow, much more
	 * visible than the (often missing) X cursor theme entries. */
	server.resize_cursor_h =
		resize_cursor_buffer_create(POLLUXDESK_RESIZE_CURSOR_H);
	server.resize_cursor_v =
		resize_cursor_buffer_create(POLLUXDESK_RESIZE_CURSOR_V);
	server.resize_cursor_nwse =
		resize_cursor_buffer_create(POLLUXDESK_RESIZE_CURSOR_NWSE);
	server.resize_cursor_nesw =
		resize_cursor_buffer_create(POLLUXDESK_RESIZE_CURSOR_NESW);

	/*
	 * wlr_cursor *only* displays an image on screen. It does not move around
	 * when the pointer moves. However, we can attach input devices to it, and
	 * it will generate aggregate events for all of them. In these events, we
	 * can choose how we want to process them, forwarding them to clients and
	 * moving the cursor around. More detail on this process is described in
	 * https://drewdevault.com/2018/07/17/Input-handling-in-wlroots.html.
	 *
	 * And more comments are sprinkled throughout the notify functions above.
	 */
	server.cursor_mode = POLLUXDESK_CURSOR_PASSTHROUGH;
	server.cursor_motion.notify = server_cursor_motion;
	wl_signal_add(&server.cursor->events.motion, &server.cursor_motion);
	server.cursor_motion_absolute.notify = server_cursor_motion_absolute;
	wl_signal_add(&server.cursor->events.motion_absolute,
			&server.cursor_motion_absolute);
	server.cursor_button.notify = server_cursor_button;
	wl_signal_add(&server.cursor->events.button, &server.cursor_button);
	server.cursor_axis.notify = server_cursor_axis;
	wl_signal_add(&server.cursor->events.axis, &server.cursor_axis);
	server.cursor_frame.notify = server_cursor_frame;
	wl_signal_add(&server.cursor->events.frame, &server.cursor_frame);

	/*
	 * Configures a seat, which is a single "seat" at which a user sits and
	 * operates the computer. This conceptually includes up to one keyboard,
	 * pointer, touch, and drawing tablet device. We also rig up a listener to
	 * let us know when new input devices are available on the backend.
	 */
	wl_list_init(&server.keyboards);
	server.new_input.notify = server_new_input;
	wl_signal_add(&server.backend->events.new_input, &server.new_input);
	server.seat = wlr_seat_create(server.wl_display, "seat0");

	/*
	 * Test hook: expose the wlroots virtual-pointer protocol so an automated
	 * harness (wlrctl) can drive the cursor for UI verification on a headless
	 * build host.  Off unless explicitly requested, because with it enabled
	 * any connected client could move the pointer and click on its own.
	 */
	if (getenv("POLLUX_VIRTUAL_INPUT") != NULL) {
		server.virtual_pointer_mgr =
			wlr_virtual_pointer_manager_v1_create(server.wl_display);
		server.new_virtual_pointer.notify = server_new_virtual_pointer;
		wl_signal_add(&server.virtual_pointer_mgr->events.new_virtual_pointer,
			&server.new_virtual_pointer);
		server.virtual_keyboard_mgr =
			wlr_virtual_keyboard_manager_v1_create(server.wl_display);
		server.new_virtual_keyboard.notify = server_new_virtual_keyboard;
		wl_signal_add(&server.virtual_keyboard_mgr->events.new_virtual_keyboard,
			&server.new_virtual_keyboard);
		wlr_log(WLR_INFO, "virtual pointer and keyboard managers enabled "
			"(test hook)");
	}
	server.request_cursor.notify = seat_request_cursor;
	wl_signal_add(&server.seat->events.request_set_cursor,
			&server.request_cursor);
	server.pointer_focus_change.notify = seat_pointer_focus_change;
	wl_signal_add(&server.seat->pointer_state.events.focus_change,
			&server.pointer_focus_change);
	server.request_set_selection.notify = seat_request_set_selection;
	wl_signal_add(&server.seat->events.request_set_selection,
			&server.request_set_selection);

	/* Add a Unix socket to the Wayland display. */
	const char *socket = wl_display_add_socket_auto(server.wl_display);
	if (!socket) {
		wlr_backend_destroy(server.backend);
		return 1;
	}

	/* Start the backend. This will enumerate outputs and inputs, become the DRM
	 * master, etc */
	if (!wlr_backend_start(server.backend)) {
		wlr_backend_destroy(server.backend);
		wl_display_destroy(server.wl_display);
		return 1;
	}

	/* Set the WAYLAND_DISPLAY environment variable to our socket and run the
	 * startup command if requested. */
	setenv("WAYLAND_DISPLAY", socket, true);
	if (startup_cmd) {
		if (fork() == 0) {
			execl("/bin/sh", "/bin/sh", "-c", startup_cmd, (void *)NULL);
		}
	}
	/* Run the Wayland event loop. This does not return until you exit the
	 * compositor. Starting the backend rigged up all of the necessary event
	 * loop configuration to listen to libinput events, DRM events, generate
	 * frame events at the refresh rate, and so on. */
	wlr_log(WLR_INFO, "Running Wayland compositor on WAYLAND_DISPLAY=%s",
			socket);

	/* Publish the pid so clients can signal us without having to match our
	 * name, which BSD truncates to 19 characters. */
	{
		const char *home = getenv("HOME");
		if (home != NULL) {
			char dir[1024];
			char path[1088];
			snprintf(dir, sizeof(dir), "%s/.config/polluxdesk", home);
			mkdir(dir, 0755);
			snprintf(path, sizeof(path), "%s/compositor.pid", dir);
			FILE *pidFile = fopen(path, "w");
			if (pidFile != NULL) {
				fprintf(pidFile, "%ld\n", (long)getpid());
				fclose(pidFile);
			}
		}
	}

	/* Publish an empty window list before anything can connect. The desktop
	 * shell trusts the file only while the process named in it is alive, so
	 * a compositor that starts and finds no windows has to say so -- else a
	 * shell starting later would read the previous compositor's list and
	 * draw running indicators for windows that no longer exist. */
	write_window_state(&server);

	wl_display_run(server.wl_display);


	/* Once wl_display_run returns, we destroy all clients then shut down the
	 * server. */
	wl_display_destroy_clients(server.wl_display);

	wl_list_remove(&server.new_xdg_toplevel.link);
	wl_list_remove(&server.new_xdg_popup.link);
	wl_list_remove(&server.new_xdg_decoration.link);

	wl_list_remove(&server.cursor_motion.link);
	wl_list_remove(&server.cursor_motion_absolute.link);
	wl_list_remove(&server.cursor_button.link);
	wl_list_remove(&server.cursor_axis.link);
	wl_list_remove(&server.cursor_frame.link);

	wl_list_remove(&server.new_input.link);
	wl_list_remove(&server.request_cursor.link);
	wl_list_remove(&server.pointer_focus_change.link);
	wl_list_remove(&server.request_set_selection.link);

	wl_list_remove(&server.new_output.link);

	wlr_scene_node_destroy(&server.scene->tree.node);

	if (server.resize_cursor_h != NULL) {
		wlr_buffer_drop(server.resize_cursor_h);
	}
	if (server.resize_cursor_v != NULL) {
		wlr_buffer_drop(server.resize_cursor_v);
	}
	if (server.resize_cursor_nwse != NULL) {
		wlr_buffer_drop(server.resize_cursor_nwse);
	}
	if (server.resize_cursor_nesw != NULL) {
		wlr_buffer_drop(server.resize_cursor_nesw);
	}

	wlr_xcursor_manager_destroy(server.cursor_mgr);
	wlr_cursor_destroy(server.cursor);
	wlr_allocator_destroy(server.allocator);
	wlr_renderer_destroy(server.renderer);
	wlr_backend_destroy(server.backend);
	wl_display_destroy(server.wl_display);
	return 0;
}
