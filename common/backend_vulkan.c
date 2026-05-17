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
 * Phase 3b (this file's current state): libretro HW context
 * handshake.  backend_vk_init publishes a retro_hw_render_callback
 * via SET_HW_RENDER and returns true on acceptance.  The
 * frontend later calls backend_vk_context_reset, which retrieves
 * the retro_hw_render_interface_vulkan via GET_HW_RENDER_INTERFACE,
 * caches the instance / gpu / device / queue handles, and
 * populates a function-pointer table by feeding entry-point names
 * through the frontend-supplied vkGetInstanceProcAddr /
 * vkGetDeviceProcAddr.  Device info (deviceName, apiVersion,
 * driverVersion, vendorID, deviceID) is logged so users can see
 * which physical device the frontend negotiated.
 *
 * The vtable's per-frame functions (begin_frame, draw_view,
 * end_frame) remain no-ops in Phase 3b -- the HW context is up
 * and the function table is populated, but the backend does not
 * yet present any pixels.  When Vulkan is the active backend,
 * the frontend will dupe the last frame indefinitely, producing
 * a frozen image; this is correct behaviour for this phase and
 * resolves in Phase 3c when end_frame starts calling
 * video_cb(NULL, w, h, RETRO_HW_FRAME_BUFFER_VALID) against a
 * cleared retro_vulkan_image.
 *
 *   Phase 3c -- first frame: clear-to-color
 *     - per-frame retro_vulkan_image creation
 *     - clear command-buffer record + submit
 *     - set_image / wait_sync_index / video_cb handoff
 *     - vtable gains a present() callback so VID_Update can
 *       dispatch through the backend rather than the
 *       kind == RHI_BACKEND_SOFTWARE inline gate
 *
 *   Phase 4..N -- real geometry: world surfaces, alias models,
 *     sprites, sky, liquids, shadows; palette + colormap LUT
 *     path; dynamic lightmap update; etc.  Each lands as a
 *     separate commit against this same file.
 *
 * Convention: every internal symbol is static.  Function-pointer
 * tables (vk_fn) are file-static structs populated by
 * context_reset and torn down by context_destroy.  No libvulkan
 * link -- every Vulkan symbol is resolved through the
 * frontend-supplied loader at run time.
 */

#include "rhi.h"

#ifdef RHI_HAVE_VULKAN

#include "vulkan/vulkan_core.h"
#include "libretro_vulkan.h"
#include <string.h>

extern retro_environment_t environ_cb;
extern retro_log_printf_t  log_cb;

/* Forward decls for the context callbacks the frontend will
 * call.  Backend_vk_init's struct initialization references
 * their addresses; definitions appear below the init body. */
static void backend_vk_context_reset(void);
static void backend_vk_context_destroy(void);

/* The frontend's interface, retrieved at context_reset and
 * zeroed at context_destroy.  All Vulkan work below the
 * vtable surface gates on this being non-NULL: if the
 * frontend hasn't called context_reset yet, vk_iface is
 * NULL and the per-frame vtable functions noop. */
static const struct retro_hw_render_interface_vulkan *vk_iface;

/* Convenience cached handles -- duplicated from vk_iface for
 * brevity at use sites.  Lifetime matches vk_iface. */
static VkInstance       vk_instance;
static VkPhysicalDevice vk_gpu;
static VkDevice         vk_device;
static VkQueue          vk_queue;
static uint32_t         vk_queue_family;

/* Function-pointer table.  Populated at context_reset via
 * the frontend-supplied get_instance_proc_addr /
 * get_device_proc_addr; zeroed at context_destroy.  Grows
 * as later phases load more entry points -- Phase 3b's set
 * is the minimum needed to identify the device and drain
 * the queue at teardown.
 *
 * Naming mirrors the upstream symbol with the 'vk' prefix
 * dropped:
 *   vkGetPhysicalDeviceProperties -> vk_fn.GetPhysicalDeviceProperties */
static struct {
    PFN_vkGetInstanceProcAddr           GetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr             GetDeviceProcAddr;
    PFN_vkGetPhysicalDeviceProperties   GetPhysicalDeviceProperties;
    PFN_vkDeviceWaitIdle                DeviceWaitIdle;
} vk_fn;

/* The retro_hw_render_callback we register with the frontend.
 * Lifetime: from backend_vk_init until backend_vk_shutdown.
 * The frontend keeps a pointer to this struct (it stores
 * the callback function pointers internally for later
 * invocation), so it must outlive any context_reset /
 * context_destroy dispatch -- making it file-static is the
 * canonical pattern. */
