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
 * Phase 3c (this file's current state): clear-to-color first
 * frame.  context_reset, in addition to the Phase 3b handshake,
 * now also creates a persistent render-target image (sized to
 * the libretro width x height globals), allocates and binds
 * device memory, creates an image view, and pre-records a
 * single command buffer that clears the image to a recognizable
 * solid colour (cornflower blue) and transitions the layout to
 * SHADER_READ_ONLY_OPTIMAL so the frontend can sample it.
 *
 * Every frame, backend_vk_end_frame submits that pre-recorded
 * command buffer, waits for the queue to go idle (the simplest
 * correct synchronisation pattern -- no fences, no semaphores,
 * no double-buffering), hands the cleared image to the frontend
 * via the interface's set_image, calls video_cb with
 * RETRO_HW_FRAME_BUFFER_VALID, and sets did_flip so retro_run's
 * dupe path is suppressed.
 *
 * Visual end state for this phase: selecting 'Renderer = Vulkan'
 * produces a solid cornflower-blue framebuffer instead of the
 * frozen black of Phase 3b.  This proves end-to-end frame
 * delivery through the HW context -- image creation, command
 * recording, queue submission, image handoff, and frontend
 * presentation are all working.  draw_view is still a no-op;
 * actual geometry comes in Phase 4+.
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

extern retro_environment_t    environ_cb;
extern retro_log_printf_t     log_cb;
extern retro_video_refresh_t  video_cb;     /* libretro.c */
extern bool                   did_flip;     /* libretro.c -- set true after end_frame
                                             * to suppress retro_run's dupe path */
extern unsigned               width;        /* libretro.c -- libretro framebuffer width */
extern unsigned               height;       /* libretro.c -- libretro framebuffer height */

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

/* Per-context Vulkan objects.  Created during context_reset
 * (after the function-pointer table is populated), destroyed
 * during context_destroy.  All zeroed via VK_NULL_HANDLE at
 * teardown.
 *
 * vk_resources_ready is true only after every object has been
 * successfully created and the clear command buffer has been
 * recorded; end_frame gates on it.  If any creation step fails
 * mid-context_reset, the flag stays false and end_frame
 * silently no-ops, falling through to the dupe path in
 * retro_run -- the same visual end state as Phase 3b. */
static qboolean         vk_resources_ready;
static VkImage          vk_image;
static VkDeviceMemory   vk_image_memory;
static VkImageView      vk_image_view;
static VkCommandPool    vk_cmd_pool;
static VkCommandBuffer  vk_cmd_buffer;

/* The retro_vulkan_image we hand to set_image.  Built once
 * after the image view is created (the contained
 * VkImageViewCreateInfo is what the frontend uses to know
 * how to sample our image).  Lifetime: until context_destroy. */
static struct retro_vulkan_image vk_retro_image;

/* Function-pointer table.  Populated at context_reset via
 * the frontend-supplied get_instance_proc_addr /
 * get_device_proc_addr; zeroed at context_destroy.  Grows
 * as later phases load more entry points.
 *
 * Naming mirrors the upstream symbol with the 'vk' prefix
 * dropped:
 *   vkGetPhysicalDeviceProperties -> vk_fn.GetPhysicalDeviceProperties */
static struct {
    /* Instance-level (loaded via GetInstanceProcAddr) */
    PFN_vkGetInstanceProcAddr             GetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr               GetDeviceProcAddr;
    PFN_vkGetPhysicalDeviceProperties     GetPhysicalDeviceProperties;
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties;

    /* Device-level (loaded via GetDeviceProcAddr) */
    PFN_vkDeviceWaitIdle                  DeviceWaitIdle;
    PFN_vkCreateImage                     CreateImage;
    PFN_vkDestroyImage                    DestroyImage;
    PFN_vkGetImageMemoryRequirements      GetImageMemoryRequirements;
    PFN_vkAllocateMemory                  AllocateMemory;
    PFN_vkFreeMemory                      FreeMemory;
    PFN_vkBindImageMemory                 BindImageMemory;
    PFN_vkCreateImageView                 CreateImageView;
    PFN_vkDestroyImageView                DestroyImageView;
    PFN_vkCreateCommandPool               CreateCommandPool;
    PFN_vkDestroyCommandPool              DestroyCommandPool;
    PFN_vkAllocateCommandBuffers          AllocateCommandBuffers;
    PFN_vkFreeCommandBuffers              FreeCommandBuffers;
    PFN_vkBeginCommandBuffer              BeginCommandBuffer;
    PFN_vkEndCommandBuffer                EndCommandBuffer;
    PFN_vkCmdPipelineBarrier              CmdPipelineBarrier;
    PFN_vkCmdClearColorImage              CmdClearColorImage;
    PFN_vkQueueSubmit                     QueueSubmit;
    PFN_vkQueueWaitIdle                   QueueWaitIdle;
} vk_fn;

/* The retro_hw_render_callback we register with the frontend.
 * Lifetime: from backend_vk_init until backend_vk_shutdown.
 * The frontend keeps a pointer to this struct so it must
 * outlive any context_reset / context_destroy dispatch --
 * making it file-static is the canonical pattern. */
static struct retro_hw_render_callback vk_hwrender;

#endif /* RHI_HAVE_VULKAN */

static qboolean
backend_vk_init(void)
{
#ifdef RHI_HAVE_VULKAN
    if (!environ_cb)
        return false;

    memset(&vk_hwrender, 0, sizeof(vk_hwrender));
    vk_hwrender.context_type    = RETRO_HW_CONTEXT_VULKAN;
    vk_hwrender.version_major   = 1;  /* Vulkan 1.0 is the floor */
    vk_hwrender.version_minor   = 0;
    vk_hwrender.context_reset   = backend_vk_context_reset;
    vk_hwrender.context_destroy = backend_vk_context_destroy;
    vk_hwrender.cache_context   = false;

    if (!environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &vk_hwrender)) {
        if (log_cb)
            log_cb(RETRO_LOG_INFO,
                   "rhi-vk: SET_HW_RENDER refused by frontend (no Vulkan available?)\n");
        return false;
    }

    if (log_cb)
        log_cb(RETRO_LOG_INFO,
               "rhi-vk: SET_HW_RENDER accepted; awaiting context_reset\n");
    return true;
#else
    return false;
#endif
}

