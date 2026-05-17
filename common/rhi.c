/*
 * RHI selection and dispatch.
 *
 * The 'tyrquake_renderer' core option selects a backend at
 * retro_load_game time.  Today only two values are valid:
 *
 *   auto      -> Future: try the frontend's preferred HW
 *                context via GET_PREFERRED_HW_RENDER, then a
 *                fallback chain (vulkan -> d3d12 -> d3d11 ->
 *                gl -> software).  Phase 1 has only the
 *                software backend built, so auto always
 *                lands there.
 *   software  -> Always the software backend, regardless of
 *                what the frontend offers.  Useful for
 *                debugging or when the user wants the SW
 *                rasterizer's specific look (per-span affine
 *                texturing, palette + colormap output) even
 *                though the frontend's video driver could
 *                support a HW path.
 *
 * The option is marked requires-restart in
 * libretro_core_options.h so any change tears down and
 * rebuilds at game-load.  No hot-swap.
 */

#include "rhi.h"
#include "common.h"
#include <string.h>
#include <libretro.h>

const render_backend_t *g_rhi = NULL;

/* environ_cb / log_cb are owned by libretro.c.  Same extern
 * pattern menu.c uses for environ_cb.  log_cb is the
 * libretro frontend's logger; using it (rather than
 * Con_Printf) routes init-time messages to whatever the
 * frontend captures -- typically a log window or stdout --
 * instead of burying them in Quake's in-game console
 * scrollback. */
extern retro_environment_t environ_cb;
extern retro_log_printf_t  log_cb;

static const render_backend_t *
rhi_pick_backend(void)
{
    struct retro_variable var;
    const char *value = NULL;

    var.key   = "tyrquake_renderer";
    var.value = NULL;
    if (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var))
        value = var.value;

    /* Explicit selection -- caller asked for the SW backend
     * by name, honour it unconditionally. */
    if (value && strcmp(value, "software") == 0)
        return &g_rhi_backend_sw;

    /* Auto.  Phase 1 has only the SW backend.  Future code
     * here will query GET_PREFERRED_HW_RENDER and try HW
     * backends in order before falling through. */
    return &g_rhi_backend_sw;
}

qboolean
rhi_init(void)
{
    const render_backend_t *candidate;

    if (g_rhi)
        return true;  /* already up; idempotent */

    candidate = rhi_pick_backend();
    if (!candidate || !candidate->init)
        return false;

    if (!candidate->init()) {
        /* Backend refused to initialize.  Future: fall back
         * to the SW backend here, since its init is
         * always trivially successful.  Phase 1 has only
         * SW so a failure here is fatal. */
        if (log_cb)
            log_cb(RETRO_LOG_ERROR, "rhi: %s backend init failed\n",
                   candidate->name ? candidate->name : "(unnamed)");
        return false;
    }

    g_rhi = candidate;
    if (log_cb)
        log_cb(RETRO_LOG_INFO, "rhi: %s backend active\n", g_rhi->name);
    return true;
}

void
rhi_shutdown(void)
{
    const render_backend_t *active = g_rhi;
    if (!active)
        return;
    /* Clear g_rhi before the backend tears down so concurrent
     * callers (there shouldn't be any in the libretro
     * single-threaded model, but be conservative) don't
     * dispatch into a half-destroyed backend. */
    g_rhi = NULL;
    if (active->shutdown)
        active->shutdown();
}
