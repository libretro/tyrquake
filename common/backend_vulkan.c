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
 * Phase 4f (this file's current state): GPU palette LUT.
 * The fragment shader (textured_palette.frag) does the index-
 * into-palette lookup itself, instead of the host doing the
 * palette convert and uploading 32bpp pixels.
 *
 * On the upload side, vid.buffer goes straight to a R8_UNORM
 * texture (1 byte per pixel, not 4), d_8to24table goes to a
 * 256x1 R8G8B8A8_UNORM palette texture, and the staging
 * buffer holds both: the first width*height bytes are the
 * index data, the next 1024 bytes are the palette.  Two
 * CmdCopyBufferToImage commands per frame carry both into
 * their respective textures.  Staging memory drops from
 * width*height*4 to width*height + 1024 (a 4x reduction at
 * 1280x960, ~5 MiB -> ~1.25 MiB) and the CPU palette-convert
 * loop in backend_vk_upload_vid_buffer becomes two memcpy
 * calls -- no per-pixel work on the host at all.
 *
 * Visual output is unchanged from Phase 4d / 4e: same Quake
 * content delivered through the same render pass, just with
 * the palette lookup moved off the CPU.
 *
 * Compute-readiness (per Lib's direction)
 * --------------------------------------
 * Several design choices keep the door open for an eventual
 * compute-based path -- whether that's a compute palette
 * LUT (read index, write RGBA via storage image), compute
 * post-process (HDR tonemap, sharpening), or eventually a
 * compute-based rasterizer that replaces R_RenderView:
 *
 *   - vk_image (the render target) gains VK_IMAGE_USAGE_
 *     STORAGE_BIT.  R8G8B8A8_UNORM's STORAGE_IMAGE_BIT is
 *     mandatory in Vulkan 1.0, so the flag is free now and
 *     lets a future compute pipeline write to the render
 *     target directly without recreating the image.
 *
 *   - The descriptor set layout declares its bindings with
 *     stageFlags = FRAGMENT | COMPUTE so a future compute
 *     pipeline can be created against the same DSL without
 *     having to redeclare a parallel layout for the same
 *     resources.
 *
 *   - Per-frame command recording (Phase 4e) is already
 *     compute-friendly: switching from CmdBeginRenderPass
 *     to CmdDispatch is a record-time change with no
 *     restructuring of the pool / buffer / submit path.
 *
 *   - The libretro Vulkan interface gives us a graphics-
 *     capable queue, which on every Vulkan-conformant
 *     implementation can also issue compute work -- no
 *     queue-family migration needed.
 *
 * The index and palette textures stay SAMPLED-only.
 * R8_UNORM's STORAGE_IMAGE_BIT is not in the Vulkan 1.0
 * mandatory format support table and there's no plausible
 * compute use case where these are write targets anyway
 * (the natural compute pattern reads them and writes
 * elsewhere).
 *
 *   Phase 4g..N -- real geometry: world surfaces, alias
 *     models, sprites, sky, liquids, shadows; palette +
 *     colormap LUT path; dynamic lightmap update; etc.
 *     R_RenderView's dispatch from backend_vk_draw_view
 *     gradually gets replaced by Vulkan-native geometry
 *     recording in record_frame.  vid.buffer becomes
 *     unused for the Vulkan path at the end of the
 *     migration.  Each stage lands as a separate commit
 *     against this same file.
 *
 * Convention: every internal symbol is static.  Function-pointer
 * tables (vk_fn) are file-static structs populated by
 * context_reset and torn down by context_destroy.  No libvulkan
 * link -- every Vulkan symbol is resolved through the
 * frontend-supplied loader at run time.
 */

#include "rhi.h"
#include "vid.h"          /* viddef_t vid, d_8to24table -- Phase 4d
                           * needs vid.buffer and the palette LUT
                           * for the per-frame upload. */

extern void R_RenderView(void);   /* r_main.c -- backend_vk_draw_view
                                   * dispatches into the SW rasterizer
                                   * for Phase 4d.  The real geometry
                                   * pipeline lands in Phase 4e+. */

#ifdef RHI_HAVE_VULKAN

#include "vulkan/vulkan_core.h"
#include "libretro_vulkan.h"
#include "shaders/generated/spv/fullscreen_quad_vs.h"
#include "shaders/generated/spv/textured_palette_fs.h"
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

/* Phase 4b: shader modules + render-pass/pipeline objects.
 * Same lifetime as the image -- created in context_reset
 * after the function-pointer table is populated, destroyed
 * in context_destroy.  vk_render_pass + vk_framebuffer wrap
 * vk_image_view; vk_pipeline binds vk_render_pass at
 * creation time, so the pipeline can only be used with
 * this render pass + framebuffer. */
static VkShaderModule   vk_vs_module;
static VkShaderModule   vk_fs_module;
static VkRenderPass     vk_render_pass;
static VkFramebuffer    vk_framebuffer;
static VkPipelineLayout vk_pipeline_layout;
static VkPipeline       vk_pipeline;

/* Phase 4c: sampled-texture objects.  vk_texture is the
 * source image that the fragment shader reads through u_tex;
 * separate from vk_image (the render target).  Sized at
 * width x height: the texture is a 1:1 image of the SW
 * renderer's vid.buffer (Phase 4d).
 *
 * Phase 4f: vk_texture is now R8_UNORM (single channel
 * holding the palette index) instead of R8G8B8A8_UNORM.
 * vk_palette_texture is the 256x1 RGBA8 lookup the FS
 * indexes into using vk_texture's value as the U coordinate.
 *
 * vk_staging is a HOST_VISIBLE buffer.  Layout:
 *   bytes [0 .. width*height-1]   = index data (R8 per pixel)
 *   bytes [width*height .. +1023] = palette (256 RGBA8 entries)
 * Mapped persistently at create_resources time;
 * vk_staging_ptr stays valid until destroy_resources frees
 * the memory (vkFreeMemory implicitly unmaps).  Per-frame
 * upload writes through vk_staging_ptr; the next QueueSubmit's
 * two CmdCopyBufferToImage commands carry the two regions
 * into their respective textures.  HOST_COHERENT means no
 * Flush call needed.
 *
 * vk_descriptor_set is allocated from vk_descriptor_pool and
 * is freed implicitly when the pool is destroyed; no separate
 * FreeDescriptorSets call. */
static VkSampler              vk_sampler;
static VkImage                vk_texture;
static VkDeviceMemory         vk_texture_memory;
static VkImageView            vk_texture_view;
static VkImage                vk_palette_texture;
static VkDeviceMemory         vk_palette_texture_memory;
static VkImageView            vk_palette_texture_view;
static VkBuffer               vk_staging_buffer;
static VkDeviceMemory         vk_staging_memory;
static VkDeviceSize           vk_staging_size;
static void                  *vk_staging_ptr;
static VkDescriptorSetLayout  vk_descriptor_set_layout;
static VkDescriptorPool       vk_descriptor_pool;
static VkDescriptorSet        vk_descriptor_set;

/* Phase 4f staging-buffer layout constants.  The palette is
 * 256 RGBA8 entries = 1 KiB, placed immediately after the
 * width*height bytes of index data.  Compile-time -- staging
 * is allocated with this exact total at create_resources. */
#define VK_PALETTE_TEXELS  256u
#define VK_PALETTE_BYTES   (VK_PALETTE_TEXELS * 4u)

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

    /* Shader / render-pass / pipeline (Phase 4b) */
    PFN_vkCreateShaderModule              CreateShaderModule;
    PFN_vkDestroyShaderModule             DestroyShaderModule;
    PFN_vkCreateRenderPass                CreateRenderPass;
    PFN_vkDestroyRenderPass               DestroyRenderPass;
    PFN_vkCreateFramebuffer               CreateFramebuffer;
    PFN_vkDestroyFramebuffer              DestroyFramebuffer;
    PFN_vkCreatePipelineLayout            CreatePipelineLayout;
    PFN_vkDestroyPipelineLayout           DestroyPipelineLayout;
    PFN_vkCreateGraphicsPipelines         CreateGraphicsPipelines;
    PFN_vkDestroyPipeline                 DestroyPipeline;
    PFN_vkCmdBeginRenderPass              CmdBeginRenderPass;
    PFN_vkCmdEndRenderPass                CmdEndRenderPass;
    PFN_vkCmdBindPipeline                 CmdBindPipeline;
    PFN_vkCmdDraw                         CmdDraw;

    /* Sampler / descriptor / staging upload (Phase 4c) */
    PFN_vkCreateSampler                   CreateSampler;
    PFN_vkDestroySampler                  DestroySampler;
    PFN_vkCreateBuffer                    CreateBuffer;
    PFN_vkDestroyBuffer                   DestroyBuffer;
    PFN_vkGetBufferMemoryRequirements     GetBufferMemoryRequirements;
    PFN_vkBindBufferMemory                BindBufferMemory;
    PFN_vkMapMemory                       MapMemory;
    PFN_vkUnmapMemory                     UnmapMemory;
    PFN_vkCreateDescriptorSetLayout       CreateDescriptorSetLayout;
    PFN_vkDestroyDescriptorSetLayout      DestroyDescriptorSetLayout;
    PFN_vkCreateDescriptorPool            CreateDescriptorPool;
    PFN_vkDestroyDescriptorPool           DestroyDescriptorPool;
    PFN_vkAllocateDescriptorSets          AllocateDescriptorSets;
    PFN_vkUpdateDescriptorSets            UpdateDescriptorSets;
    PFN_vkResetCommandBuffer              ResetCommandBuffer;
    PFN_vkCmdCopyBufferToImage            CmdCopyBufferToImage;
    PFN_vkCmdBindDescriptorSets           CmdBindDescriptorSets;
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
 * Create the per-context render target (image + memory + view),
 * the graphics-pipeline objects (shader modules + render pass +
 * framebuffer + pipeline layout + graphics pipeline), and the
 * command-buffer infrastructure (pool + one primary buffer);
 * then pre-record a render-pass-based draw that runs every
 * frame.  On any failure rolls back and leaves
 * vk_resources_ready false; end_frame then no-ops and the
 * frontend falls back to dupe.
 */
static qboolean
backend_vk_create_resources(void)
{
    VkImageCreateInfo                       image_ci;
    VkMemoryRequirements                    mem_req;
    VkMemoryAllocateInfo                    alloc_info;
    VkImageViewCreateInfo                   view_ci;
    VkShaderModuleCreateInfo                sm_ci;
    VkAttachmentDescription                 attachment;
    VkAttachmentReference                   color_ref;
    VkSubpassDescription                    subpass;
    VkSubpassDependency                     dep;
    VkRenderPassCreateInfo                  rp_ci;
    VkFramebufferCreateInfo                 fb_ci;
    VkPipelineLayoutCreateInfo              pl_ci;
    VkPipelineShaderStageCreateInfo         stages[2];
    VkPipelineVertexInputStateCreateInfo    vi;
    VkPipelineInputAssemblyStateCreateInfo  ia;
    VkViewport                              viewport;
    VkRect2D                                scissor;
    VkPipelineViewportStateCreateInfo       vp;
    VkPipelineRasterizationStateCreateInfo  rs;
    VkPipelineMultisampleStateCreateInfo    ms;
    VkPipelineColorBlendAttachmentState     cba;
    VkPipelineColorBlendStateCreateInfo     cb;
    VkGraphicsPipelineCreateInfo            gp_ci;
    VkCommandPoolCreateInfo                 pool_ci;
    VkCommandBufferAllocateInfo             cmd_alloc;
    VkImageView                             fb_attachments[1];
    VkSamplerCreateInfo                     sampler_ci;
    VkBufferCreateInfo                      buf_ci;
    VkDescriptorSetLayoutBinding            dsl_bindings[2];
    VkDescriptorSetLayoutCreateInfo         dsl_ci;
    VkDescriptorPoolSize                    dp_size;
    VkDescriptorPoolCreateInfo              dp_ci;
    VkDescriptorSetAllocateInfo             ds_alloc;
    VkDescriptorImageInfo                   ds_image_infos[2];
    VkWriteDescriptorSet                    ds_writes[2];
    uint32_t                                mem_type;
    VkResult                                r;

    /* ---------------------------------------------------------
     * Render-target image: R8G8B8A8_UNORM with MUTABLE_FORMAT
     * (libretro spec recommends mutable for 8-bit formats so
     * the frontend can reinterpret as sRGB if it wants).
     * Usage covers our needs (COLOR_ATTACHMENT for the render
     * pass output, SAMPLED for the frontend's read) plus
     * TRANSFER_SRC which the libretro Vulkan interface docs
     * call out as required for set_image.  We retain
     * TRANSFER_DST as well so a future phase can do
     * vkCmdCopyBufferToImage uploads (e.g. lifting the SW
     * framebuffer for a Phase 4c texture sample) without
     * needing to recreate the image.  STORAGE_BIT is added
     * in Phase 4f to keep the door open for compute-based
     * post-process or a compute-rasterizer end state:
     * R8G8B8A8_UNORM's STORAGE_IMAGE_BIT is mandatory in
     * Vulkan 1.0 so the flag is free now. */
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
                           | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                           | VK_IMAGE_USAGE_SAMPLED_BIT
                           | VK_IMAGE_USAGE_STORAGE_BIT;
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

    /* ---------------------------------------------------------
     * Shader modules.  Each module is just the SPIR-V bytecode
     * wrapped -- modules can be destroyed immediately after the
     * pipeline that uses them is created, but we keep them
     * around for the lifetime of the context to keep teardown
     * symmetric.  No per-frame cost; the driver compiled the
     * bytecode into its internal ISA at pipeline creation. */
    memset(&sm_ci, 0, sizeof(sm_ci));
    sm_ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    sm_ci.codeSize = spv_fullscreen_quad_vs_size;
    sm_ci.pCode    = spv_fullscreen_quad_vs;
    r = vk_fn.CreateShaderModule(vk_device, &sm_ci, NULL, &vk_vs_module);
    if (r != VK_SUCCESS) {
        if (log_cb)
            log_cb(RETRO_LOG_ERROR,
                   "rhi-vk: CreateShaderModule (vs) failed (%d)\n", (int)r);
        return false;
    }

    memset(&sm_ci, 0, sizeof(sm_ci));
    sm_ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    sm_ci.codeSize = spv_textured_palette_fs_size;
    sm_ci.pCode    = spv_textured_palette_fs;
    r = vk_fn.CreateShaderModule(vk_device, &sm_ci, NULL, &vk_fs_module);
    if (r != VK_SUCCESS) {
        if (log_cb)
            log_cb(RETRO_LOG_ERROR,
                   "rhi-vk: CreateShaderModule (fs) failed (%d)\n", (int)r);
        return false;
    }

    /* ---------------------------------------------------------
     * Render pass.  One color attachment, one subpass.  The
     * attachment's load_op = CLEAR handles the per-frame
     * background fill (no separate vkCmdClearColorImage
     * needed); the final_layout = SHADER_READ_ONLY_OPTIMAL
     * triggers the layout transition we previously did
     * manually via a second pipeline barrier.  Both happen as
     * part of the render-pass instance with no explicit
     * barrier commands in the command buffer.
     *
     * The external -> 0 subpass dependency covers the case
     * where the frontend is still reading from a previous
     * frame's image when we start writing the next: it waits
     * for SHADER_READ to finish before COLOR_ATTACHMENT_WRITE
     * begins.  Today end_frame's QueueWaitIdle already drains
     * the queue between submits so the dependency is
     * redundant, but it's correct hygiene for a future phase
     * that removes the wait. */
    memset(&attachment, 0, sizeof(attachment));
    attachment.format         = VK_FORMAT_R8G8B8A8_UNORM;
    attachment.samples        = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    color_ref.attachment = 0;
    color_ref.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    memset(&subpass, 0, sizeof(subpass));
    subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments    = &color_ref;

    memset(&dep, 0, sizeof(dep));
    dep.srcSubpass    = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass    = 0;
    dep.srcStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dep.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    memset(&rp_ci, 0, sizeof(rp_ci));
    rp_ci.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp_ci.attachmentCount = 1;
    rp_ci.pAttachments    = &attachment;
    rp_ci.subpassCount    = 1;
    rp_ci.pSubpasses      = &subpass;
    rp_ci.dependencyCount = 1;
    rp_ci.pDependencies   = &dep;

    r = vk_fn.CreateRenderPass(vk_device, &rp_ci, NULL, &vk_render_pass);
    if (r != VK_SUCCESS) {
        if (log_cb)
            log_cb(RETRO_LOG_ERROR, "rhi-vk: CreateRenderPass failed (%d)\n", (int)r);
        return false;
    }

    /* Framebuffer wraps the image view + render pass. */
    fb_attachments[0] = vk_image_view;
    memset(&fb_ci, 0, sizeof(fb_ci));
    fb_ci.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fb_ci.renderPass      = vk_render_pass;
    fb_ci.attachmentCount = 1;
    fb_ci.pAttachments    = fb_attachments;
    fb_ci.width           = width;
    fb_ci.height          = height;
    fb_ci.layers          = 1;

    r = vk_fn.CreateFramebuffer(vk_device, &fb_ci, NULL, &vk_framebuffer);
    if (r != VK_SUCCESS) {
        if (log_cb)
            log_cb(RETRO_LOG_ERROR, "rhi-vk: CreateFramebuffer failed (%d)\n", (int)r);
        return false;
    }

    /* ---------------------------------------------------------
     * Sampler.  NEAREST filtering on a CLAMP_TO_EDGE address
     * mode.  NEAREST keeps the Phase 4c checkerboard crisp at
     * any scale and matches Quake's "no texture filtering"
     * aesthetic; LINEAR would be appropriate for a Phase 4e
     * "filtered look" option.  CLAMP_TO_EDGE keeps the
     * fragments sampled by the oversize triangle's out-of-
     * viewport portion (uv.x or uv.y in (1, 2]) from wrapping
     * back into the texture -- they'd be clipped anyway but
     * the safe address mode costs nothing. */
    memset(&sampler_ci, 0, sizeof(sampler_ci));
    sampler_ci.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_ci.magFilter    = VK_FILTER_NEAREST;
    sampler_ci.minFilter    = VK_FILTER_NEAREST;
    sampler_ci.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler_ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_ci.minLod       = 0.0f;
    sampler_ci.maxLod       = 0.0f;

    r = vk_fn.CreateSampler(vk_device, &sampler_ci, NULL, &vk_sampler);
    if (r != VK_SUCCESS) {
        if (log_cb)
            log_cb(RETRO_LOG_ERROR, "rhi-vk: CreateSampler failed (%d)\n", (int)r);
        return false;
    }

    /* ---------------------------------------------------------
     * Source index texture for the fragment shader to sample
     * from.  Sized 1:1 with the SW framebuffer (width x
     * height), R8_UNORM (one byte per pixel, raw palette
     * index from vid.buffer).  OPTIMAL tiling, DEVICE_LOCAL
     * memory.  Usage covers the data path we need:
     * TRANSFER_DST for the upload, SAMPLED for the fragment-
     * shader read.  No MUTABLE_FORMAT flag; no STORAGE_BIT
     * either -- R8_UNORM's storage support is not in the
     * Vulkan 1.0 mandatory format table and the natural
     * compute use case for this texture is a read, not a
     * write. */
    memset(&image_ci, 0, sizeof(image_ci));
    image_ci.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_ci.imageType     = VK_IMAGE_TYPE_2D;
    image_ci.format        = VK_FORMAT_R8_UNORM;
    image_ci.extent.width  = width;
    image_ci.extent.height = height;
    image_ci.extent.depth  = 1;
    image_ci.mipLevels     = 1;
    image_ci.arrayLayers   = 1;
    image_ci.samples       = VK_SAMPLE_COUNT_1_BIT;
    image_ci.tiling        = VK_IMAGE_TILING_OPTIMAL;
    image_ci.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT
                           | VK_IMAGE_USAGE_SAMPLED_BIT;
    image_ci.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    image_ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    r = vk_fn.CreateImage(vk_device, &image_ci, NULL, &vk_texture);
    if (r != VK_SUCCESS) {
        if (log_cb)
            log_cb(RETRO_LOG_ERROR,
                   "rhi-vk: CreateImage (index texture) failed (%d)\n", (int)r);
        return false;
    }

    memset(&mem_req, 0, sizeof(mem_req));
    vk_fn.GetImageMemoryRequirements(vk_device, vk_texture, &mem_req);

    mem_type = backend_vk_find_memory_type(mem_req.memoryTypeBits,
                                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mem_type == 0xFFFFFFFFu) {
        if (log_cb)
            log_cb(RETRO_LOG_ERROR,
                   "rhi-vk: no device-local memory type for index texture\n");
        return false;
    }

    memset(&alloc_info, 0, sizeof(alloc_info));
    alloc_info.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize  = mem_req.size;
    alloc_info.memoryTypeIndex = mem_type;

    r = vk_fn.AllocateMemory(vk_device, &alloc_info, NULL, &vk_texture_memory);
    if (r != VK_SUCCESS) {
        if (log_cb)
            log_cb(RETRO_LOG_ERROR,
                   "rhi-vk: AllocateMemory (index texture) failed (%d)\n", (int)r);
        return false;
    }

    r = vk_fn.BindImageMemory(vk_device, vk_texture, vk_texture_memory, 0);
    if (r != VK_SUCCESS) {
        if (log_cb)
            log_cb(RETRO_LOG_ERROR,
                   "rhi-vk: BindImageMemory (index texture) failed (%d)\n", (int)r);
        return false;
    }

    memset(&view_ci, 0, sizeof(view_ci));
    view_ci.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_ci.image            = vk_texture;
    view_ci.viewType         = VK_IMAGE_VIEW_TYPE_2D;
    view_ci.format           = VK_FORMAT_R8_UNORM;
    view_ci.components.r     = VK_COMPONENT_SWIZZLE_IDENTITY;
    view_ci.components.g     = VK_COMPONENT_SWIZZLE_IDENTITY;
    view_ci.components.b     = VK_COMPONENT_SWIZZLE_IDENTITY;
    view_ci.components.a     = VK_COMPONENT_SWIZZLE_IDENTITY;
    view_ci.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    view_ci.subresourceRange.baseMipLevel   = 0;
    view_ci.subresourceRange.levelCount     = 1;
    view_ci.subresourceRange.baseArrayLayer = 0;
    view_ci.subresourceRange.layerCount     = 1;

    r = vk_fn.CreateImageView(vk_device, &view_ci, NULL, &vk_texture_view);
    if (r != VK_SUCCESS) {
        if (log_cb)
            log_cb(RETRO_LOG_ERROR,
                   "rhi-vk: CreateImageView (index texture) failed (%d)\n", (int)r);
        return false;
    }

    /* ---------------------------------------------------------
     * Palette texture.  256x1 R8G8B8A8_UNORM, one texel per
     * palette entry.  The FS samples this with NEAREST
     * filtering at u = idx (the R8_UNORM index value, which
     * arrives as the float idx/255).  Verified mapping:
     * floor((idx/255) * 256) == idx for every idx in
     * [0, 255], so the texel boundary maps exactly to the
     * palette entry without off-by-one. */
    memset(&image_ci, 0, sizeof(image_ci));
    image_ci.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_ci.imageType     = VK_IMAGE_TYPE_2D;
    image_ci.format        = VK_FORMAT_R8G8B8A8_UNORM;
    image_ci.extent.width  = VK_PALETTE_TEXELS;
    image_ci.extent.height = 1;
    image_ci.extent.depth  = 1;
    image_ci.mipLevels     = 1;
    image_ci.arrayLayers   = 1;
    image_ci.samples       = VK_SAMPLE_COUNT_1_BIT;
    image_ci.tiling        = VK_IMAGE_TILING_OPTIMAL;
    image_ci.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT
                           | VK_IMAGE_USAGE_SAMPLED_BIT;
    image_ci.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    image_ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    r = vk_fn.CreateImage(vk_device, &image_ci, NULL, &vk_palette_texture);
    if (r != VK_SUCCESS) {
        if (log_cb)
            log_cb(RETRO_LOG_ERROR,
                   "rhi-vk: CreateImage (palette) failed (%d)\n", (int)r);
        return false;
    }

    memset(&mem_req, 0, sizeof(mem_req));
    vk_fn.GetImageMemoryRequirements(vk_device, vk_palette_texture, &mem_req);

    mem_type = backend_vk_find_memory_type(mem_req.memoryTypeBits,
                                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mem_type == 0xFFFFFFFFu) {
        if (log_cb)
            log_cb(RETRO_LOG_ERROR,
                   "rhi-vk: no device-local memory type for palette\n");
        return false;
    }

    memset(&alloc_info, 0, sizeof(alloc_info));
    alloc_info.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize  = mem_req.size;
    alloc_info.memoryTypeIndex = mem_type;

    r = vk_fn.AllocateMemory(vk_device, &alloc_info, NULL, &vk_palette_texture_memory);
    if (r != VK_SUCCESS) {
        if (log_cb)
            log_cb(RETRO_LOG_ERROR,
                   "rhi-vk: AllocateMemory (palette) failed (%d)\n", (int)r);
        return false;
    }

    r = vk_fn.BindImageMemory(vk_device, vk_palette_texture,
                              vk_palette_texture_memory, 0);
    if (r != VK_SUCCESS) {
        if (log_cb)
            log_cb(RETRO_LOG_ERROR,
                   "rhi-vk: BindImageMemory (palette) failed (%d)\n", (int)r);
        return false;
    }

    memset(&view_ci, 0, sizeof(view_ci));
    view_ci.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_ci.image            = vk_palette_texture;
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

    r = vk_fn.CreateImageView(vk_device, &view_ci, NULL, &vk_palette_texture_view);
    if (r != VK_SUCCESS) {
        if (log_cb)
            log_cb(RETRO_LOG_ERROR,
                   "rhi-vk: CreateImageView (palette) failed (%d)\n", (int)r);
        return false;
    }

    /* ---------------------------------------------------------
     * Staging buffer.  HOST_VISIBLE | HOST_COHERENT, TRANSFER_
     * SRC.  Layout (Phase 4f):
     *   bytes [0 .. width*height-1]               = index data
     *   bytes [width*height .. +VK_PALETTE_BYTES] = palette
     * Mapped once here and kept mapped through context_destroy
     * -- per-frame uploads memcpy through vk_staging_ptr with
     * no map/unmap cost.  HOST_COHERENT means the GPU sees
     * host writes as soon as the next QueueSubmit happens; no
     * Flush calls.  At 1280x960 the buffer is ~1.25 MiB
     * (vs. ~5 MiB in Phase 4d's RGBA-per-pixel layout). */
    vk_staging_size = (VkDeviceSize)width * (VkDeviceSize)height
                    + (VkDeviceSize)VK_PALETTE_BYTES;

    memset(&buf_ci, 0, sizeof(buf_ci));
    buf_ci.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buf_ci.size        = vk_staging_size;
    buf_ci.usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    buf_ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    r = vk_fn.CreateBuffer(vk_device, &buf_ci, NULL, &vk_staging_buffer);
    if (r != VK_SUCCESS) {
        if (log_cb)
            log_cb(RETRO_LOG_ERROR, "rhi-vk: CreateBuffer (staging) failed (%d)\n", (int)r);
        return false;
    }

    memset(&mem_req, 0, sizeof(mem_req));
    vk_fn.GetBufferMemoryRequirements(vk_device, vk_staging_buffer, &mem_req);

    mem_type = backend_vk_find_memory_type(mem_req.memoryTypeBits,
                                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                                         | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mem_type == 0xFFFFFFFFu) {
        if (log_cb)
            log_cb(RETRO_LOG_ERROR,
                   "rhi-vk: no HOST_VISIBLE|HOST_COHERENT memory for staging\n");
        return false;
    }

    memset(&alloc_info, 0, sizeof(alloc_info));
    alloc_info.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize  = mem_req.size;
    alloc_info.memoryTypeIndex = mem_type;

    r = vk_fn.AllocateMemory(vk_device, &alloc_info, NULL, &vk_staging_memory);
    if (r != VK_SUCCESS) {
        if (log_cb)
            log_cb(RETRO_LOG_ERROR,
                   "rhi-vk: AllocateMemory (staging) failed (%d)\n", (int)r);
        return false;
    }

    r = vk_fn.BindBufferMemory(vk_device, vk_staging_buffer,
                               vk_staging_memory, 0);
    if (r != VK_SUCCESS) {
        if (log_cb)
            log_cb(RETRO_LOG_ERROR,
                   "rhi-vk: BindBufferMemory (staging) failed (%d)\n", (int)r);
        return false;
    }

    vk_staging_ptr = NULL;
    r = vk_fn.MapMemory(vk_device, vk_staging_memory, 0,
                        VK_WHOLE_SIZE, 0, &vk_staging_ptr);
    if (r != VK_SUCCESS || !vk_staging_ptr) {
        if (log_cb)
            log_cb(RETRO_LOG_ERROR,
                   "rhi-vk: MapMemory (staging) failed (%d)\n", (int)r);
        return false;
    }

    /* Zero the staging buffer so the very first frame's
     * CmdCopyBufferToImage doesn't sample uninitialised host
     * memory before backend_vk_end_frame's first per-frame
     * upload runs.  Cheap one-time cost. */
    memset(vk_staging_ptr, 0, (size_t)vk_staging_size);

    /* ---------------------------------------------------------
     * Descriptor set layout.  Two combined-image-samplers
     * matching textured_palette.frag's
     *   layout(set = 0, binding = 0) uniform sampler2D u_index;
     *   layout(set = 0, binding = 1) uniform sampler2D u_palette;
     * stageFlags = FRAGMENT | COMPUTE so the same DSL can
     * be referenced by a future compute pipeline without
     * having to declare a parallel layout for the same
     * resources -- "compute door" architecture per Lib's
     * direction. */
    memset(&dsl_bindings, 0, sizeof(dsl_bindings));
    dsl_bindings[0].binding         = 0;
    dsl_bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    dsl_bindings[0].descriptorCount = 1;
    dsl_bindings[0].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT
                                    | VK_SHADER_STAGE_COMPUTE_BIT;
    dsl_bindings[1].binding         = 1;
    dsl_bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    dsl_bindings[1].descriptorCount = 1;
    dsl_bindings[1].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT
                                    | VK_SHADER_STAGE_COMPUTE_BIT;

    memset(&dsl_ci, 0, sizeof(dsl_ci));
    dsl_ci.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dsl_ci.bindingCount = 2;
    dsl_ci.pBindings    = dsl_bindings;

    r = vk_fn.CreateDescriptorSetLayout(vk_device, &dsl_ci, NULL,
                                        &vk_descriptor_set_layout);
    if (r != VK_SUCCESS) {
        if (log_cb)
            log_cb(RETRO_LOG_ERROR,
                   "rhi-vk: CreateDescriptorSetLayout failed (%d)\n", (int)r);
        return false;
    }

    /* Descriptor pool sized for exactly one set holding two
     * combined-image-samplers (index + palette). */
    memset(&dp_size, 0, sizeof(dp_size));
    dp_size.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    dp_size.descriptorCount = 2;

    memset(&dp_ci, 0, sizeof(dp_ci));
    dp_ci.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dp_ci.maxSets       = 1;
    dp_ci.poolSizeCount = 1;
    dp_ci.pPoolSizes    = &dp_size;

    r = vk_fn.CreateDescriptorPool(vk_device, &dp_ci, NULL, &vk_descriptor_pool);
    if (r != VK_SUCCESS) {
        if (log_cb)
            log_cb(RETRO_LOG_ERROR,
                   "rhi-vk: CreateDescriptorPool failed (%d)\n", (int)r);
        return false;
    }

    memset(&ds_alloc, 0, sizeof(ds_alloc));
    ds_alloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ds_alloc.descriptorPool     = vk_descriptor_pool;
    ds_alloc.descriptorSetCount = 1;
    ds_alloc.pSetLayouts        = &vk_descriptor_set_layout;

    r = vk_fn.AllocateDescriptorSets(vk_device, &ds_alloc, &vk_descriptor_set);
    if (r != VK_SUCCESS) {
        if (log_cb)
            log_cb(RETRO_LOG_ERROR,
                   "rhi-vk: AllocateDescriptorSets failed (%d)\n", (int)r);
        return false;
    }

    /* Point each binding at its texture + the shared sampler.
     * The imageLayout we declare here is what the shader
     * assumes at sample time; record_frame's per-frame layout
     * transitions land both textures in
     * SHADER_READ_ONLY_OPTIMAL before the draw runs. */
    memset(&ds_image_infos, 0, sizeof(ds_image_infos));
    ds_image_infos[0].sampler     = vk_sampler;
    ds_image_infos[0].imageView   = vk_texture_view;
    ds_image_infos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    ds_image_infos[1].sampler     = vk_sampler;
    ds_image_infos[1].imageView   = vk_palette_texture_view;
    ds_image_infos[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    memset(&ds_writes, 0, sizeof(ds_writes));
    ds_writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    ds_writes[0].dstSet          = vk_descriptor_set;
    ds_writes[0].dstBinding      = 0;
    ds_writes[0].dstArrayElement = 0;
    ds_writes[0].descriptorCount = 1;
    ds_writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    ds_writes[0].pImageInfo      = &ds_image_infos[0];
    ds_writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    ds_writes[1].dstSet          = vk_descriptor_set;
    ds_writes[1].dstBinding      = 1;
    ds_writes[1].dstArrayElement = 0;
    ds_writes[1].descriptorCount = 1;
    ds_writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    ds_writes[1].pImageInfo      = &ds_image_infos[1];

    vk_fn.UpdateDescriptorSets(vk_device, 2, ds_writes, 0, NULL);

    /* ---------------------------------------------------------
     * Pipeline layout.  Includes the Phase 4c descriptor set
     * layout at set 0.  No push constants. */
    memset(&pl_ci, 0, sizeof(pl_ci));
    pl_ci.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl_ci.setLayoutCount = 1;
    pl_ci.pSetLayouts    = &vk_descriptor_set_layout;

    r = vk_fn.CreatePipelineLayout(vk_device, &pl_ci, NULL, &vk_pipeline_layout);
    if (r != VK_SUCCESS) {
        if (log_cb)
            log_cb(RETRO_LOG_ERROR,
                   "rhi-vk: CreatePipelineLayout failed (%d)\n", (int)r);
        return false;
    }

    /* Graphics pipeline.  Static viewport / scissor matched to
     * our render-target dimensions (the resolution option is
     * requires-restart, so width / height don't change for the
     * lifetime of the context -- no need for VK_DYNAMIC_STATE_
     * VIEWPORT).  No vertex input (the VS generates positions
     * from gl_VertexIndex).  No depth / stencil.  No blending.
     * Triangle list with three vertices = the fullscreen
     * triangle. */
    memset(&stages[0], 0, sizeof(stages[0]));
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vk_vs_module;
    stages[0].pName  = "main";
    memset(&stages[1], 0, sizeof(stages[1]));
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = vk_fs_module;
    stages[1].pName  = "main";

    memset(&vi, 0, sizeof(vi));
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    memset(&ia, 0, sizeof(ia));
    ia.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    memset(&viewport, 0, sizeof(viewport));
    viewport.x        = 0.0f;
    viewport.y        = 0.0f;
    viewport.width    = (float)width;
    viewport.height   = (float)height;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;

    memset(&scissor, 0, sizeof(scissor));
    scissor.extent.width  = width;
    scissor.extent.height = height;

    memset(&vp, 0, sizeof(vp));
    vp.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.pViewports    = &viewport;
    vp.scissorCount  = 1;
    vp.pScissors     = &scissor;

    memset(&rs, 0, sizeof(rs));
    rs.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode    = VK_CULL_MODE_NONE;
    rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth   = 1.0f;

    memset(&ms, 0, sizeof(ms));
    ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    memset(&cba, 0, sizeof(cba));
    cba.blendEnable    = VK_FALSE;
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT
                       | VK_COLOR_COMPONENT_G_BIT
                       | VK_COLOR_COMPONENT_B_BIT
                       | VK_COLOR_COMPONENT_A_BIT;

    memset(&cb, 0, sizeof(cb));
    cb.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments    = &cba;

    memset(&gp_ci, 0, sizeof(gp_ci));
    gp_ci.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gp_ci.stageCount          = 2;
    gp_ci.pStages             = stages;
    gp_ci.pVertexInputState   = &vi;
    gp_ci.pInputAssemblyState = &ia;
    gp_ci.pViewportState      = &vp;
    gp_ci.pRasterizationState = &rs;
    gp_ci.pMultisampleState   = &ms;
    gp_ci.pColorBlendState    = &cb;
    gp_ci.layout              = vk_pipeline_layout;
    gp_ci.renderPass          = vk_render_pass;
    gp_ci.subpass             = 0;

    r = vk_fn.CreateGraphicsPipelines(vk_device, VK_NULL_HANDLE,
                                      1, &gp_ci, NULL, &vk_pipeline);
    if (r != VK_SUCCESS) {
        if (log_cb)
            log_cb(RETRO_LOG_ERROR,
                   "rhi-vk: CreateGraphicsPipelines failed (%d)\n", (int)r);
        return false;
    }

    /* ---------------------------------------------------------
     * Command pool + buffer.  RESET_COMMAND_BUFFER_BIT lets
     * vkBeginCommandBuffer implicitly reset the buffer at the
     * top of every per-frame recording (Phase 4e);
     * vkResetCommandBuffer is never called explicitly. */
    memset(&pool_ci, 0, sizeof(pool_ci));
    pool_ci.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_ci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_ci.queueFamilyIndex = vk_queue_family;

    r = vk_fn.CreateCommandPool(vk_device, &pool_ci, NULL, &vk_cmd_pool);
    if (r != VK_SUCCESS) {
        if (log_cb)
            log_cb(RETRO_LOG_ERROR, "rhi-vk: CreateCommandPool failed (%d)\n", (int)r);
        return false;
    }

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

    /* No per-frame recording here -- backend_vk_record_frame
     * fills the buffer fresh each time end_frame runs.  The
     * buffer is in initial state at this point; the first
     * recording will transition it to executable via the
     * implicit reset that vkBeginCommandBuffer does when the
     * pool was created with RESET_COMMAND_BUFFER_BIT. */

    vk_resources_ready = true;
    if (log_cb)
        log_cb(RETRO_LOG_INFO,
               "rhi-vk: render target %ux%u + R8 index + 256x1 palette + "
               "staging %llu bytes ready; GPU palette LUT active\n",
               width, height,
               (unsigned long long)vk_staging_size);
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
         * needed.  This also frees the one-shot upload
         * command buffer if it was still allocated. */
        if (vk_fn.DestroyCommandPool)
            vk_fn.DestroyCommandPool(vk_device, vk_cmd_pool, NULL);
        vk_cmd_pool   = VK_NULL_HANDLE;
        vk_cmd_buffer = VK_NULL_HANDLE;
    }
    /* Phase 4c descriptor / sampler / staging / texture
     * teardown.  Reverse-create order, except that descriptor
     * pool destruction implicitly frees the allocated set, so
     * vk_descriptor_set just gets zeroed without a separate
     * Free call. */
    if (vk_descriptor_pool != VK_NULL_HANDLE) {
        if (vk_fn.DestroyDescriptorPool)
            vk_fn.DestroyDescriptorPool(vk_device, vk_descriptor_pool, NULL);
        vk_descriptor_pool = VK_NULL_HANDLE;
        vk_descriptor_set  = VK_NULL_HANDLE;
    }
    if (vk_descriptor_set_layout != VK_NULL_HANDLE) {
        if (vk_fn.DestroyDescriptorSetLayout)
            vk_fn.DestroyDescriptorSetLayout(vk_device,
                                             vk_descriptor_set_layout, NULL);
        vk_descriptor_set_layout = VK_NULL_HANDLE;
    }
    if (vk_staging_buffer != VK_NULL_HANDLE) {
        if (vk_fn.DestroyBuffer)
            vk_fn.DestroyBuffer(vk_device, vk_staging_buffer, NULL);
        vk_staging_buffer = VK_NULL_HANDLE;
    }
    if (vk_staging_memory != VK_NULL_HANDLE) {
        /* vkFreeMemory implicitly unmaps any persistent
         * mapping; no UnmapMemory needed.  Zero
         * vk_staging_ptr so a stale-pointer dereference
         * after teardown would be a NULL deref rather than
         * a use-after-free on freed device memory. */
        if (vk_fn.FreeMemory)
            vk_fn.FreeMemory(vk_device, vk_staging_memory, NULL);
        vk_staging_memory = VK_NULL_HANDLE;
    }
    vk_staging_ptr  = NULL;
    vk_staging_size = 0;
    if (vk_texture_view != VK_NULL_HANDLE) {
        if (vk_fn.DestroyImageView)
            vk_fn.DestroyImageView(vk_device, vk_texture_view, NULL);
        vk_texture_view = VK_NULL_HANDLE;
    }
    if (vk_texture != VK_NULL_HANDLE) {
        if (vk_fn.DestroyImage)
            vk_fn.DestroyImage(vk_device, vk_texture, NULL);
        vk_texture = VK_NULL_HANDLE;
    }
    if (vk_texture_memory != VK_NULL_HANDLE) {
        if (vk_fn.FreeMemory)
            vk_fn.FreeMemory(vk_device, vk_texture_memory, NULL);
        vk_texture_memory = VK_NULL_HANDLE;
    }
    if (vk_palette_texture_view != VK_NULL_HANDLE) {
        if (vk_fn.DestroyImageView)
            vk_fn.DestroyImageView(vk_device, vk_palette_texture_view, NULL);
        vk_palette_texture_view = VK_NULL_HANDLE;
    }
    if (vk_palette_texture != VK_NULL_HANDLE) {
        if (vk_fn.DestroyImage)
            vk_fn.DestroyImage(vk_device, vk_palette_texture, NULL);
        vk_palette_texture = VK_NULL_HANDLE;
    }
    if (vk_palette_texture_memory != VK_NULL_HANDLE) {
        if (vk_fn.FreeMemory)
            vk_fn.FreeMemory(vk_device, vk_palette_texture_memory, NULL);
        vk_palette_texture_memory = VK_NULL_HANDLE;
    }
    if (vk_sampler != VK_NULL_HANDLE) {
        if (vk_fn.DestroySampler)
            vk_fn.DestroySampler(vk_device, vk_sampler, NULL);
        vk_sampler = VK_NULL_HANDLE;
    }
    /* Phase 4b pipeline objects: reverse-create order.  The
     * graphics pipeline references the layout and the render
     * pass; the framebuffer references the render pass + image
     * view; the shader modules can go any time after pipeline
     * destruction. */
    if (vk_pipeline != VK_NULL_HANDLE) {
        if (vk_fn.DestroyPipeline)
            vk_fn.DestroyPipeline(vk_device, vk_pipeline, NULL);
        vk_pipeline = VK_NULL_HANDLE;
    }
    if (vk_pipeline_layout != VK_NULL_HANDLE) {
        if (vk_fn.DestroyPipelineLayout)
            vk_fn.DestroyPipelineLayout(vk_device, vk_pipeline_layout, NULL);
        vk_pipeline_layout = VK_NULL_HANDLE;
    }
    if (vk_framebuffer != VK_NULL_HANDLE) {
        if (vk_fn.DestroyFramebuffer)
            vk_fn.DestroyFramebuffer(vk_device, vk_framebuffer, NULL);
        vk_framebuffer = VK_NULL_HANDLE;
    }
    if (vk_render_pass != VK_NULL_HANDLE) {
        if (vk_fn.DestroyRenderPass)
            vk_fn.DestroyRenderPass(vk_device, vk_render_pass, NULL);
        vk_render_pass = VK_NULL_HANDLE;
    }
    if (vk_fs_module != VK_NULL_HANDLE) {
        if (vk_fn.DestroyShaderModule)
            vk_fn.DestroyShaderModule(vk_device, vk_fs_module, NULL);
        vk_fs_module = VK_NULL_HANDLE;
    }
    if (vk_vs_module != VK_NULL_HANDLE) {
        if (vk_fn.DestroyShaderModule)
            vk_fn.DestroyShaderModule(vk_device, vk_vs_module, NULL);
        vk_vs_module = VK_NULL_HANDLE;
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
        /* Phase 4b additions */
        LOAD_DEV(CreateShaderModule);
        LOAD_DEV(DestroyShaderModule);
        LOAD_DEV(CreateRenderPass);
        LOAD_DEV(DestroyRenderPass);
        LOAD_DEV(CreateFramebuffer);
        LOAD_DEV(DestroyFramebuffer);
        LOAD_DEV(CreatePipelineLayout);
        LOAD_DEV(DestroyPipelineLayout);
        LOAD_DEV(CreateGraphicsPipelines);
        LOAD_DEV(DestroyPipeline);
        LOAD_DEV(CmdBeginRenderPass);
        LOAD_DEV(CmdEndRenderPass);
        LOAD_DEV(CmdBindPipeline);
        LOAD_DEV(CmdDraw);
        /* Phase 4c additions */
        LOAD_DEV(CreateSampler);
        LOAD_DEV(DestroySampler);
        LOAD_DEV(CreateBuffer);
        LOAD_DEV(DestroyBuffer);
        LOAD_DEV(GetBufferMemoryRequirements);
        LOAD_DEV(BindBufferMemory);
        LOAD_DEV(MapMemory);
        LOAD_DEV(UnmapMemory);
        LOAD_DEV(CreateDescriptorSetLayout);
        LOAD_DEV(DestroyDescriptorSetLayout);
        LOAD_DEV(CreateDescriptorPool);
        LOAD_DEV(DestroyDescriptorPool);
        LOAD_DEV(AllocateDescriptorSets);
        LOAD_DEV(UpdateDescriptorSets);
        LOAD_DEV(ResetCommandBuffer);
        LOAD_DEV(CmdCopyBufferToImage);
        LOAD_DEV(CmdBindDescriptorSets);
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
    /* Phase 4d: dispatch into the SW rasterizer.  R_RenderView
     * fills vid.buffer with 8bpp palette-indexed pixels for the
     * 3D world view.  The 2D HUD / console / status bar are
     * composited into the same buffer by the caller chain
     * (SCR_UpdateScreen -> Sbar_Draw / SCR_DrawConsole / etc.)
     * after V_RenderView returns.  By the time
     * backend_vk_end_frame runs, vid.buffer contains a fully
     * composited frame ready for upload.
     *
     * Phase 4e+ replaces R_RenderView with Vulkan-native
     * geometry recording.  The 2D path follows; when both are
     * migrated this function records geometry directly into
     * vk_cmd_buffer and vid.buffer goes unused for the Vulkan
     * path. */
    R_RenderView();
#endif
}

#ifdef RHI_HAVE_VULKAN
/*
 * Per-frame upload (Phase 4f).  Copies the raw 8bpp vid.buffer
 * into the index region of the staging buffer (bytes
 * [0, width*height)) and d_8to24table into the palette region
 * (bytes [width*height, width*height + VK_PALETTE_BYTES)).
 * No per-pixel arithmetic on the host -- the FS does the
 * index-into-palette lookup at sample time.
 *
 * Walks the index region row by row to honour vid.rowbytes
 * (currently == width, but the SW renderer is allowed to pad
 * rows; defending against a future change there costs
 * nothing).  HOST_COHERENT memory means no Flush call needed;
 * the next QueueSubmit's CmdCopyBufferToImage commands see
 * the new bytes.
 *
 * The palette is uploaded every frame even though it changes
 * only on damage flashes / underwater tint / level load: at
 * 1 KiB it's invisible against the index data and avoiding
 * change-detection logic keeps record_frame's command stream
 * uniform across frames.
 */
static void
backend_vk_upload_vid_buffer(void)
{
    uint8_t *idx_dst;
    uint8_t *pal_dst;
    uint32_t y;

    if (!vk_staging_ptr || !vid.buffer)
        return;

    idx_dst = (uint8_t *)vk_staging_ptr;
    for (y = 0; y < height; y++) {
        const uint8_t *srow = (const uint8_t *)vid.buffer
                            + (size_t)y * (size_t)vid.rowbytes;
        memcpy(idx_dst + (size_t)y * (size_t)width, srow, (size_t)width);
    }

    pal_dst = idx_dst + (size_t)width * (size_t)height;
    memcpy(pal_dst, d_8to24table, VK_PALETTE_BYTES);
}

/*
 * Record the per-frame command buffer.  Called by end_frame
 * after the host-side staging upload.  Sequence:
 *
 *   Begin (ONE_TIME_SUBMIT)
 *     Barrier UNDEFINED -> TRANSFER_DST_OPTIMAL on vk_texture.
 *       UNDEFINED-as-discard handles both the first frame
 *       (texture genuinely UNDEFINED out of create_resources)
 *       and subsequent frames (texture in
 *       SHADER_READ_ONLY_OPTIMAL from the previous frame's
 *       record).  Wrapping the previous content as garbage
 *       is correct: we overwrite the entire texture in the
 *       copy that follows.
 *     CmdCopyBufferToImage staging -> texture.
 *     Barrier TRANSFER_DST -> SHADER_READ_ONLY_OPTIMAL so
 *       the FS's sample sees the freshly copied bytes.
 *     CmdBindDescriptorSets binds the texture+sampler set.
 *     BeginRenderPass + BindPipeline + Draw(3) + EndRenderPass.
 *   End
 *
 * The ONE_TIME_SUBMIT flag tells the driver this recorded
 * stream will be submitted exactly once -- legal because
 * the buffer is reset on the next Begin, which is what
 * VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT enables.
 * Drivers can apply optimisations (eliding state caching
 * that a reusable buffer would need to preserve) under
 * this contract.
 *
 * Returns false on any record-time failure so end_frame can
 * skip the submit and let the frontend dupe-frame instead.
 */
static qboolean
backend_vk_record_frame(void)
{
    VkCommandBufferBeginInfo  begin_info;
    VkImageMemoryBarrier      barriers[2];
    VkBufferImageCopy         regions[2];
    VkRenderPassBeginInfo     rpbi;
    VkClearValue              clear_val;
    VkResult                  r;

    memset(&begin_info, 0, sizeof(begin_info));
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    r = vk_fn.BeginCommandBuffer(vk_cmd_buffer, &begin_info);
    if (r != VK_SUCCESS) {
        if (log_cb)
            log_cb(RETRO_LOG_ERROR, "rhi-vk: BeginCommandBuffer failed (%d)\n", (int)r);
        return false;
    }

    /* Both textures: UNDEFINED -> TRANSFER_DST_OPTIMAL in a
     * single CmdPipelineBarrier.  UNDEFINED-as-discard is the
     * right old layout for both the first frame and every
     * frame thereafter; we overwrite the entire image content
     * in the CmdCopyBufferToImage that follows. */
    memset(&barriers, 0, sizeof(barriers));
    barriers[0].sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barriers[0].srcAccessMask                   = 0;
    barriers[0].dstAccessMask                   = VK_ACCESS_TRANSFER_WRITE_BIT;
    barriers[0].oldLayout                       = VK_IMAGE_LAYOUT_UNDEFINED;
    barriers[0].newLayout                       = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barriers[0].srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
    barriers[0].dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
    barriers[0].image                           = vk_texture;
    barriers[0].subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    barriers[0].subresourceRange.baseMipLevel   = 0;
    barriers[0].subresourceRange.levelCount     = 1;
    barriers[0].subresourceRange.baseArrayLayer = 0;
    barriers[0].subresourceRange.layerCount     = 1;
    barriers[1]       = barriers[0];
    barriers[1].image = vk_palette_texture;

    vk_fn.CmdPipelineBarrier(vk_cmd_buffer,
                             VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0,
                             0, NULL,
                             0, NULL,
                             2, barriers);

    /* Index texture: bytes [0, width*height) of staging at
     * the start of vk_texture.  Palette texture: bytes
     * [width*height, +VK_PALETTE_BYTES) of staging at the
     * start of vk_palette_texture. */
    memset(&regions, 0, sizeof(regions));
    regions[0].bufferOffset                    = 0;
    regions[0].bufferRowLength                 = 0;  /* tightly packed */
    regions[0].bufferImageHeight               = 0;
    regions[0].imageSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    regions[0].imageSubresource.mipLevel       = 0;
    regions[0].imageSubresource.baseArrayLayer = 0;
    regions[0].imageSubresource.layerCount     = 1;
    regions[0].imageExtent.width               = width;
    regions[0].imageExtent.height              = height;
    regions[0].imageExtent.depth               = 1;

    vk_fn.CmdCopyBufferToImage(vk_cmd_buffer,
                               vk_staging_buffer,
                               vk_texture,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               1, &regions[0]);

    regions[1].bufferOffset                    = (VkDeviceSize)width
                                               * (VkDeviceSize)height;
    regions[1].bufferRowLength                 = 0;
    regions[1].bufferImageHeight               = 0;
    regions[1].imageSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    regions[1].imageSubresource.mipLevel       = 0;
    regions[1].imageSubresource.baseArrayLayer = 0;
    regions[1].imageSubresource.layerCount     = 1;
    regions[1].imageExtent.width               = VK_PALETTE_TEXELS;
    regions[1].imageExtent.height              = 1;
    regions[1].imageExtent.depth               = 1;

    vk_fn.CmdCopyBufferToImage(vk_cmd_buffer,
                               vk_staging_buffer,
                               vk_palette_texture,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               1, &regions[1]);

    /* Both textures: TRANSFER_DST -> SHADER_READ_ONLY in a
     * single CmdPipelineBarrier. */
    barriers[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barriers[0].oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barriers[0].newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barriers[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barriers[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barriers[1].oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barriers[1].newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    vk_fn.CmdPipelineBarrier(vk_cmd_buffer,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0,
                             0, NULL,
                             0, NULL,
                             2, barriers);

    clear_val.color.float32[0] = 0.39f;
    clear_val.color.float32[1] = 0.58f;
    clear_val.color.float32[2] = 0.93f;
    clear_val.color.float32[3] = 1.0f;

    memset(&rpbi, 0, sizeof(rpbi));
    rpbi.sType                    = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpbi.renderPass               = vk_render_pass;
    rpbi.framebuffer              = vk_framebuffer;
    rpbi.renderArea.extent.width  = width;
    rpbi.renderArea.extent.height = height;
    rpbi.clearValueCount          = 1;
    rpbi.pClearValues             = &clear_val;

    vk_fn.CmdBindDescriptorSets(vk_cmd_buffer,
                                VK_PIPELINE_BIND_POINT_GRAPHICS,
                                vk_pipeline_layout,
                                0,                /* firstSet */
                                1, &vk_descriptor_set,
                                0, NULL);          /* no dynamic offsets */
    vk_fn.CmdBeginRenderPass(vk_cmd_buffer, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
    vk_fn.CmdBindPipeline(vk_cmd_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vk_pipeline);
    vk_fn.CmdDraw(vk_cmd_buffer, 3, 1, 0, 0);
    vk_fn.CmdEndRenderPass(vk_cmd_buffer);

    r = vk_fn.EndCommandBuffer(vk_cmd_buffer);
    if (r != VK_SUCCESS) {
        if (log_cb)
            log_cb(RETRO_LOG_ERROR, "rhi-vk: EndCommandBuffer failed (%d)\n", (int)r);
        return false;
    }

    return true;
}
#endif

static void
backend_vk_end_frame(void)
{
#ifdef RHI_HAVE_VULKAN
    VkSubmitInfo si;
    VkResult     r;

    if (!vk_iface || !vk_resources_ready)
        return;

    /* Refresh the staging buffer with the latest vid.buffer
     * contents before recording the copy command.  The host
     * write must happen before the GPU read; both record
     * (which references the buffer handle, not its bytes)
     * and submit (which actually reads through the handle)
     * come after this. */
    backend_vk_upload_vid_buffer();

    /* Fresh per-frame command-buffer recording.  Phase 4e
     * lifted this out of create_resources so subsequent
     * phases can vary the recorded commands per frame
     * (different draw counts, conditional passes, etc.).
     * A failure here means we can't submit a coherent
     * frame -- bail and let retro_run's dupe path keep the
     * display going. */
    if (!backend_vk_record_frame())
        return;

    /* Submit the just-recorded command buffer. */
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