#ifdef RHI_HAVE_VULKAN

/*
 * Find a memory type that satisfies the type_bits requirements
 * mask (from GetImageMemoryRequirements) and exposes every flag
 * in required_props.  Standard pattern -- walk the
 * memoryTypes[] array and pick the first one that matches both.
 * Returns 0xFFFFFFFF if none found.
 */
static uint32_t
backend_vk_find_memory_type(uint32_t type_bits,
                            VkMemoryPropertyFlags required_props)
{
    VkPhysicalDeviceMemoryProperties mem_props;
    uint32_t i;

    if (!vk_fn.GetPhysicalDeviceMemoryProperties || vk_gpu == VK_NULL_HANDLE)
        return 0xFFFFFFFFu;

    memset(&mem_props, 0, sizeof(mem_props));
    vk_fn.GetPhysicalDeviceMemoryProperties(vk_gpu, &mem_props);

    for (i = 0; i < mem_props.memoryTypeCount; ++i) {
        if (!(type_bits & (1u << i)))
            continue;
        if ((mem_props.memoryTypes[i].propertyFlags & required_props)
            != required_props)
            continue;
        return i;
    }
    return 0xFFFFFFFFu;
}

/*
 * Create the per-context render target (image + memory + view)
 * and the command-buffer infrastructure (pool + one primary
 * buffer), then pre-record a clear-to-color command that runs
 * every frame.  On any failure rolls back and leaves
 * vk_resources_ready false; end_frame then no-ops and the
 * frontend falls back to dupe.
 */
