#include "WineDisplayHost.h"
#include "wineios_host.h"

#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>

/*
 * Registry of surfaces the driver has handed us. All fields are guarded by
 * reg_mutex; the pixel bits themselves are guarded by the driver's per-surface
 * lock (info.lock/unlock). The driver guarantees callbacks are never made
 * while it holds a pixel lock, so taking reg_mutex inside callbacks and
 * pixel locks under reg_mutex in readers cannot deadlock.
 */

#define MAX_SURFACES 32
#define MAX_TITLES   32
#define TITLE_CHARS  256

struct host_surface {
    int used;
    wine_gui_surface_info pub;
    const void *bits;
    uint32_t stride;
    void (*lock)(void *lock_ctx);
    void (*unlock)(void *lock_ctx);
    void *lock_ctx;
};

struct host_title {
    int used;
    uint64_t hwnd;
    uint16_t text[TITLE_CHARS];
    uint32_t len;
};

static pthread_mutex_t reg_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct host_surface surfaces[MAX_SURFACES];
static struct host_title titles[MAX_TITLES];
static _Atomic uint64_t change_seq = 1;
static uint32_t screen_w, screen_h;

/* input queue: main thread enqueues, the driver's Wine thread blocks in
 * next_event draining it */
#define QUEUE_LEN 512
static pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t queue_cond = PTHREAD_COND_INITIALIZER;
static struct wineios_event queue[QUEUE_LEN];
static int queue_head, queue_count;

static void bump(void) { atomic_fetch_add(&change_seq, 1); }

static struct host_surface *find_surface(uint64_t id)
{
    for (int i = 0; i < MAX_SURFACES; i++)
        if (surfaces[i].used && surfaces[i].pub.id == id) return &surfaces[i];
    return NULL;
}

/* --- driver -> host callbacks (Wine threads) ------------------------------ */

static int cb_screen_size(void *ctx, uint32_t *width, uint32_t *height)
{
    if (!screen_w || !screen_h) return 0;
    *width = screen_w;
    *height = screen_h;
    return 1;
}

static void cb_surface_added(void *ctx, const struct wineios_surface_info *info)
{
    pthread_mutex_lock(&reg_mutex);
    for (int i = 0; i < MAX_SURFACES; i++) {
        if (surfaces[i].used) continue;
        surfaces[i].used = 1;
        surfaces[i].pub = (wine_gui_surface_info){
            .id = info->id, .hwnd = info->hwnd,
            .x = 0, .y = 0, .w = (int32_t)info->width, .h = (int32_t)info->height,
            .visible = 0,
            .width = info->width, .height = info->height,
            .update_seq = 0,
        };
        surfaces[i].bits = info->bits;
        surfaces[i].stride = info->stride;
        surfaces[i].lock = info->lock;
        surfaces[i].unlock = info->unlock;
        surfaces[i].lock_ctx = info->lock_ctx;
        break;
    }
    pthread_mutex_unlock(&reg_mutex);
    bump();
}

static void cb_surface_placed(void *ctx, uint64_t id, int32_t x, int32_t y,
                              int32_t w, int32_t h, int visible)
{
    pthread_mutex_lock(&reg_mutex);
    struct host_surface *s = find_surface(id);
    if (s) {
        s->pub.x = x; s->pub.y = y; s->pub.w = w; s->pub.h = h;
        s->pub.visible = visible;
    }
    pthread_mutex_unlock(&reg_mutex);
    bump();
}

static void cb_surface_updated(void *ctx, uint64_t id, int32_t left, int32_t top,
                               int32_t right, int32_t bottom)
{
    pthread_mutex_lock(&reg_mutex);
    struct host_surface *s = find_surface(id);
    if (s) s->pub.update_seq++;
    pthread_mutex_unlock(&reg_mutex);
    bump();
}

static void cb_surface_removed(void *ctx, uint64_t id)
{
    /* Contract: once we return, bits/lock must not be used again. Readers
     * hold reg_mutex across their whole copy, so clearing under it is
     * sufficient. */
    pthread_mutex_lock(&reg_mutex);
    struct host_surface *s = find_surface(id);
    if (s) memset(s, 0, sizeof(*s));
    pthread_mutex_unlock(&reg_mutex);
    bump();
}

static void cb_window_title(void *ctx, uint64_t hwnd, const uint16_t *utf16, uint32_t len)
{
    pthread_mutex_lock(&reg_mutex);
    struct host_title *slot = NULL;
    for (int i = 0; i < MAX_TITLES; i++) {
        if (titles[i].used && titles[i].hwnd == hwnd) { slot = &titles[i]; break; }
        if (!slot && !titles[i].used) slot = &titles[i];
    }
    if (slot) {
        if (len > TITLE_CHARS) len = TITLE_CHARS;
        slot->used = 1;
        slot->hwnd = hwnd;
        slot->len = len;
        memcpy(slot->text, utf16, len * sizeof(*utf16));
    }
    pthread_mutex_unlock(&reg_mutex);
    bump();
}

static int cb_next_event(void *ctx, struct wineios_event *out, int32_t timeout_ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }

    pthread_mutex_lock(&queue_mutex);
    while (!queue_count) {
        if (pthread_cond_timedwait(&queue_cond, &queue_mutex, &ts) != 0) {
            pthread_mutex_unlock(&queue_mutex);
            return 0;
        }
    }
    *out = queue[queue_head];
    queue_head = (queue_head + 1) % QUEUE_LEN;
    queue_count--;
    pthread_mutex_unlock(&queue_mutex);
    return 1;
}