static struct retro_hw_render_callback vk_hwrender;

#endif /* RHI_HAVE_VULKAN */

static qboolean
backend_vk_init(void)
{
#ifdef RHI_HAVE_VULKAN
    if (!environ_cb) {
        /* Should never happen -- rhi_init runs from
         * retro_load_game which is well after
         * retro_set_environment publishes environ_cb -- but
         * guard anyway. */
        return false;
    }

    memset(&vk_hwrender, 0, sizeof(vk_hwrender));
    vk_hwrender.context_type    = RETRO_HW_CONTEXT_VULKAN;
    vk_hwrender.version_major   = 1;  /* Vulkan 1.0 is the floor */
    vk_hwrender.version_minor   = 0;
    vk_hwrender.context_reset   = backend_vk_context_reset;
    vk_hwrender.context_destroy = backend_vk_context_destroy;
    vk_hwrender.cache_context   = false;  /* always rebuild on context loss */

    if (!environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &vk_hwrender)) {
        if (log_cb)
            log_cb(RETRO_LOG_INFO,
                   "rhi-vk: SET_HW_RENDER refused by frontend (no Vulkan available?)\n");
        return false;
    }

    /* SET_HW_RENDER acceptance just means the frontend has
     * recorded our request.  The actual HW context comes up
     * later when the frontend calls backend_vk_context_reset;
     * until then vk_iface is NULL and the vtable's begin /
     * draw / end pretend the backend isn't ready (they're
     * no-ops in Phase 3b regardless). */
    if (log_cb)
        log_cb(RETRO_LOG_INFO,
               "rhi-vk: SET_HW_RENDER accepted; awaiting context_reset\n");
    return true;
#else
    return false;
#endif
}

#ifdef RHI_HAVE_VULKAN

static void
backend_vk_context_reset(void)
{
    const struct retro_hw_render_interface *iface_base = NULL;
    const struct retro_hw_render_interface_vulkan *vki = NULL;
    VkPhysicalDeviceProperties props;

    if (!environ_cb)
        return;

    /* Retrieve the per-API interface.  The frontend hands
     * us the same pointer for every context_reset over the
     * same context's lifetime, so caching it is safe. */
    if (!environ_cb(RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE,
                    (void *)&iface_base) || !iface_base) {
        if (log_cb)
            log_cb(RETRO_LOG_ERROR,
                   "rhi-vk: GET_HW_RENDER_INTERFACE failed in context_reset\n");
        return;
    }
    if (iface_base->interface_type != RETRO_HW_RENDER_INTERFACE_VULKAN) {
        if (log_cb)
            log_cb(RETRO_LOG_ERROR,
                   "rhi-vk: frontend returned wrong interface_type %d (expected %d)\n",
                   (int)iface_base->interface_type,
                   (int)RETRO_HW_RENDER_INTERFACE_VULKAN);
        return;
    }
    if (iface_base->interface_version != RETRO_HW_RENDER_INTERFACE_VULKAN_VERSION) {
        /* Soft warning: the libretro spec allows older cores
         * against newer frontends so long as relevant fields
         * stay in place.  Surface the skew for triage. */
        if (log_cb)
            log_cb(RETRO_LOG_WARN,
                   "rhi-vk: interface_version mismatch: frontend %u, expected %u\n",
                   iface_base->interface_version,
                   (unsigned)RETRO_HW_RENDER_INTERFACE_VULKAN_VERSION);
    }

    vki             = (const struct retro_hw_render_interface_vulkan *)iface_base;
    vk_iface        = vki;
    vk_instance     = vki->instance;
    vk_gpu          = vki->gpu;
    vk_device       = vki->device;
    vk_queue        = vki->queue;
    vk_queue_family = vki->queue_index;

    /* The interface provides instance + device proc-address
     * resolvers directly; cache them and use them for every
     * other Vulkan symbol.  No libvulkan link -- everything
     * routes through the frontend's already-loaded loader. */
    vk_fn.GetInstanceProcAddr = vki->get_instance_proc_addr;
    vk_fn.GetDeviceProcAddr   = vki->get_device_proc_addr;

    /* Load instance-level functions through the instance
     * proc-addr resolver.  Device-level functions follow
     * via GetDeviceProcAddr. */
    if (vk_fn.GetInstanceProcAddr && vk_instance != VK_NULL_HANDLE) {
        vk_fn.GetPhysicalDeviceProperties = (PFN_vkGetPhysicalDeviceProperties)
            vk_fn.GetInstanceProcAddr(vk_instance, "vkGetPhysicalDeviceProperties");
    }
    if (vk_fn.GetDeviceProcAddr && vk_device != VK_NULL_HANDLE) {
        vk_fn.DeviceWaitIdle = (PFN_vkDeviceWaitIdle)
            vk_fn.GetDeviceProcAddr(vk_device, "vkDeviceWaitIdle");
    }

    /* Log the GPU + driver info so the user can see which
     * physical device the frontend negotiated.  Useful for
     * triage when the renderer behaves unexpectedly --
     * different vendors / drivers can expose Vulkan
     * implementations with subtly different conformance. */
    if (vk_fn.GetPhysicalDeviceProperties && vk_gpu != VK_NULL_HANDLE) {
        memset(&props, 0, sizeof(props));
        vk_fn.GetPhysicalDeviceProperties(vk_gpu, &props);
        if (log_cb)
            log_cb(RETRO_LOG_INFO,
                   "rhi-vk: device='%s' apiVersion=%u.%u.%u driverVersion=0x%08x vendor=0x%04x device=0x%04x\n",
                   props.deviceName,
                   VK_VERSION_MAJOR(props.apiVersion),
                   VK_VERSION_MINOR(props.apiVersion),
                   VK_VERSION_PATCH(props.apiVersion),
                   props.driverVersion,
                   props.vendorID,
                   props.deviceID);
    } else if (log_cb) {
        log_cb(RETRO_LOG_WARN,
               "rhi-vk: could not load GetPhysicalDeviceProperties; device identification unavailable\n");
    }

    if (log_cb)
        log_cb(RETRO_LOG_INFO,
               "rhi-vk: context up; queue_family=%u\n", vk_queue_family);
}

