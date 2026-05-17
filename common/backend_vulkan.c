/*
 * Vulkan renderer backend.
 *
 * Single-TU implementation.  All internal state is file-static;
 * the only exported symbol is the g_rhi_backend_vk vtable
 * instance.  The vtable is exposed unconditionally so rhi.c can
 * take its address regardless of whether the build flag is set;
 * its init() returns false when RHI_HAVE_VULKAN is undefined,
 * which causes rhi.c's auto path to fall through to the software
 * backend.
 *
 * Build flag:
 *   RHI_HAVE_VULKAN
 *     When defined, the file includes vulkan_core.h and
 *     libretro_vulkan.h (vendored under deps/vulkan/include/,
 *     see deps/vulkan/README.md) and performs the libretro HW
 *     context handshake + actual Vulkan setup.  When undefined,
 *     the file compiles as a no-op vtable instance with init
 *     returning false; the build has no dependency on Vulkan
 *     headers in the default configuration.
 *
 *     The build flag is named RHI_HAVE_VULKAN rather than
 *     RHI_BACKEND_VULKAN to avoid a textual collision with the
 *     enum value of the same name in rhi.h (which is set to a
 *     small integer for the rhi_backend_kind enum -- a
 *     -DRHI_BACKEND_VULKAN=1 macro at compile time would
 *     replace the enum identifier with the literal 1).
 *
 * Phase 3a (this file's current state): scaffolding only.  All
 * vtable functions are stubs.  init() unconditionally returns
 * false so the backend is never picked; auto in rhi.c falls
 * through to SW.  Subsequent phases will fill in:
 *
 *   Phase 3b -- libretro HW context handshake:
 *     - SET_HW_RENDER with RETRO_HW_CONTEXT_VULKAN
 *     - context_reset / context_destroy callbacks
 *     - GET_HW_RENDER_INTERFACE retrieval, function-pointer
 *       table population via vkGetInstanceProcAddr
 *
 *   Phase 3c -- first frame:
 *     - command pool / command buffer allocation
 *     - clear-to-color recorded into the per-frame retro_vulkan_image
 *     - semaphore signalling, retro_video_refresh handoff
 *
 *   Phase 4..N -- world / alias / sprite / sky / etc. pipelines,
 *   asset upload, lightmap update, palette + colormap LUT path,
 *   each landed as a separate commit against this same file.
 *
 * Convention: every internal symbol is static.  Function-pointer
 * tables (vk_*) are file-static structs populated by context_reset
 * and torn down by context_destroy.
 */

#include "rhi.h"

#ifdef RHI_HAVE_VULKAN

#include "vulkan/vulkan_core.h"
#include "libretro_vulkan.h"

/* Function-pointer table, populated by context_reset from the
 * frontend-supplied vkGetInstanceProcAddr.  Filled in Phase 3b. */
static struct {
    int placeholder;  /* Replaced with real vk*_fn pointers. */
} vk_fn;

/* The frontend's HW render interface, retrieved via
 * GET_HW_RENDER_INTERFACE inside context_reset. */
static const struct retro_hw_render_interface_vulkan *vk_iface;

#endif /* RHI_HAVE_VULKAN */

static qboolean
backend_vk_init(void)
{
#ifdef RHI_HAVE_VULKAN
    /* Phase 3b will:
     *   - allocate the retro_hw_render_callback struct
     *   - set type = RETRO_HW_CONTEXT_VULKAN,
     *     version_major = 1, version_minor = 0
     *   - install context_reset / context_destroy callbacks
     *   - call environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &cb)
     *   - return cb's success status; on success the frontend
     *     will later call context_reset, which is where the
     *     real work happens.
     *
     * For now the file builds with the flag on but the backend
     * still refuses to come up so auto stays on SW. */
    return false;
#else
    /* Backend disabled at build time. */
    return false;
#endif
}

static void
backend_vk_shutdown(void)
{
#ifdef RHI_HAVE_VULKAN
    /* Phase 3b: tear down the HW context, release the
     * retro_hw_render_callback, zero the vk_fn table. */
#endif
}

static void
backend_vk_begin_frame(void)
{
#ifdef RHI_HAVE_VULKAN
    /* Phase 3c: acquire the per-frame retro_vulkan_image from
     * the frontend, transition its layout to GENERAL, allocate
     * a command buffer from the per-frame pool. */
#endif
}

static void
backend_vk_draw_view(const refdef_t *rd)
{
    (void)rd;
#ifdef RHI_HAVE_VULKAN
    /* Phase 3c: record clear-to-color.
     * Phase 4+: real scene rendering. */
#endif
}

static void
backend_vk_end_frame(void)
{
#ifdef RHI_HAVE_VULKAN
    /* Phase 3c: transition the image to SHADER_READ_ONLY,
     * end the command buffer, submit signalling the
     * frontend-supplied semaphore, call
     * set_image / set_command_buffers on the interface, hand
     * back to the frontend via video_cb with
     * RETRO_HW_FRAME_BUFFER_VALID. */
#endif
}

const render_backend_t g_rhi_backend_vk = {
    "vulkan",
    RHI_BACKEND_VULKAN,
    backend_vk_init,
    backend_vk_shutdown,
    backend_vk_begin_frame,
    backend_vk_draw_view,
    backend_vk_end_frame
};
