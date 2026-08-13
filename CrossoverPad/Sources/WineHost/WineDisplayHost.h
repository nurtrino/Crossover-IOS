#ifndef CROSSOVER_WINE_DISPLAY_HOST_H
#define CROSSOVER_WINE_DISPLAY_HOST_H

/*
 * WineDisplayHost — the app side of the wineios.drv display bridge.
 *
 * The Wine display driver (wineios.drv, running on Wine background threads in
 * THIS process) resolves `wineios_host_get` with dlsym and calls the returned
 * vtable to register window surfaces, report dirty rects, and drain input
 * (contract: wineios_host.h). This file wraps that callback surface into a
 * poll-friendly API for Swift:
 *
 *   presenter (CADisplayLink, main thread)
 *     wine_display_change_seq()      — cheap "anything new?" check
 *     wine_display_list_surfaces()   — snapshot of live surfaces
 *     wine_display_copy_surface()    — BGRA pixels under the driver's lock
 *   input (touch/keyboard handlers, main thread)
 *     wine_display_send_*            — enqueue; a Wine thread injects them
 *
 * Everything here is thread-safe; nothing blocks on Wine.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wine_gui_surface_info {
    uint64_t id;            /* driver surface id */
    uint64_t hwnd;          /* Windows HWND (diagnostic) */
    int32_t  x, y, w, h;    /* placement of the window's visible rect on the desktop */
    int32_t  visible;
    uint32_t width, height; /* pixel size of the surface bitmap */
    uint64_t update_seq;    /* bumped on every pixel update of this surface */
} wine_gui_surface_info;

/* Set the desktop size (screen pixels) BEFORE starting a GUI session; the
 * driver asks for it once at init. */
void wine_display_host_configure(uint32_t screen_width, uint32_t screen_height);

/* Bumped on any surface add/remove/place/update. */
uint64_t wine_display_change_seq(void);

/* Snapshot the live surfaces (creation order). Returns the count written. */
int wine_display_list_surfaces(wine_gui_surface_info *out, int max_count);

/* Copy a surface's BGRA8888 top-down pixels into dst (needs width*height*4
 * bytes). Returns 1 on success, 0 if the surface is gone or dst too small. */
int wine_display_copy_surface(uint64_t id, void *dst, size_t dst_size);

/* Latest window title (UTF-16 -> UTF-8) for the surface's window, if any.
 * Returns the byte length written (0 if none). */
int wine_display_copy_title(uint64_t hwnd, char *utf8, int max_bytes);

/* Input; coordinates are Windows virtual-desktop pixels. surface_id may be 0
 * (events then target the foreground window). */
void wine_display_send_mouse_move(uint64_t surface_id, int32_t x, int32_t y);
void wine_display_send_mouse_button(uint64_t surface_id, int32_t x, int32_t y,
                                    uint32_t button, int down);
void wine_display_send_wheel(uint64_t surface_id, int32_t x, int32_t y, int32_t delta);
void wine_display_send_key(uint32_t vk, int down);
void wine_display_send_char(uint32_t utf16);

#ifdef __cplusplus
}
#endif

#endif /* CROSSOVER_WINE_DISPLAY_HOST_H */
