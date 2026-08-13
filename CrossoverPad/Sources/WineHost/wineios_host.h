/*
 * wineios.drv <-> host application bridge contract
 *
 * Copyright 2026 the Crossover-IOS project
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/*
 * The contract between the in-process Wine display driver (wineios.drv,
 * running on Wine threads) and the host application's presenter (UIKit/Metal,
 * running on the app main thread). Everything lives in ONE process; the
 * driver finds the host with dlsym(RTLD_DEFAULT, WINEIOS_HOST_ENTRY) at init.
 * If the symbol is absent the driver runs headless (surfaces still exist,
 * flushes are counted and optionally dumped — that is the CI mode).
 *
 * This header is intentionally self-contained plain C99 (no Wine, no UIKit
 * types) because it is compiled on both sides: the Wine unixlib and the app's
 * WineHost bridge keep an identical copy each (see
 * native/wine/dlls/wineios.drv/wineios_host.h is the twin; keep the two in sync — the
 * abi_version handshake catches a mismatch at runtime).
 *
 * Threading rules (the whole design hangs on these):
 *  - Every host callback here is invoked from a WINE thread. Callbacks must
 *    not block on the app main thread and must not call UIKit directly;
 *    they only mutate host-side registries/queues under host locks.
 *  - The host reads surface pixels from its own threads at any time, but
 *    only between surface_added and the return of surface_removed, and only
 *    while holding the surface's lock (wineios_surface_info.lock/unlock).
 *  - next_event blocks a dedicated Wine guest thread; the host's input
 *    handlers (main thread) only enqueue and signal.
 *  - The driver NEVER invokes a host callback while holding a surface's
 *    pixel lock, so host callbacks may take host-side locks that the
 *    host's readers hold around lock()/unlock() without deadlocking.
 */

#ifndef __WINEIOS_HOST_H
#define __WINEIOS_HOST_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WINEIOS_HOST_ABI_VERSION 1

/* The symbol the driver resolves in the host executable:
 *   const struct wineios_host *wineios_host_get(void);
 * Return NULL to decline (driver then runs headless). */
#define WINEIOS_HOST_ENTRY "wineios_host_get"

/* input events, host -> driver */
enum wineios_event_type
{
    WINEIOS_EVENT_MOUSE_MOVE = 1,   /* x,y screen px */
    WINEIOS_EVENT_MOUSE_BUTTON,     /* x,y screen px; button; down */
    WINEIOS_EVENT_MOUSE_WHEEL,      /* x,y screen px; wheel = signed detents*120 */
    WINEIOS_EVENT_KEY,              /* vk = Windows virtual-key code; down */
    WINEIOS_EVENT_CHAR,             /* ch = UTF-16 code unit (injected as unicode key) */
};

enum wineios_mouse_button
{
    WINEIOS_BUTTON_LEFT = 0,
    WINEIOS_BUTTON_RIGHT = 1,
    WINEIOS_BUTTON_MIDDLE = 2,
};

struct wineios_event
{
    uint32_t type;        /* enum wineios_event_type */
    uint64_t surface_id;  /* surface under the touch, 0 = foreground window */
    int32_t  x, y;        /* screen pixels (Windows virtual-desktop coords) */
    uint32_t button;      /* enum wineios_mouse_button */
    int32_t  wheel;       /* wheel delta, multiples of 120 */
    uint32_t vk;          /* Windows VK_* code */
    uint32_t ch;          /* UTF-16 code unit */
    uint32_t down;        /* 1 = press, 0 = release (buttons / keys) */
};

/* a top-level window's pixel surface, driver -> host */
struct wineios_surface_info
{
    uint64_t id;          /* driver-assigned, unique for the process lifetime */
    uint64_t hwnd;        /* Windows HWND value (opaque to the host; for logs) */
    void    *bits;        /* BGRA8888, top-down, valid until surface_removed returns */
    uint32_t width;       /* pixel size of the surface bitmap */
    uint32_t height;
    uint32_t stride;      /* bytes per row (width * 4) */
    /* pixel lock: hold while reading bits; the driver holds it while writing */
    void   (*lock)(void *lock_ctx);
    void   (*unlock)(void *lock_ctx);
    void    *lock_ctx;
};

struct wineios_host
{
    uint32_t abi_version;  /* must equal WINEIOS_HOST_ABI_VERSION */
    void    *ctx;          /* passed back to every callback */

    /* Screen size in PIXELS the Windows desktop should run at
     * (UIScreen.nativeBounds). Return 0 to fall back to the driver default. */
    int  (*screen_size)(void *ctx, uint32_t *width, uint32_t *height);

    /* surface lifecycle (Wine threads; keep these quick, no UIKit) */
    void (*surface_added)(void *ctx, const struct wineios_surface_info *info);
    /* new position/visibility of a surface's window on the virtual desktop */
    void (*surface_placed)(void *ctx, uint64_t id, int32_t x, int32_t y,
                           int32_t width, int32_t height, int visible);
    /* pixels changed inside the given surface-local rect (already written) */
    void (*surface_updated)(void *ctx, uint64_t id, int32_t left, int32_t top,
                            int32_t right, int32_t bottom);
    /* after this returns the host must no longer touch bits/lock */
    void (*surface_removed)(void *ctx, uint64_t id);

    /* window title changed (UTF-16, not nul-terminated) */
    void (*window_title)(void *ctx, uint64_t hwnd, const uint16_t *utf16, uint32_t len);

    /* Blocking input dequeue, called from a dedicated Wine thread.
     * Return 1 with *out filled, 0 on timeout, -1 to stop the input loop. */
    int  (*next_event)(void *ctx, struct wineios_event *out, int32_t timeout_ms);
};

typedef const struct wineios_host *(*wineios_host_get_fn)(void);

#ifdef __cplusplus
}
#endif

#endif /* __WINEIOS_HOST_H */