static qboolean
backend_vk_create_resources(void)
{
    VkImageCreateInfo               image_ci;
    VkMemoryRequirements            mem_req;
    VkMemoryAllocateInfo            alloc_info;
    VkImageViewCreateInfo           view_ci;
    VkCommandPoolCreateInfo         pool_ci;
    VkCommandBufferAllocateInfo     cmd_alloc;
    VkCommandBufferBeginInfo        begin_info;
    VkImageMemoryBarrier            to_dst;
    VkImageMemoryBarrier            to_shader_read;
    VkClearColorValue               clear_color;
    VkImageSubresourceRange         range;
    uint32_t                        mem_type;
    VkResult                        r;

    /* Render-target image.  R8G8B8A8_UNORM with MUTABLE_FORMAT
     * (libretro spec recommends mutable for 8-bit formats so
     * the frontend can reinterpret as sRGB if it wants).
     * Usage covers our needs (TRANSFER_DST for the clear,
     * SAMPLED for the frontend's read) plus TRANSFER_SRC
     * which the libretro Vulkan interface docs call out as
     * required for set_image. */
    memset(&image_ci, 0, sizeof(image_ci));
    image_ci.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_ci.flags         = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    image_ci.imageType     = VK_IMAGE_TYPE_2D;
    image_ci.format        = VK_FORMAT_R8G8B8A8_UNORM;
    image_ci.extent.width  = width;
    image_ci.extent.height = height;
    image_ci.extent.depth  = 1;
    image_ci.mipLevels     = 1;
    image_ci.arrayLayers   = 1;
    image_ci.samples       = VK_SAMPLE_COUNT_1_BIT;
    image_ci.tiling        = VK_IMAGE_TILING_OPTIMAL;
    image_ci.usage         = VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                           | VK_IMAGE_USAGE_TRANSFER_DST_BIT
                           | VK_IMAGE_USAGE_SAMPLED_BIT;
    image_ci.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    image_ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    r = vk_fn.CreateImage(vk_device, &image_ci, NULL, &vk_image);
    if (r != VK_SUCCESS) {
        if (log_cb)
            log_cb(RETRO_LOG_ERROR, "rhi-vk: CreateImage failed (%d)\n", (int)r);
        return false;
    }

    /* Allocate device-local memory and bind it to the image. */
    memset(&mem_req, 0, sizeof(mem_req));
    vk_fn.GetImageMemoryRequirements(vk_device, vk_image, &mem_req);

    mem_type = backend_vk_find_memory_type(mem_req.memoryTypeBits,
                                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mem_type == 0xFFFFFFFFu) {
        if (log_cb)
            log_cb(RETRO_LOG_ERROR,
                   "rhi-vk: no device-local memory type for render target\n");
        return false;
    }

    memset(&alloc_info, 0, sizeof(alloc_info));
    alloc_info.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize  = mem_req.size;
    alloc_info.memoryTypeIndex = mem_type;

    r = vk_fn.AllocateMemory(vk_device, &alloc_info, NULL, &vk_image_memory);
    if (r != VK_SUCCESS) {
        if (log_cb)
            log_cb(RETRO_LOG_ERROR,
                   "rhi-vk: AllocateMemory %lu bytes failed (%d)\n",
                   (unsigned long)mem_req.size, (int)r);
        return false;
    }

    r = vk_fn.BindImageMemory(vk_device, vk_image, vk_image_memory, 0);
    if (r != VK_SUCCESS) {
        if (log_cb)
            log_cb(RETRO_LOG_ERROR, "rhi-vk: BindImageMemory failed (%d)\n", (int)r);
        return false;
    }

    /* Image view (2D, COLOR aspect).  The view -- and the
     * create_info we built it from -- both go into the
     * retro_vulkan_image we hand to set_image.  The libretro
     * spec says the frontend may reinterpret pNext/format
     * etc., so the create_info has to outlive the
     * video_refresh call.  Storing it inside vk_retro_image
     * (which is file-static) satisfies that lifetime. */
    memset(&view_ci, 0, sizeof(view_ci));
    view_ci.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_ci.image            = vk_image;
    view_ci.viewType         = VK_IMAGE_VIEW_TYPE_2D;
    view_ci.format           = VK_FORMAT_R8G8B8A8_UNORM;
    view_ci.components.r     = VK_COMPONENT_SWIZZLE_IDENTITY;
    view_ci.components.g     = VK_COMPONENT_SWIZZLE_IDENTITY;
    view_ci.components.b     = VK_COMPONENT_SWIZZLE_IDENTITY;
    view_ci.components.a     = VK_COMPONENT_SWIZZLE_IDENTITY;
    view_ci.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    view_ci.subresourceRange.baseMipLevel   = 0;
    view_ci.subresourceRange.levelCount     = 1;
    view_ci.subresourceRange.baseArrayLayer = 0;
    view_ci.subresourceRange.layerCount     = 1;

    r = vk_fn.CreateImageView(vk_device, &view_ci, NULL, &vk_image_view);
    if (r != VK_SUCCESS) {
        if (log_cb)
            log_cb(RETRO_LOG_ERROR, "rhi-vk: CreateImageView failed (%d)\n", (int)r);
        return false;
    }

    memset(&vk_retro_image, 0, sizeof(vk_retro_image));
    vk_retro_image.image_view   = vk_image_view;
    vk_retro_image.image_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vk_retro_image.create_info  = view_ci;

    /* Command pool from the queue family the frontend gave us. */
    memset(&pool_ci, 0, sizeof(pool_ci));
    pool_ci.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_ci.flags            = 0;  /* no per-frame reset needed -- single
                                      * static command buffer */
    pool_ci.queueFamilyIndex = vk_queue_family;

    r = vk_fn.CreateCommandPool(vk_device, &pool_ci, NULL, &vk_cmd_pool);
    if (r != VK_SUCCESS) {
        if (log_cb)
            log_cb(RETRO_LOG_ERROR, "rhi-vk: CreateCommandPool failed (%d)\n", (int)r);
        return false;
    }

    /* One primary command buffer.  Recorded once below; re-
     * submitted every frame in end_frame. */
    memset(&cmd_alloc, 0, sizeof(cmd_alloc));
    cmd_alloc.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmd_alloc.commandPool        = vk_cmd_pool;
    cmd_alloc.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_alloc.commandBufferCount = 1;

    r = vk_fn.AllocateCommandBuffers(vk_device, &cmd_alloc, &vk_cmd_buffer);
    if (r != VK_SUCCESS) {
        if (log_cb)
            log_cb(RETRO_LOG_ERROR,
                   "rhi-vk: AllocateCommandBuffers failed (%d)\n", (int)r);
        return false;
    }

    /* Record the clear-to-color command buffer.  Run once per
     * frame in end_frame.  Sequence:
     *   1) Barrier UNDEFINED -> TRANSFER_DST_OPTIMAL
     *   2) CmdClearColorImage to a recognisable solid colour
     *   3) Barrier TRANSFER_DST_OPTIMAL -> SHADER_READ_ONLY_OPTIMAL
     * Layout at end of submit is SHADER_READ_ONLY_OPTIMAL,
     * matching what we report to the frontend in
     * vk_retro_image.image_layout. */
    memset(&begin_info, 0, sizeof(begin_info));
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
    /* SIMULTANEOUS_USE so re-submitting before a previous
     * submission has completed is legal.  We follow each
     * submit with QueueWaitIdle so this is technically
     * unnecessary today, but keeping the flag set lets a
     * future phase remove the wait without re-recording. */

    r = vk_fn.BeginCommandBuffer(vk_cmd_buffer, &begin_info);
    if (r != VK_SUCCESS) {
        if (log_cb)
            log_cb(RETRO_LOG_ERROR, "rhi-vk: BeginCommandBuffer failed (%d)\n", (int)r);
        return false;
    }

    range.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    range.baseMipLevel   = 0;
    range.levelCount     = 1;
    range.baseArrayLayer = 0;
    range.layerCount     = 1;

    memset(&to_dst, 0, sizeof(to_dst));
    to_dst.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_dst.srcAccessMask       = 0;
    to_dst.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_dst.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
    to_dst.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.image               = vk_image;
    to_dst.subresourceRange    = range;

    vk_fn.CmdPipelineBarrier(vk_cmd_buffer,
                             VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0,
                             0, NULL,
                             0, NULL,
                             1, &to_dst);

    /* Cornflower blue: easily distinguished from black, not
     * a colour Quake content itself emits in any quantity. */
    clear_color.float32[0] = 0.39f;
    clear_color.float32[1] = 0.58f;
    clear_color.float32[2] = 0.93f;
    clear_color.float32[3] = 1.0f;

    vk_fn.CmdClearColorImage(vk_cmd_buffer,
                             vk_image,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             &clear_color,
                             1, &range);

    memset(&to_shader_read, 0, sizeof(to_shader_read));
    to_shader_read.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_shader_read.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_shader_read.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
    to_shader_read.oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_shader_read.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    to_shader_read.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_shader_read.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_shader_read.image               = vk_image;
    to_shader_read.subresourceRange    = range;

    vk_fn.CmdPipelineBarrier(vk_cmd_buffer,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0,
                             0, NULL,
                             0, NULL,
                             1, &to_shader_read);

    r = vk_fn.EndCommandBuffer(vk_cmd_buffer);
    if (r != VK_SUCCESS) {
        if (log_cb)
            log_cb(RETRO_LOG_ERROR, "rhi-vk: EndCommandBuffer failed (%d)\n", (int)r);
        return false;
    }

    vk_resources_ready = true;
    if (log_cb)
        log_cb(RETRO_LOG_INFO,
               "rhi-vk: render target %ux%u ready; clear command buffer pre-recorded\n",
               width, height);
    return true;
}

