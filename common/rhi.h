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

/* Forward declaration so the dispatch_3d_particles vtable
 * entry can be typed without dragging d_iface.h (which
 * pulls in cvar.h / mathlib.h, more than rhi.h needs) into
 * this header. */
struct particle_s;

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

    /* 2D solid-color fill intercept (Phase 4t).  The
     * last single-byte 2D path.  Used for multiplayer
     * scoreboard team-colour swatches (Sbar_DrawFrags
     * in sbar.c paints a small filled rectangle per
     * player with their top / bottom palette indices)
     * and a handful of HUD bits that draw a flat
     * rectangle of one palette colour.  SW path writes
     * vid.buffer directly with a tight `for (v) for
     * (u) dest[u] = c` loop.
     *
     * `c` is a palette index in [0, 255].  The
     * backend caches one 1x1 slot per distinct colour
     * value seen and queues an overlay quad
     * referencing it -- the slot's single byte goes
     * through the existing palette-lookup path in the
     * overlay FS so the rendered colour matches the
     * SW write byte-for-byte.  At 256 possible
     * colours there's a theoretical worst case of
     * 256 slots, but Quake in practice uses a
     * handful (the 14 team colours, plus a few HUD
     * tints); well within OVERLAY_SLOT_MAX = 128.
     *
     * NULL on SW backend / SW-only build.  Draw_Fill
     * falls through to its SW path. */
    void     (*queue_2d_fill)(int x, int y, int w, int h, int c);

    /* 2D fade-screen intercept (Phase 4t).  The other
     * last 2D path.  Draw_FadeScreen is what M_Draw
     * calls when scr_con_current == 0 and the menu is
     * up -- it writes byte 0 (palette index 0 ==
     * black) to 3 of every 4 pixels in a 4x2
     * checkerboard pattern, preserving the 4th pixel
     * (the world / HUD visible underneath) so the
     * world dims behind the menu.  SW path writes
     * vid.buffer directly.
     *
     * Backend stages a full-screen mask slot at
     * vid.width x vid.height bytes (~2 MiB at
     * 1920x1080, fits within vk_staging_size which is
     * vid.width * vid.height + VK_PALETTE_BYTES).
     * Mask content: 255 (overlay FS discards) at the
     * preserved positions, 0 (black) elsewhere.
     * Quadded full-screen at (0,0) -> (vid.width,
     * vid.height) with full UV -- CLAMP_TO_EDGE
     * sampler is fine because UV stays in [0, 1].
     * Lazy-uploaded on first call, kept for the run
     * lifetime; the mask depends only on vid.width /
     * vid.height which don't change post-init in the
     * libretro core.
     *
     * NULL on SW backend / SW-only build.  Draw_FadeScreen
     * falls through to its SW path. */
    void     (*queue_2d_fade_screen)(void);

    /* 3D particle compute dispatch (Phase 5b-02).  GPU
     * port of D_DrawParticle: backend stages the active
     * particle list into a GPU SSBO, snapshots the
     * relevant CPU SW raster state into push constants
     * (r_origin / r_pright / r_pup / r_ppn / xcenter /
     * ycenter / d_pix_* / d_vrect*_particle /
     * d_y_aspect_shift), and records a compute dispatch
     * that runs the particle rasterizer one workgroup
     * invocation per particle.  Output is palette-
     * indexed pixels written into the same compute-
     * writable target the existing palette compute
     * pass reads (vk_texture on the Vulkan backend),
     * Z-tested against a GPU side-channel of d_pzbuffer
     * uploaded once per frame ahead of the dispatch.
     *
     * `head` is the head of the active_particles linked
     * list (the same pointer R_DrawParticles iterates
     * via p = p->next).  Backends walk the list internally,
     * capping at whatever their GPU buffer can hold; any
     * overflow is silently dropped (no realloc -- the
     * backend's particle buffer is fixed-size at init,
     * dimensioned to a comfortable upper bound on Quake's
     * typical particle counts).
     *
     * Called from R_DrawParticles when both
     * g_rhi_compute_rendering is true and the active
     * backend exposes this entry; on that branch the SW
     * for-loop over D_DrawParticle is skipped -- particles
     * render entirely on the GPU.  When the entry is NULL
     * (SW backend, or HW backend without compute support)
     * or compute_rendering is disabled, R_DrawParticles
     * runs its original CPU SW path unchanged.
     *
     * NULL on SW backend / SW-only build.  Stays NULL on
     * any HW backend that doesn't (yet) implement compute
     * particle rasterization. */
    void     (*dispatch_3d_particles)(struct particle_s *head);

    /* 3D water-warp compute dispatch (Phase 5b-03).  GPU
     * port of D_WarpScreen: when the active backend
     * exposes this entry and g_rhi_compute_rendering is
     * true, R_SetupFrame suppresses r_dowarp (so the SW
     * raster renders the 3D view at full resolution into
     * vid.buffer, not into the downscaled r_warpbuffer),
     * and R_RenderView calls this entry at the point it
     * would have called D_WarpScreen.  The backend
     * records a fused warp + palette-mapping compute
     * dispatch that replaces this frame's regular
     * palette compute pass; the warp shader samples the
     * just-uploaded vk_texture with per-pixel sin
     * offsets (intsintable[(phase + pixel_coord) & 255])
     * and writes the palette-mapped RGBA result to the
     * swapchain image.
     *
     * The GPU warp runs at the full render resolution,
     * not the downscaled r_warpbuffer resolution the SW
     * path uses.  The SW downscale exists to keep the
     * per-pixel sin warp tractable on a CPU at high
     * output resolutions (the r_waterwarp_scale cvar
     * defaults to 0.5, halving each axis); the
     * downscale is a workaround, not a feature, and
     * compute hardware doesn't need it.  Full-res GPU
     * warp keeps the underlying render's detail intact.
     *
     * No arguments: the backend reads cl.time (for the
     * sin phase), scr_vrect (for the warpable region's
     * bounds), and the precomputed intsintable[]
     * (uploaded once at backend init from R_InitTurb's
     * output) directly.  All of those globals are at
     * their final values for this frame by the time
     * R_RenderView reaches its end-of-view warp call
     * site.
     *
     * NULL on SW backend / SW-only build.  When NULL,
     * R_SetupFrame falls through to its original
     * r_dowarp computation (sets the flag based on
     * water content + r_waterwarp cvar), and
     * R_RenderView's r_dowarp branch runs CPU
     * D_WarpScreen unchanged. */
    void     (*dispatch_3d_warp_screen)(void);
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