static void
backend_vk_context_destroy(void)
{
    /* Frontend is tearing down our context (window resize,
     * driver loss, shutdown).  All Vulkan objects we hold
     * are about to become invalid -- in later phases we'd
     * destroy any objects we created here (command pools,
     * images, etc.).  Phase 3b owns nothing of its own
     * beyond cached handles.
     *
     * Wait for any outstanding work to drain before the
     * frontend pulls the rug.  Belt-and-suspenders --
     * frontends should not be calling context_destroy with
     * the queue still busy, but doing it ourselves makes
     * the teardown deterministic. */
    if (vk_fn.DeviceWaitIdle && vk_device != VK_NULL_HANDLE)
        vk_fn.DeviceWaitIdle(vk_device);

    vk_iface        = NULL;
    vk_instance     = VK_NULL_HANDLE;
    vk_gpu          = VK_NULL_HANDLE;
    vk_device       = VK_NULL_HANDLE;
    vk_queue        = VK_NULL_HANDLE;
    vk_queue_family = 0;
    memset(&vk_fn, 0, sizeof(vk_fn));

    if (log_cb)
        log_cb(RETRO_LOG_INFO, "rhi-vk: context destroyed\n");
}

#endif /* RHI_HAVE_VULKAN */

static void
backend_vk_shutdown(void)
{
#ifdef RHI_HAVE_VULKAN
    /* Symmetric with init.  The frontend triggers
     * context_destroy on its own at game-unload, so we
     * don't need to do anything here for HW state.  Reset
     * the registered callback struct so a subsequent init
     * starts from a clean slate. */
    memset(&vk_hwrender, 0, sizeof(vk_hwrender));
#endif
}

static void
backend_vk_begin_frame(void)
{
#ifdef RHI_HAVE_VULKAN
    /* Phase 3c: acquire per-frame command buffer.
     * Phase 3b: still a no-op. */
#endif
}

static void
backend_vk_draw_view(const refdef_t *rd)
{
    (void)rd;
#ifdef RHI_HAVE_VULKAN
    /* Phase 3c: record clear-to-color into the per-frame
     * image.
     * Phase 4+: real scene rendering. */
#endif
}

static void
backend_vk_end_frame(void)
{
#ifdef RHI_HAVE_VULKAN
    /* Phase 3c: submit, set_image, hand back via
     * video_cb(NULL, w, h, RETRO_HW_FRAME_BUFFER_VALID). */
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