/*
 * Tear down everything backend_vk_create_resources built.  Safe
 * to call when resources are partially or fully constructed --
 * each step is gated on its corresponding handle being non-null.
 * Always called from context_destroy (and from
 * create_resources's own rollback if a step failed).
 */
static void
backend_vk_destroy_resources(void)
{
    /* Drain the queue first so nothing is still referencing
     * the objects we're about to destroy.  DeviceWaitIdle is
     * a stronger barrier than QueueWaitIdle (covers every
     * queue), which is what we want at teardown. */
    if (vk_fn.DeviceWaitIdle && vk_device != VK_NULL_HANDLE)
        vk_fn.DeviceWaitIdle(vk_device);

    if (vk_cmd_pool != VK_NULL_HANDLE) {
        /* DestroyCommandPool frees every buffer allocated
         * from it; no separate FreeCommandBuffers call
         * needed. */
        if (vk_fn.DestroyCommandPool)
            vk_fn.DestroyCommandPool(vk_device, vk_cmd_pool, NULL);
        vk_cmd_pool   = VK_NULL_HANDLE;
        vk_cmd_buffer = VK_NULL_HANDLE;
    }
    if (vk_image_view != VK_NULL_HANDLE) {
        if (vk_fn.DestroyImageView)
            vk_fn.DestroyImageView(vk_device, vk_image_view, NULL);
        vk_image_view = VK_NULL_HANDLE;
    }
    if (vk_image != VK_NULL_HANDLE) {
        if (vk_fn.DestroyImage)
            vk_fn.DestroyImage(vk_device, vk_image, NULL);
        vk_image = VK_NULL_HANDLE;
    }
    if (vk_image_memory != VK_NULL_HANDLE) {
        if (vk_fn.FreeMemory)
            vk_fn.FreeMemory(vk_device, vk_image_memory, NULL);
        vk_image_memory = VK_NULL_HANDLE;
    }
    memset(&vk_retro_image, 0, sizeof(vk_retro_image));
    vk_resources_ready = false;
}