/* User preference: render the 3D view via GPU compute
 * shaders that port Quake's SW rasterizer line-for-line
 * (Phase 5 onward), rather than via traditional graphics
 * pipelines.  Generic across HW backends -- any backend
 * with compute support (Vulkan today, future D3D12 / GL
 * 4.3+ / Metal) should honor this; backends without
 * compute (SW, hypothetical future GL 3.x / D3D9 /
 * D3D11) ignore it.  Set by the libretro layer from the
 * `tyrquake_compute_rendering` core option; read by each
 * backend's init / draw_view as appropriate.
 *
 * Semantics when honored:
 *
 *   enabled  -> The backend's draw_view runs Quake's SW
 *               rasterizer in a GPU compute shader,
 *               writing the same per-span affine-textured
 *               surface-cached palette-indexed pixels the
 *               CPU SW rasterizer would write to
 *               vid.buffer, but at GPU speed.  Output is
 *               pixel-identical to the SW renderer at the
 *               same resolution (`tyrquake_resolution`).
 *
 *   disabled -> The backend's draw_view records the 3D
 *               view via traditional Vulkan / D3D12 / GL /
 *               Metal graphics pipelines with hardware
 *               rasterization, depth buffer, texture
 *               filtering, etc.  Visually different from
 *               SW (cleaner edges, filtered textures) but
 *               can be faster still.
 *
 * Default is `enabled` -- matches the existing Vulkan-
 * backend behaviour (which runs R_RenderView on the CPU
 * and gives SW-look output) more closely than the
 * graphics path, and lines up with Lib's preference
 * statement that compute is the marquee mode. */
extern qboolean g_rhi_compute_rendering;

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