static const struct wineios_host host_vtable = {
    .abi_version = WINEIOS_HOST_ABI_VERSION,
    .ctx = NULL,
    .screen_size = cb_screen_size,
    .surface_added = cb_surface_added,
    .surface_placed = cb_surface_placed,
    .surface_updated = cb_surface_updated,
    .surface_removed = cb_surface_removed,
    .window_title = cb_window_title,
    .next_event = cb_next_event,
};

/* The entry point the driver dlsym's (WINEIOS_HOST_ENTRY). Must survive
 * dead-stripping and stay externally visible in the app binary. */
__attribute__((used, visibility("default")))
const struct wineios_host *wineios_host_get(void)
{
    return &host_vtable;
}

/* --- app-facing API (Swift) ----------------------------------------------- */

void wine_display_host_configure(uint32_t screen_width, uint32_t screen_height)
{
    screen_w = screen_width;
    screen_h = screen_height;
}

uint64_t wine_display_change_seq(void)
{
    return atomic_load(&change_seq);
}

int wine_display_list_surfaces(wine_gui_surface_info *out, int max_count)
{
    int n = 0;
    pthread_mutex_lock(&reg_mutex);
    for (int i = 0; i < MAX_SURFACES && n < max_count; i++)
        if (surfaces[i].used) out[n++] = surfaces[i].pub;
    pthread_mutex_unlock(&reg_mutex);
    return n;
}

int wine_display_copy_surface(uint64_t id, void *dst, size_t dst_size)
{
    int ok = 0;
    pthread_mutex_lock(&reg_mutex);
    struct host_surface *s = find_surface(id);
    if (s) {
        size_t needed = (size_t)s->stride * s->pub.height;
        if (dst_size >= needed) {
            s->lock(s->lock_ctx);
            memcpy(dst, s->bits, needed);
            s->unlock(s->lock_ctx);
            ok = 1;
        }
    }
    pthread_mutex_unlock(&reg_mutex);
    return ok;
}

int wine_display_copy_title(uint64_t hwnd, char *utf8, int max_bytes)
{
    int written = 0;
    pthread_mutex_lock(&reg_mutex);
    for (int i = 0; i < MAX_TITLES; i++) {
        if (!titles[i].used || titles[i].hwnd != hwnd) continue;
        /* minimal UTF-16 -> UTF-8 (drops surrogate pairs to '?': titles are
         * diagnostics, not typography) */
        for (uint32_t c = 0; c < titles[i].len && written < max_bytes - 4; c++) {
            uint32_t ch = titles[i].text[c];
            if (ch >= 0xd800 && ch <= 0xdfff) ch = '?';
            if (ch < 0x80) utf8[written++] = (char)ch;
            else if (ch < 0x800) {
                utf8[written++] = (char)(0xc0 | (ch >> 6));
                utf8[written++] = (char)(0x80 | (ch & 0x3f));
            } else {
                utf8[written++] = (char)(0xe0 | (ch >> 12));
                utf8[written++] = (char)(0x80 | ((ch >> 6) & 0x3f));
                utf8[written++] = (char)(0x80 | (ch & 0x3f));
            }
        }
        break;
    }
    pthread_mutex_unlock(&reg_mutex);
    return written;
}

static void enqueue(const struct wineios_event *ev)
{
    pthread_mutex_lock(&queue_mutex);
    if (queue_count < QUEUE_LEN) {
        queue[(queue_head + queue_count) % QUEUE_LEN] = *ev;
        queue_count++;
        pthread_cond_signal(&queue_cond);
    }
    /* full queue: drop newest — input is transient */
    pthread_mutex_unlock(&queue_mutex);
}

void wine_display_send_mouse_move(uint64_t surface_id, int32_t x, int32_t y)
{
    struct wineios_event ev = { .type = WINEIOS_EVENT_MOUSE_MOVE,
                                .surface_id = surface_id, .x = x, .y = y };
    enqueue(&ev);
}

void wine_display_send_mouse_button(uint64_t surface_id, int32_t x, int32_t y,
                                    uint32_t button, int down)
{
    struct wineios_event ev = { .type = WINEIOS_EVENT_MOUSE_BUTTON,
                                .surface_id = surface_id, .x = x, .y = y,
                                .button = button, .down = (uint32_t)!!down };
    enqueue(&ev);
}

void wine_display_send_wheel(uint64_t surface_id, int32_t x, int32_t y, int32_t delta)
{
    struct wineios_event ev = { .type = WINEIOS_EVENT_MOUSE_WHEEL,
                                .surface_id = surface_id, .x = x, .y = y, .wheel = delta };
    enqueue(&ev);
}

void wine_display_send_key(uint32_t vk, int down)
{
    struct wineios_event ev = { .type = WINEIOS_EVENT_KEY, .vk = vk, .down = (uint32_t)!!down };
    enqueue(&ev);
}

void wine_display_send_char(uint32_t utf16)
{
    struct wineios_event ev = { .type = WINEIOS_EVENT_CHAR, .ch = utf16 };
    enqueue(&ev);
}
