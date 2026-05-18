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

    /* 2D pic intercept (Phase 4k, Phase 4l).  Backends
     * that render HUD / menu pics natively populate this;
     * SW backend leaves it NULL and Draw_Pic /
     * Draw_TransPic fall through to their original
     * memcpy-into-vid.buffer paths.  When non-NULL the
     * call replaces the SW path: Draw_Pic / Draw_TransPic
     * forward to queue_2d_pic and return; the SW
     * rasterisation into vid.buffer is skipped.  Phase
     * 4k double-rendered for one commit to validate the
     * Vulkan side; Phase 4l makes the suppression
     * unconditional so the compute upload of vid.buffer
     * no longer carries redundant HUD pixels.
     *
     * x, y are in vid.width / vid.height screen-space (the
     * same coordinates Draw_Pic gets); the backend converts
     * to NDC internally.  pic->width / pic->height define
     * the rectangle, pic->data is width*height bytes of
     * palette indices.  Index 255 is Quake's transparency
     * key (d_iface.h: TRANSPARENT_COLOR) -- the overlay
     * FS discards on it, so the same intercept handles
     * Draw_TransPic correctly without a separate entry. */
    void     (*queue_2d_pic)(int x, int y, const qpic_t *pic);

    /* 2D character intercept (Phase 4o).  Backends that
     * render console / menu / HUD text natively populate
     * this; SW backend leaves it NULL and Draw_Character /
     * Draw_CharacterScaled fall through to their original
     * memcpy-into-vid.buffer paths.  When non-NULL the
     * call replaces the SW path the same way queue_2d_pic
     * does for pics: forward + return; no SW write.
     *
     * x, y are screen-space; num is the character index in
     * the 16x16 conchars atlas (after & 255); scale is the
     * on-screen pixel scale, where the drawn rect is
     * (8 * scale) x (8 * scale).  The backend is
     * responsible for handling conchars's "byte 0 means
     * transparent" semantics (different from the pic
     * path's "byte 255 means transparent"). */
    void     (*queue_2d_char)(int x, int y, int num, int scale);

    /* 2D scaled-pic intercept (Phase 4q).  Same role as
     * queue_2d_pic but for the scale > 1 path of
     * Draw_PicScaled / Draw_TransPicScaled.  Before this
     * intercept those functions ran a triple-nested
     * memcpy loop (`for (v) for (u) for (sx)`) that
     * stretched the pic into vid.buffer at scale*scale
     * cost per source pixel -- expensive in CPU and a
     * quality hit because the result then went through
     * the compute upscale, while scale == 1 pics handed
     * off to queue_2d_pic rendered crisply via the
     * overlay shader.  This intercept unifies the path:
     * everything 2D goes through the overlay queue at
     * native resolution, vid.buffer holds only the 3D
     * view (and whatever SW-direct paths still remain --
     * Draw_TransPicTranslateScaled for the multiplayer
     * color preview, Draw_Fill, Draw_FadeScreen,
     * Draw_ConsoleBackground).
     *
     * x, y are screen-space (the post-scaling pixel
     * coords already computed by the caller, same as
     * the SW path used as the dest base).  pic is the
     * hunk-allocated qpic_t whose pointer keys the
     * slot cache the same way queue_2d_pic uses it.
     * scale is the integer multiplier; the resulting
     * on-screen rect is (pic->width * scale) x
     * (pic->height * scale).  When non-NULL,
     * Draw_PicScaled / Draw_TransPicScaled forward and
     * return -- no SW write.  When NULL (SW backend),
     * the SW stretched-memcpy runs unchanged.
     *
     * The overlay fragment shader's byte-255 discard
     * handles both opaque (Draw_PicScaled, no 255 in
     * pic data) and transparent (Draw_TransPicScaled,
     * uses TRANSPARENT_COLOR == 255) callers without
     * needing a separate transparent variant. */
    void     (*queue_2d_pic_scaled)(int x, int y,
                                    const qpic_t *pic, int scale);

    /* 2D console-background intercept (Phase 4r,
     * originally tried as Phase 4p / 67c8f47 and reverted
     * because at that time the scale > 1 pic path still
     * SW-wrote vid.buffer; M_Draw's menu pics landed
     * there and the conback overlay quad covered them.
     * After Phase 4q above moves the scale > 1 path into
     * the overlay queue too, the intercept becomes safe:
     * M_Draw queues conback then menu pics, so the
     * conback lands underneath them in queue order and
     * the menu pics correctly draw on top).
     *
     * Draw_ConsoleBackground is the one place in the SW
     * 2D pipeline that writes vid.buffer directly with a
     * stretched-bottom-portion sample of a pic
     * (gfx/conback.lmp) rather than through Draw_Pic /
     * Draw_PicScaled.  Phase 4l / 4o / 4q left it on the
     * SW path; this entry routes it through the overlay
     * queue too.
     *
     * `lines` is the on-screen height of the backdrop
     * (the scr_con_current value the SW path uses).  The
     * backend stretches the bottom `lines / vid.height`
     * fraction of the pic up to a (vid.width, lines)
     * on-screen rect, queued in call order so earlier
     * overlay pushes (Sbar HUD entries from
     * Sbar_Draw, etc.) are covered, and later overlay
     * pushes (console text chars from Con_DrawConsole's
     * character loop, menu pics from M_Draw) correctly
     * draw on top.  Caller passes the pre-
     * Draw_ConbackString-modified conback (version-
     * string baked in) so the cache captures the right
     * pixels on first upload. */
    void     (*queue_2d_console_background)(int lines, const qpic_t *pic);

    /* 2D translated scaled-pic intercept (Phase 4s).
     * Handles the one caller of Draw_TransPicTranslate
     * Scaled left as 'out of scope' by Phase 4q: the
     * multiplayer Setup menu's player-model colour
     * preview (M_DrawTransPicTranslate in menu.c, the
     * only non-scale-1 caller of Draw_TransPicTranslate
     * Scaled).  Phase 4q routed the rest of the menu
     * pics through the overlay queue, and Phase 4r
     * routed Draw_ConsoleBackground (the menu backdrop)
     * through too; the player-preview pic, still SW-
     * writing vid.buffer, then gets covered by the
     * conback overlay quad at composite time and never
     * shows.
     *
     * The translation table modifies the source bytes
     * before sampling (re-maps the TOP_RANGE / BOTTOM_
     * RANGE palette windows so the player sprite picks
     * up the user-selected shirt / pants colours).
     * Because the pic data and translation table both
     * vary per call (the same pic stays at the same
     * qpic_t pointer but its visible pixels change as
     * the user navigates the colour picker), the
     * normal slot-cache invariant 'cached pixels match
     * the qpic_t key' doesn't hold.
     *
     * Backends should dedicate one slot to this caller
     * (sentinel key distinct from any qpic_t and from
     * the conchars key &draw_chars), translate the
     * source bytes through `translation` into a
     * scratch buffer (preserving byte 255 == TRANSPARENT
     * _COLOR so the overlay FS discard works the same
     * as for Draw_TransPicScaled), and on each call
     * either upload to the dedicated slot on the
     * first call or refresh its pixels in place on
     * subsequent calls.  Queued overlay quad uses
     * the (pic->width * scale, pic->height * scale)
     * on-screen rect, full UV, same as queue_2d_pic_
     * scaled.
     *
     * NULL on SW backend / SW-only build -- the SW
     * stretched-memcpy-with-per-byte-translation loop
     * in Draw_TransPicTranslateScaled runs unchanged. */
    void     (*queue_2d_pic_translate_scaled)(int x, int y,
                                              const qpic_t *pic,
                                              const byte *translation,
                                              int scale);
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