static void
backend_vk_load_fn(void)
{
    /* Instance-level loads. */
    if (vk_fn.GetInstanceProcAddr && vk_instance != VK_NULL_HANDLE) {
        vk_fn.GetPhysicalDeviceProperties = (PFN_vkGetPhysicalDeviceProperties)
            vk_fn.GetInstanceProcAddr(vk_instance, "vkGetPhysicalDeviceProperties");
        vk_fn.GetPhysicalDeviceMemoryProperties = (PFN_vkGetPhysicalDeviceMemoryProperties)
            vk_fn.GetInstanceProcAddr(vk_instance, "vkGetPhysicalDeviceMemoryProperties");
    }

    /* Device-level loads. */
    if (vk_fn.GetDeviceProcAddr && vk_device != VK_NULL_HANDLE) {
        #define LOAD_DEV(name) \
            vk_fn.name = (PFN_vk##name) \
                vk_fn.GetDeviceProcAddr(vk_device, "vk" #name)
        LOAD_DEV(DeviceWaitIdle);
        LOAD_DEV(CreateImage);
        LOAD_DEV(DestroyImage);
        LOAD_DEV(GetImageMemoryRequirements);
        LOAD_DEV(AllocateMemory);
        LOAD_DEV(FreeMemory);
        LOAD_DEV(BindImageMemory);
        LOAD_DEV(CreateImageView);
        LOAD_DEV(DestroyImageView);
        LOAD_DEV(CreateCommandPool);
        LOAD_DEV(DestroyCommandPool);
        LOAD_DEV(AllocateCommandBuffers);
        LOAD_DEV(FreeCommandBuffers);
        LOAD_DEV(BeginCommandBuffer);
        LOAD_DEV(EndCommandBuffer);
        LOAD_DEV(CmdPipelineBarrier);
        LOAD_DEV(CmdClearColorImage);
        LOAD_DEV(QueueSubmit);
        LOAD_DEV(QueueWaitIdle);
        #undef LOAD_DEV
    }
}

static void
backend_vk_context_reset(void)
{
    const struct retro_hw_render_interface *iface_base = NULL;
    const struct retro_hw_render_interface_vulkan *vki = NULL;
    VkPhysicalDeviceProperties props;

    if (!environ_cb)
        return;

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

    vk_fn.GetInstanceProcAddr = vki->get_instance_proc_addr;
    vk_fn.GetDeviceProcAddr   = vki->get_device_proc_addr;

    backend_vk_load_fn();

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

    /* Stand up the per-context Vulkan resources.  On failure
     * roll back so context_destroy doesn't have to handle
     * a half-constructed state -- create_resources logged
     * the specific step that failed. */
    if (!backend_vk_create_resources()) {
        backend_vk_destroy_resources();
        if (log_cb)
            log_cb(RETRO_LOG_ERROR,
                   "rhi-vk: resource creation failed; end_frame will no-op\n");
    }
}

static void
backend_vk_context_destroy(void)
{
    backend_vk_destroy_resources();

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
    memset(&vk_hwrender, 0, sizeof(vk_hwrender));
#endif
}

static void
backend_vk_begin_frame(void)
{
#ifdef RHI_HAVE_VULKAN
    /* Phase 3c: nothing to do.  The clear command buffer is
     * pre-recorded once and re-submitted every frame, so
     * no per-frame setup is needed.  Future phases that
     * record fresh commands per frame will reset the
     * command buffer here. */
#endif
}

static void
backend_vk_draw_view(const refdef_t *rd)
{
    (void)rd;
#ifdef RHI_HAVE_VULKAN
    /* Phase 3c: nothing to record.  The pre-recorded clear
     * command buffer is the entire per-frame work.
     * Phase 4+ will record geometry commands here. */
#endif
}

static void
backend_vk_end_frame(void)
{
#ifdef RHI_HAVE_VULKAN
    VkSubmitInfo si;
    VkResult     r;

    if (!vk_iface || !vk_resources_ready)
        return;

    /* Submit the pre-recorded clear command buffer. */
    memset(&si, 0, sizeof(si));
    si.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers    = &vk_cmd_buffer;

    r = vk_fn.QueueSubmit(vk_queue, 1, &si, VK_NULL_HANDLE);
    if (r != VK_SUCCESS) {
        if (log_cb)
            log_cb(RETRO_LOG_ERROR, "rhi-vk: QueueSubmit failed (%d)\n", (int)r);
        return;
    }

    /* QueueWaitIdle is the simplest correct synchronisation.
     * Drains the queue so the image is guaranteed ready by
     * the time set_image hands it to the frontend; no
     * semaphores or fences needed.  This is conservative
     * and throttles the GPU to one frame at a time -- a
     * future phase introduces a fence-based ring so the
     * GPU can run ahead. */
    vk_fn.QueueWaitIdle(vk_queue);

    /* Hand the image to the frontend.  src_queue_family
     * matches the queue we submitted from, so no ownership
     * transfer happens (libretro_vulkan.h spec: if
     * src_queue_family == queue_index, no transfer
     * occurs).  Zero semaphores -- the QueueWaitIdle above
     * already ensures the image is fully written. */
    vk_iface->set_image(vk_iface->handle,
                        &vk_retro_image,
                        0, NULL,
                        vk_queue_family);

    /* Tell the frontend a HW frame is ready.  The data
     * pointer is the magic value RETRO_HW_FRAME_BUFFER_VALID;
     * pitch is 0 (not meaningful for HW frames).  did_flip
     * suppresses retro_run's dupe-NULL-frame path. */
    if (video_cb)
        video_cb(RETRO_HW_FRAME_BUFFER_VALID, width, height, 0);
    did_flip = true;
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
