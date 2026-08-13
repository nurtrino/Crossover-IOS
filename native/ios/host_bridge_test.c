/*
 * host_bridge_test — exercises the app side of the wineios.drv display
 * bridge (CrossoverPad/Sources/WineHost/WineDisplayHost.c) by playing the
 * driver's role: registers a surface, flushes pixels, and round-trips
 * input events and window titles through the same vtable wineios.drv
 * resolves at runtime. Pure host C — compiles and runs anywhere:
 *
 *   gcc -o /tmp/hbt native/ios/host_bridge_test.c \
 *       CrossoverPad/Sources/WineHost/WineDisplayHost.c \
 *       -ICrossoverPad/Sources/WineHost -pthread && /tmp/hbt
 */
#include "WineDisplayHost.h"
#include "wineios_host.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

extern const struct wineios_host *wineios_host_get(void);

static unsigned char bits[64 * 32 * 4];
static pthread_mutex_t px_lock = PTHREAD_MUTEX_INITIALIZER;
static void lockf(void *c) { pthread_mutex_lock(&px_lock); }
static void unlockf(void *c) { pthread_mutex_unlock(&px_lock); }

int main(void)
{
    const struct wineios_host *h = wineios_host_get();
    assert(h && h->abi_version == WINEIOS_HOST_ABI_VERSION);

    uint32_t w, hh;
    wine_display_host_configure(800, 600);
    assert(h->screen_size(h->ctx, &w, &hh) == 1 && w == 800 && hh == 600);

    struct wineios_surface_info info = {
        .id = 7, .hwnd = 0x1234, .bits = bits, .width = 64, .height = 32,
        .stride = 64 * 4, .lock = lockf, .unlock = unlockf, .lock_ctx = 0,
    };
    uint64_t seq0 = wine_display_change_seq();
    h->surface_added(h->ctx, &info);
    h->surface_placed(h->ctx, 7, 10, 20, 64, 32, 1);
    memset(bits, 0xAB, sizeof(bits));
    h->surface_updated(h->ctx, 7, 0, 0, 64, 32);
    assert(wine_display_change_seq() > seq0);

    wine_gui_surface_info list[8];
    int n = wine_display_list_surfaces(list, 8);
    assert(n == 1 && list[0].id == 7 && list[0].x == 10 && list[0].visible == 1
           && list[0].update_seq == 1);

    unsigned char out[64 * 32 * 4];
    assert(wine_display_copy_surface(7, out, sizeof(out)) == 1);
    assert(out[0] == 0xAB && out[sizeof(out) - 1] == 0xAB);
    assert(wine_display_copy_surface(7, out, 16) == 0);  /* too small */

    uint16_t title[] = { 'H', 'i', 0x4E2D };  /* "Hi" + CJK */
    h->window_title(h->ctx, 0x1234, title, 3);
    char utf8[64];
    int len = wine_display_copy_title(0x1234, utf8, sizeof(utf8));
    assert(len == 5 && !memcmp(utf8, "Hi\xe4\xb8\xad", 5));

    /* input queue: producer/consumer round trip */
    wine_display_send_mouse_move(7, 100, 200);
    wine_display_send_mouse_button(7, 100, 200, 0, 1);
    wine_display_send_char('A');
    struct wineios_event ev;
    assert(h->next_event(h->ctx, &ev, 100) == 1 && ev.type == WINEIOS_EVENT_MOUSE_MOVE
           && ev.x == 100 && ev.y == 200 && ev.surface_id == 7);
    assert(h->next_event(h->ctx, &ev, 100) == 1 && ev.type == WINEIOS_EVENT_MOUSE_BUTTON && ev.down == 1);
    assert(h->next_event(h->ctx, &ev, 100) == 1 && ev.type == WINEIOS_EVENT_CHAR && ev.ch == 'A');
    assert(h->next_event(h->ctx, &ev, 50) == 0);  /* timeout, empty */

    h->surface_removed(h->ctx, 7);
    assert(wine_display_list_surfaces(list, 8) == 0);
    assert(wine_display_copy_surface(7, out, sizeof(out)) == 0);

    puts("host bridge test PASS");
    return 0;
}
