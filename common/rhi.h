/*
 * Render Hardware Interface (RHI)
 *
 * Vtable-based abstraction over the renderer backend.  At
 * libretro entry the runtime selects one backend (software
 * today; vulkan / gl / d3d11 / d3d12 to come) and publishes
 * it through the g_rhi pointer.  All renderer-aware code
 * outside backend_*.c calls through the vtable and never
 * sees the active backend's API directly.
 *
 * Phase 1 surface is intentionally minimal -- just enough to
 * route the existing software renderer through the vtable so
 * the abstraction is exercised before any HW backend exists.
 * The vtable will grow as HW backends are added: texture
 * upload, lightmap update, palette / colormap LUT publish,
 * a 2D draw path for HUD / console / menus, frame begin/end
 * brackets around command-buffer work, and so on.
 *
 * Selection logic is centralized in rhi.c; backends are
 * single-TU implementations in backend_*.c with all internal
 * state file-static and the only exported symbol being a
 * single render_backend_t instance.
 */

#ifndef RHI_H
#define RHI_H

#include "qtypes.h"
#include "render.h"   /* for refdef_t */
#include "wad.h"      /* for qpic_t (queue_2d_pic) */

typedef enum {
    RHI_BACKEND_NONE = 0,
    RHI_BACKEND_SOFTWARE,
    RHI_BACKEND_VULKAN,
    RHI_BACKEND_GL,
    RHI_BACKEND_D3D11,
    RHI_BACKEND_D3D12,
    RHI_BACKEND_COUNT
} rhi_backend_kind;

typedef struct render_backend_s {
    const char       *name;          /* "software", "vulkan", ... */
    rhi_backend_kind  kind;

    /* Lifecycle.  init() returns false if the backend can't
     * stand up (e.g. a HW context request denied by frontend).
     * On false, rhi.c falls back to the next candidate. */
    qboolean (*init)(void);
    void     (*shutdown)(void);

    /* Per-frame brackets.  No-ops on the software backend (the
     * SW path emits pixels directly into the linear output
     * buffer that the libretro video callback reads at end of
     * retro_run).  HW backends will use these for command-
     * buffer acquire / submit. */
    void     (*begin_frame)(void);
    void     (*draw_view)(const refdef_t *rd);
    void     (*end_frame)(void);

    /* 2D pic intercept (Phase 4k).  Backends that render HUD /
     * menu pics natively populate this; SW backend leaves it
     * NULL and Draw_Pic falls through to its original
     * memcpy-into-vid.buffer path.  When non-NULL the call
     * runs in _addition_ to the SW path -- Phase 4k double-
     * renders (pixel-perfect overlap, so it looks normal);
     * Phase 4l suppresses the SW path when the intercept is
     * active.
     *
     * x, y are in vid.width / vid.height screen-space (the
     * same coordinates Draw_Pic gets); the backend converts
     * to NDC internally.  pic->width / pic->height define
     * the rectangle, pic->data is width*height bytes of
     * palette indices. */
    void     (*queue_2d_pic)(int x, int y, const qpic_t *pic);
} render_backend_t;

/* The active backend.  Set by rhi_init(), read by every
 * renderer call site.  NULL before rhi_init() returns
 * successfully -- callers should fall back to the SW path
 * inline if g_rhi is NULL (defensive: rhi_init only fails on
 * an explicit HW backend request whose context the frontend
 * refuses, and the software path is always available, so in
 * practice g_rhi is non-NULL after retro_load_game). */
extern const render_backend_t *g_rhi;

/* Backend instances published by each backend_*.c.  Each
 * .c file compiles unconditionally so its vtable symbol is
 * always linkable; whether the backend actually stands up is
 * decided at rhi_init() time by the backend's init function
 * (which returns false when its build flag -- RHI_HAVE_VULKAN
 * etc. -- is undefined, causing rhi.c's auto path to fall
 * through to the next candidate). */
extern const render_backend_t g_rhi_backend_sw;
extern const render_backend_t g_rhi_backend_vk;

/* Backend-selection entry point, called from retro_load_game
 * after update_variables() has populated the core option
 * cache.  Reads the 'tyrquake_renderer' option and picks a
 * backend.  Returns false only if no backend stands up --
 * which should not happen because the software backend's
 * init is trivially successful. */
qboolean rhi_init(void);

/* Tear down the active backend.  Called from
 * retro_unload_game.  Safe to call when no backend is
 * active. */
void     rhi_shutdown(void);

#endif /* RHI_H */
