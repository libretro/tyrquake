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
 * Phase 4g (this file's current state): the per-frame
 * palette LUT moved off the graphics pipeline and into a
 * compute dispatch.  Same on-screen output as Phase 4f;
 * what changes is the GPU work that produces it.
 *
 * Phase 4f reserved a "compute door" for exactly this
 * swap: VK_IMAGE_USAGE_STORAGE_BIT on the render target,
 * descriptor-set-layout binding stage flags declared as
 * FRAGMENT | COMPUTE, per-frame command recording from
 * Phase 4e ready to issue compute dispatches as well as
 * graphics draws.  This phase walks through that door.
 *
 * What's gone:
 *
 *   - vk_render_pass.  No render pass; the compute
 *     dispatch is the entire per-frame GPU workload.
 *   - vk_framebuffer.  Render passes need framebuffers;
 *     compute dispatches write directly into images via
 *     storage descriptors.
 *   - vk_pipeline (graphics) + vk_vs_module + vk_fs_module.
 *     No graphics pipeline; no vertex or fragment shader.
 *     The fullscreen_quad.vert and textured_palette.frag
 *     SPIR-V headers stay in the tree as reference
 *     material but are no longer #included from this file.
 *   - CmdBeginRenderPass / CmdEndRenderPass / CmdDraw /
 *     CreateRenderPass / CreateFramebuffer /
 *     CreateGraphicsPipelines / DestroyRenderPass /
 *     DestroyFramebuffer entries in vk_fn -- the function
 *     pointers themselves drop out because nothing calls
 *     them.
 *
 * What's new:
 *
 *   - textured_palette.comp + its generated SPIR-V
 *     header (common/shaders/generated/spv/
 *     textured_palette_cs.h).  Functional equivalent of
 *     textured_palette.frag.
 *   - vk_cs_module + vk_compute_pipeline.
 *   - CreateComputePipelines + CmdDispatch in vk_fn.
 *   - Descriptor set layout grows from 2 to 3 bindings:
 *       binding 0: sampler2D u_index   (unchanged)
 *       binding 1: sampler2D u_palette (unchanged)
 *       binding 2: writeonly image2D u_output -- the
 *         compute shader's storage-image write target,
 *         pointing at vk_image_view.  Stage flags =
 *         COMPUTE only (no fragment use).
 *   - Descriptor pool gains 1 STORAGE_IMAGE entry.
 *   - record_frame loses the BeginRenderPass / Draw /
 *     EndRenderPass triplet and gains a CmdDispatch
 *     bracketed by two extra layout transitions on
 *     vk_image: UNDEFINED -> GENERAL on entry (compute
 *     needs GENERAL to write storage images), then
 *     GENERAL -> SHADER_READ_ONLY_OPTIMAL on exit so the
 *     libretro frontend reads it as a sampled image.
 *
 * What's the same:
 *
 *   - vid.buffer upload pattern.  R_RenderView still
 *     populates the 8bpp framebuffer; backend_vk_upload_
 *     vid_buffer still memcpys the index data and
 *     d_8to24table_shifted into the staging buffer.
 *   - CmdCopyBufferToImage staging -> vk_texture and
 *     staging -> vk_palette_texture, with their pre /
 *     post layout transitions.
 *   - Per-frame command recording (Phase 4e mechanics).
 *   - QueueSubmit + QueueWaitIdle + set_image to the
 *     libretro frontend.
 *
 * Why this is worth doing:
 *
 *   - Validates the Phase 4f compute-door design end-to-
 *     end.  Without the swap, the door is just an
 *     assertion; with it, the assertion is exercised.
 *   - Removes graphics-pipeline state that wasn't going
 *     to survive into Phase 4h+.  When real geometry
 *     drawing arrives, its render pass / pipeline / blend
 *     state / vertex input will be designed for that
 *     workload (overlay quads with alpha blending,
 *     world surfaces with depth + multitexturing, etc.).
 *     The fullscreen-palette-quad pipeline this file used
 *     to carry was specific to "show the SW framebuffer
 *     with an LUT" and wouldn't have been the right
 *     ancestor for any of those.  Better to drop it now
 *     and write the right thing later than to carry it
 *     forward as legacy.
 *   - Sets up the recording pattern (compute dispatch as
 *     a first-class per-frame operation) that future
 *     compute work -- HDR tonemap, post-process,
 *     compute-based rasterizer -- will reuse.
 *
 * Phase 4k (this file's current state): the overlay's hard-
 * coded test pics give way to a real Draw_Pic intercept.
 *
 *   - struct overlay_slot grows a `const void *key`
 *     field used as the cache key; backend_vk_queue_2d_pic
 *     does a linear scan over the slot array looking for
 *     a slot whose key == pic, and on miss finds the
 *     first empty slot and uploads pic->data into it via
 *     the existing begin_uploads / upload_pic_slot /
 *     end_uploads trio.  Once the slot is populated (or
 *     was already cached) the function appends an entry
 *     to vk_overlay_draws with NDC corners converted
 *     from Quake's vid.width / vid.height screen space.
 *
 *   - The render_backend_t vtable grows a queue_2d_pic
 *     pointer (rhi.h).  Software backend leaves it NULL
 *     (falls through to vid.buffer memcpy); Vulkan
 *     backend points it at backend_vk_queue_2d_pic.
 *
 *   - draw.c's Draw_Pic gains a one-liner that fires
 *     g_rhi->queue_2d_pic if the backend implements it,
 *     before the original SW memcpy runs.  Phase 4k
 *     deliberately keeps the SW path -- Vulkan overlay
 *     and SW HUD render the same pic at the same screen
 *     position, the Vulkan one paints on top of the
 *     compute-uploaded SW HUD, and they overlap pixel-
 *     perfectly.  Phase 4l suppresses the SW path when
 *     the Vulkan intercept is active.
 *
 *   - vk_overlay_draws is filled per-frame instead of
 *     once at create_resources: backend_vk_record_frame
 *     calls a new backend_vk_fill_overlay_vb at the
 *     start of its overlay work (filling the vertex
 *     buffer from whatever Draw_Pic queued during
 *     retro_run), and resets vk_overlay_draw_count to
 *     zero at the end of record_frame (ready for the
 *     next frame's queue).  The three hardcoded demo
 *     quads from Phase 4j are gone; the visible
 *     overlay is now whatever Quake actually draws.
 *
 * Uploads are synchronous (begin_uploads /
 * upload_pic_slot / end_uploads inside queue_2d_pic).
 * First frame after a level load may stall briefly
 * while ~50 HUD pics get uploaded; steady state hits
 * the cache and only queues draws.  Phase 4m+ can batch
 * uploads with a fence-based ring if needed.
 *
 * Phase 4l..N -- suppress SW HUD; intercept Draw_Character,
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
#include "vid.h"          /* viddef_t vid, d_8to24table_shifted --
                           * Phase 4d needs vid.buffer for the per-
                           * frame index upload; Phase 4f reads
                           * d_8to24table_shifted (the tinted
                           * RGBA palette that tracks damage / quad /
                           * underwater shifts) for the palette
                           * texture upload. */
#include "draw.h"         /* draw_chars -- the 128x128 conchars atlas
                           * Phase 4o queue_2d_char uploads into a
                           * dedicated slot to back Draw_Character /
                           * Draw_String. */

extern void R_RenderView(void);   /* r_main.c -- backend_vk_draw_view
                                   * dispatches into the SW rasterizer
                                   * for Phase 4d.  The real geometry
                                   * pipeline lands in Phase 4e+. */

#ifdef RHI_HAVE_VULKAN

#include "vulkan/vulkan_core.h"
#include "libretro_vulkan.h"
#include "shaders/generated/spv/textured_palette_cs.h"
#include "shaders/generated/spv/overlay_quad_vs.h"
#include "shaders/generated/spv/overlay_quad_fs.h"
#include "shaders/generated/spv/particles_cs.h"
#include "shaders/generated/spv/warpscreen_cs.h"
#include "d_iface.h"      /* particle_t -- the GPU compute particle
                           * rasterizer walks the active linked list
                           * via this typedef.  Also TURB_CYCLE for
                           * the warp shader's phase modulus. */
#include "d_local.h"      /* d_pzbuffer / d_pix_* / d_vrect*_particle /
                           * d_y_aspect_shift -- the SW raster state
                           * the particle compute shader's push
                           * constants snapshot. */
#include "r_local.h"      /* xcenter, ycenter, r_origin -- the rest
                           * of the SW raster state the particle
                           * compute shader snapshots.  Plus scr_vrect
                           * (transitively via screen.h) for the warp
                           * shader's bounds. */
#include "r_shared.h"     /* intsintable[] -- the sin lookup table the
                           * warp shader uploads once at init. */
#include "screen.h"       /* scr_vrect -- the rectangle the warp
                           * dispatch's push constants snapshot each
                           * frame. */
#include "client.h"       /* cl -- cl.time drives the warp phase. */
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

/* Phase 4g: compute pipeline + its shader module.  Same
 * lifetime as vk_image -- created in context_reset after
 * the function-pointer table is populated, destroyed in
 * context_destroy.  The pipeline binds vk_pipeline_layout
 * which references the (3-binding) descriptor set layout
 * declared further down; the layout has no push constants. */
static VkShaderModule   vk_cs_module;
static VkPipelineLayout vk_pipeline_layout;
static VkPipeline       vk_compute_pipeline;

/* Phase 4h: 2D overlay graphics pipeline.  Render pass +
 * framebuffer wrap the same vk_image_view that the compute
 * pipeline writes to, with loadOp = LOAD so the compute
 * output is preserved.  The render pass's attachment
 * initialLayout = GENERAL receives vk_image straight from
 * the compute dispatch (no explicit pipeline barrier
 * between them; the subpass dependency carries the
 * memory dependency), and finalLayout =
 * SHADER_READ_ONLY_OPTIMAL hands it to the libretro
 * frontend.
 *
 * Phase 4i: the overlay pipeline reuses vk_pipeline_layout
 * (the 3-binding compute layout) instead of its own empty
 * layout.  The FS samples a palette-indexed R8_UNORM
 * texture + the shared 256x1 vk_palette_texture and
 * outputs the resulting RGB.
 *
 * Phase 4j: the single test texture grows into a slot
 * array (vk_overlay_slots), each entry carrying its own
 * image / memory / view / descriptor set + dimensions;
 * the single draw turns into a per-frame draw list
 * (vk_overlay_draws), each entry pointing at a slot and
 * supplying NDC corners for the quad.  The vertex buffer
 * is sized for OVERLAY_DRAW_MAX quads (HOST_VISIBLE
 * persistent map; populated at create_resources for
 * Phase 4j, will become per-frame in Phase 4k).  All
 * three demo slots are populated at create_resources via
 * the shared begin_uploads / upload_pic_slot /
 * end_uploads helper trio. */
#define OVERLAY_SLOT_MAX 128   /* room for HUD + menu pics; Phase 4j uses 3 */
#define OVERLAY_DRAW_MAX 4096  /* per-frame draw cap; bumped from 256 in
                                * Phase 4o because Draw_Character / Draw_String
                                * intercepts can push thousands of entries when
                                * the console is open at high resolution (every
                                * visible character is one entry).  At 4096
                                * entries the VB is 4096 * 4 * 16 = 256 KB --
                                * still trivial. */

struct overlay_slot {
    VkImage         image;
    VkDeviceMemory  memory;
    VkImageView     view;
    VkDescriptorSet descriptor_set;
    unsigned        width;
    unsigned        height;
    const void     *key;     /* cache key: qpic_t pointer for pic slots, or
                              * &draw_chars for the conchars slot, or NULL for
                              * an empty slot */
};

struct overlay_draw {
    unsigned slot_idx;
    float    x0;   /* NDC top-left x */
    float    y0;   /* NDC top-left y */
    float    x1;   /* NDC bottom-right x */
    float    y1;   /* NDC bottom-right y */
    /* Sub-UV control (Phase 4o).  pic draws set (0, 0)-(1, 1)
     * to sample the whole slot texture; character draws set
     * (col/16, row/16)-((col+1)/16, (row+1)/16) to pick one
     * 8x8 cell out of the 128x128 conchars atlas. */
    float    u0;
    float    v0;
    float    u1;
    float    v1;
};

static VkShaderModule       vk_overlay_vs_module;
static VkShaderModule       vk_overlay_fs_module;
static VkRenderPass         vk_overlay_render_pass;
static VkFramebuffer        vk_overlay_framebuffer;
static VkPipeline           vk_overlay_pipeline;
static VkBuffer             vk_overlay_vertex_buffer;
static VkDeviceMemory       vk_overlay_vertex_memory;
static void                *vk_overlay_vertex_ptr;
static struct overlay_slot  vk_overlay_slots[OVERLAY_SLOT_MAX];
static struct overlay_draw  vk_overlay_draws[OVERLAY_DRAW_MAX];
static unsigned             vk_overlay_draw_count;
static size_t               vk_overlay_upload_offset;

/* Phase 4c: sampled-texture objects.  vk_texture is the
 * source image that the compute shader reads through u_index;
 * separate from vk_image (the render target).  Sized at
 * width x height: the texture is a 1:1 image of the SW
 * renderer's vid.buffer (Phase 4d).
 *
 * Phase 4f: vk_texture is R8_UNORM (single channel holding
 * the palette index) instead of R8G8B8A8_UNORM.
 * vk_palette_texture is the 256x1 RGBA8 lookup the compute
 * shader indexes into using vk_texture's value as the U
 * coordinate.  (Phase 5b-01 then promoted vk_texture to
 * R8_UINT + STORAGE_BIT so compute SW raster ports can
 * imageStore palette indices directly; see the format-
 * choice rationale at the vk_texture CreateImage call
 * site.)
 *
 * Phase 4g: the descriptor set grows from 2 bindings to 3.
 * Binding 2 is a writeonly STORAGE_IMAGE pointing at
 * vk_image_view -- the compute dispatch's output target.
 * The same vk_image_view is reused for both compute-write
 * (descriptor binding 2 with VK_IMAGE_LAYOUT_GENERAL) and
 * frontend-sample (libretro set_image with VK_IMAGE_LAYOUT_
 * SHADER_READ_ONLY_OPTIMAL); R8G8B8A8_UNORM is storage-
 * capable per the Vulkan 1.0 mandatory format support table
 * and the view inherits the image's full usage flags.
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

/* --------------------------------------------------------
 * Phase 5b-02: GPU compute particle rasterizer resources.
 *
 * vk_zbuffer is the GPU side of d_pzbuffer.  R16_UINT
 * storage image (matches the SW path's `short *` Z values
 * bit-for-bit; the Z-test only ever compares non-negative
 * izi values so unsigned read works), sized to width x
 * height.  Filled from a host-visible staging buffer
 * (vk_particles_zstaging) once per frame ahead of the
 * particle dispatch, so the dispatch's Z-tests see the
 * world / alias / brush Z values the CPU SW raster left
 * in d_pzbuffer.  No need to read this back to the CPU --
 * subsequent CPU stages don't depend on whatever the
 * particle pass wrote to the GPU copy.
 *
 * vk_particles_buffer is an SSBO holding up to
 * VK_PARTICLES_MAX records of struct vk_gpu_particle (vec3
 * origin + uint colour, 16 bytes each).  Host-visible +
 * host-coherent so backend_vk_dispatch_3d_particles can
 * memcpy directly with no Flush.  Capacity sized
 * generously above Quake's typical active count (default
 * MAX_PARTICLES is 2048; the cmdline can raise
 * r_numparticles arbitrarily but actual active particles
 * rarely exceed a few hundred in normal gameplay).
 * Overflow is silently truncated -- a degenerate scene
 * with thousands of simultaneous particles is a glitch
 * worth accepting over wasting hundreds of MiB on a worst-
 * case allocation.
 *
 * vk_particles_zstaging is the host-visible staging
 * buffer for d_pzbuffer uploads.  Separate from
 * vk_staging_buffer (which is sized for the vid.buffer
 * index data + the 1 KiB palette, no room for the 2-byte-
 * per-pixel zbuffer) -- simpler than retrofitting the
 * existing buffer layout.  Sized to width * height * 2.
 *
 * vk_particles_cs_module / vk_particles_pipeline_layout /
 * vk_particles_pipeline are the compute pipeline objects
 * for the particles.comp shader.  Layout: a 3-binding DSL
 * (SSBO at binding 0, vk_texture as storage image at
 * binding 1, vk_zbuffer as storage image at binding 2) +
 * a 96-byte push-constant block (struct vk_particles_pc).
 *
 * vk_particles_dsl / vk_particles_pool / vk_particles_set
 * are the particle pipeline's descriptor objects.
 * Separate pool from vk_descriptor_pool because the
 * binding layouts differ; sharing would complicate the
 * pool sizing.  Allocated once at create_resources,
 * updated once with the final image views / SSBO handle.
 *
 * vk_pending_particle_count is the per-frame staging
 * count.  Set by backend_vk_dispatch_3d_particles; read
 * by backend_vk_record_frame to decide whether to emit
 * the particle dispatch + barrier sequence; reset to
 * zero after every record.  When zero, record_frame
 * follows the original (no-particle) path -- no zbuffer
 * upload, no GENERAL transitions on vk_texture, no
 * dispatch.  Avoids paying for the extra ~4 MiB upload
 * + extra barriers on frames without particles. */
#define VK_PARTICLES_MAX   8192u
#define VK_PARTICLES_BYTES (VK_PARTICLES_MAX * 16u)

struct vk_gpu_particle {
    float    origin[3];
    uint32_t color;
};

struct vk_particles_pc {
    float    r_origin[3];
    float    xcenter;
    float    r_pright[3];
    float    ycenter;
    float    r_pup[3];
    int32_t  d_y_aspect_shift;
    float    r_ppn[3];
    int32_t  d_pix_shift;
    int32_t  d_vrectx;
    int32_t  d_vrecty;
    int32_t  d_vrectright_particle;
    int32_t  d_vrectbottom_particle;
    int32_t  d_pix_min;
    int32_t  d_pix_max;
    uint32_t particle_count;
    uint32_t _pad;
};

static VkImage                vk_zbuffer;
static VkDeviceMemory         vk_zbuffer_memory;
static VkImageView            vk_zbuffer_view;
static VkBuffer               vk_particles_buffer;
static VkDeviceMemory         vk_particles_memory;
static void                  *vk_particles_ptr;
static VkBuffer               vk_particles_zstaging;
static VkDeviceMemory         vk_particles_zstaging_memory;
static void                  *vk_particles_zstaging_ptr;
static VkShaderModule         vk_particles_cs_module;
static VkPipelineLayout       vk_particles_pipeline_layout;
static VkPipeline             vk_particles_pipeline;
static VkDescriptorSetLayout  vk_particles_dsl;
static VkDescriptorPool       vk_particles_pool;
static VkDescriptorSet        vk_particles_set;
static uint32_t               vk_pending_particle_count;
static struct vk_particles_pc vk_particles_push;

/* --------------------------------------------------------
 * Phase 5b-03: GPU compute water-warp resources.
 *
 * Fused warp + palette-mapping compute pipeline that
 * replaces the regular palette dispatch (vk_compute_
 * pipeline) on frames where the player's view leaf is in
 * water.  Reads u_index (vk_texture, the just-uploaded
 * vid.buffer + any particle dispatch writes), samples it
 * with a per-pixel sin offset from intsintable, palette-
 * maps the sampled byte through u_palette, and writes
 * the RGBA result directly to u_output (vk_image, the
 * frame's final swapchain image).  Single dispatch per
 * frame -- no intermediate vk_texture_warped buffer
 * needed because the warp and palette steps are fused
 * into one shader.
 *
 * Resources:
 *   vk_warp_table_buffer  : UBO holding the 256-entry
 *                           intsintable[] from R_InitTurb,
 *                           uploaded ONCE at backend init
 *                           (the table only depends on
 *                           TURB_SCREEN_AMP and TURB_CYCLE,
 *                           neither of which change).
 *                           std140-packed as 64 ivec4s.
 *   vk_warp_table_memory  : DEVICE_LOCAL backing memory
 *                           for vk_warp_table_buffer
 *                           (uploaded via a one-shot
 *                           CopyBuffer at init time).
 *   vk_warp_cs_module     : compiled SPIR-V for
 *                           warpscreen.comp.
 *   vk_warp_dsl           : 4-binding DSL (matches the
 *                           palette compute's 3-binding
 *                           DSL plus binding 3 for the
 *                           sin table UBO).
 *   vk_warp_pool          : descriptor pool, 1 set
 *                           sized for the four bindings.
 *   vk_warp_set           : the allocated descriptor set;
 *                           bindings 0-2 point at the
 *                           same handles vk_descriptor_set
 *                           does (vk_texture / vk_palette
 *                           _texture / vk_image), binding
 *                           3 at vk_warp_table_buffer.
 *   vk_warp_pipeline_layout : DSL + 32 B push constants.
 *   vk_warp_pipeline      : the compute pipeline.
 *
 * Per-frame state:
 *   vk_warp_active        : set by backend_vk_dispatch_3d_
 *                           warp_screen_impl, read by
 *                           record_frame to choose between
 *                           the regular palette dispatch
 *                           and the warp+palette dispatch.
 *                           Reset to false at end of
 *                           record_frame.
 *   vk_warp_push          : push-constant snapshot at
 *                           dispatch-stage time
 *                           (phase / scr_vrect bounds);
 *                           record_frame pushes this
 *                           verbatim.
 */
#define VK_WARP_TABLE_SIZE  (2u * 128u)              /* TURB_TABLE_SIZE */
#define VK_WARP_TABLE_BYTES (16u * 64u)              /* 64 ivec4 slots */

struct vk_warp_pc {
    int32_t phase;
    int32_t scr_x;
    int32_t scr_y;
    int32_t scr_w;
    int32_t scr_h;
    int32_t _pad0;
    int32_t _pad1;
    int32_t _pad2;
};

static VkBuffer               vk_warp_table_buffer;
static VkDeviceMemory         vk_warp_table_memory;
/* Temporary handles used between the warp pipeline
 * setup block and the post-cmd-pool upload block in
 * create_resources.  Held in file-static scope only so
 * the two blocks can communicate; the upload destroys
 * them before create_resources returns.  Outside
 * create_resources they're always VK_NULL_HANDLE. */
static VkBuffer               vk_warp_table_staging;
static VkDeviceMemory         vk_warp_table_staging_memory;
static VkShaderModule         vk_warp_cs_module;
static VkPipelineLayout       vk_warp_pipeline_layout;
static VkPipeline             vk_warp_pipeline;
static VkDescriptorSetLayout  vk_warp_dsl;
static VkDescriptorPool       vk_warp_pool;
static VkDescriptorSet        vk_warp_set;
static qboolean               vk_warp_active;
static struct vk_warp_pc      vk_warp_push;

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

    /* Shader / pipeline (Phase 4b for the layout, Phase 4g
     * for compute, Phase 4h for the overlay graphics
     * path).  Render-pass / framebuffer / graphics-
     * pipeline / Cmd{BeginRenderPass,EndRenderPass,Draw}
     * came back at Phase 4h for the overlay path on top
     * of the compute output.  CmdBindVertexBuffers is new
     * at Phase 4h -- the first time the file uses vertex
     * input. */
    PFN_vkCreateShaderModule              CreateShaderModule;
    PFN_vkDestroyShaderModule             DestroyShaderModule;
    PFN_vkCreatePipelineLayout            CreatePipelineLayout;
    PFN_vkDestroyPipelineLayout           DestroyPipelineLayout;
    PFN_vkCreateComputePipelines          CreateComputePipelines;
    PFN_vkCreateGraphicsPipelines         CreateGraphicsPipelines;
    PFN_vkDestroyPipeline                 DestroyPipeline;
    PFN_vkCreateRenderPass                CreateRenderPass;
    PFN_vkDestroyRenderPass               DestroyRenderPass;
    PFN_vkCreateFramebuffer               CreateFramebuffer;
    PFN_vkDestroyFramebuffer              DestroyFramebuffer;
    PFN_vkCmdBindPipeline                 CmdBindPipeline;
    PFN_vkCmdDispatch                     CmdDispatch;
    PFN_vkCmdBeginRenderPass              CmdBeginRenderPass;
    PFN_vkCmdEndRenderPass                CmdEndRenderPass;
    PFN_vkCmdDraw                         CmdDraw;
    PFN_vkCmdBindVertexBuffers            CmdBindVertexBuffers;

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
    PFN_vkCmdPushConstants                CmdPushConstants;
    PFN_vkCmdCopyBuffer                   CmdCopyBuffer;
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
 * Phase 4j: batched one-shot overlay-pic uploads.
 *
 * Three helpers cooperate.  backend_vk_begin_uploads
 * opens a recording on vk_cmd_buffer (one-time-submit
 * usage) and resets the staging-buffer running offset.
 * backend_vk_upload_pic_slot then runs once per pic:
 * it creates the slot's image + memory + view +
 * descriptor set, copies the pic's bytes into the
 * staging buffer at the running offset, records the
 * required pipeline barriers + CmdCopyBufferToImage into
 * the same command buffer, updates the descriptor set,
 * and advances the offset.  backend_vk_end_uploads
 * closes the buffer, submits it, and QueueWaitIdles so
 * everything is visible by the time create_resources
 * returns.  vk_cmd_buffer auto-resets on the next
 * per-frame BeginCommandBuffer; the staging region is
 * overwritten by the first per-frame vid.buffer upload
 * (assuming the running offset stays within the
 * vk_staging_size budget -- which it trivially does for
 * Phase 4j's three small pics, totalling 1024 + 256 +
 * 256 = 1536 bytes against ~1.2 MB of staging).
 *
 * These helpers will be reused in Phase 4k from inside
 * the Draw_Pic intercept's "qpic_t not in cache, upload
 * it" path -- the cache lookup decides _whether_ to
 * call upload_pic_slot, and the helpers do the same
 * thing in either case.
 */
static qboolean
backend_vk_begin_uploads(void)
{
    VkCommandBufferBeginInfo bi;
    VkResult                 r;

    memset(&bi, 0, sizeof(bi));
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    r = vk_fn.BeginCommandBuffer(vk_cmd_buffer, &bi);
    if (r != VK_SUCCESS) {
        if (log_cb)
            log_cb(RETRO_LOG_ERROR,
                   "rhi-vk: BeginCommandBuffer (upload batch) failed (%d)\n",
                   (int)r);
        return false;
    }
    vk_overlay_upload_offset = 0;
    return true;
}

static qboolean
backend_vk_upload_pic_slot(unsigned slot_idx,
                           unsigned w, unsigned h,
                           const uint8_t *data)
{
    struct overlay_slot         *slot;
    VkImageCreateInfo            image_ci;
    VkMemoryRequirements         mem_req;
    VkMemoryAllocateInfo         alloc_info;
    VkImageViewCreateInfo        view_ci;
    VkDescriptorSetAllocateInfo  ds_alloc;
    VkDescriptorImageInfo        ds_image_infos[3];
    VkWriteDescriptorSet         ds_writes[3];
    VkImageMemoryBarrier         barrier;
    VkBufferImageCopy            region;
    uint32_t                     mem_type;
    size_t                       bytes;
    VkResult                     r;

    if (slot_idx >= OVERLAY_SLOT_MAX) {
        if (log_cb)
            log_cb(RETRO_LOG_ERROR,
                   "rhi-vk: upload_pic_slot: slot_idx %u >= %d\n",
                   slot_idx, OVERLAY_SLOT_MAX);
        return false;
    }
    slot = &vk_overlay_slots[slot_idx];
    if (slot->image != VK_NULL_HANDLE) {
        if (log_cb)
            log_cb(RETRO_LOG_ERROR,
                   "rhi-vk: upload_pic_slot: slot %u already in use\n",
                   slot_idx);
        return false;
    }

    bytes = (size_t)w * (size_t)h;
    if (vk_overlay_upload_offset + bytes > vk_staging_size) {
        if (log_cb)
            log_cb(RETRO_LOG_ERROR,
                   "rhi-vk: upload_pic_slot: staging overflow "
                   "(offset %llu + %llu > %llu)\n",
                   (unsigned long long)vk_overlay_upload_offset,
                   (unsigned long long)bytes,
                   (unsigned long long)vk_staging_size);
        return false;
    }

    /* Slot's GPU image: R8_UNORM palette-index pic. */
    memset(&image_ci, 0, sizeof(image_ci));
    image_ci.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_ci.imageType     = VK_IMAGE_TYPE_2D;
    image_ci.format        = VK_FORMAT_R8_UNORM;
    image_ci.extent.width  = w;
    image_ci.extent.height = h;
    image_ci.extent.depth  = 1;
    image_ci.mipLevels     = 1;
    image_ci.arrayLayers   = 1;
    image_ci.samples       = VK_SAMPLE_COUNT_1_BIT;
    image_ci.tiling        = VK_IMAGE_TILING_OPTIMAL;
    image_ci.usage         = VK_IMAGE_USAGE_SAMPLED_BIT
                           | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    image_ci.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    image_ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    r = vk_fn.CreateImage(vk_device, &image_ci, NULL, &slot->image);
    if (r != VK_SUCCESS) {
        if (log_cb)
            log_cb(RETRO_LOG_ERROR,
                   "rhi-vk: CreateImage (slot %u) failed (%d)\n",
                   slot_idx, (int)r);
        return false;
    }

    vk_fn.GetImageMemoryRequirements(vk_device, slot->image, &mem_req);
    mem_type = backend_vk_find_memory_type(mem_req.memoryTypeBits,
                                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mem_type == 0xFFFFFFFFu) {
        if (log_cb)
            log_cb(RETRO_LOG_ERROR,
                   "rhi-vk: no DEVICE_LOCAL memory for slot %u image\n",
                   slot_idx);
        return false;
    }

    memset(&alloc_info, 0, sizeof(alloc_info));
    alloc_info.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize  = mem_req.size;
    alloc_info.memoryTypeIndex = mem_type;

    r = vk_fn.AllocateMemory(vk_device, &alloc_info, NULL, &slot->memory);
    if (r != VK_SUCCESS) {
        if (log_cb)
            log_cb(RETRO_LOG_ERROR,
                   "rhi-vk: AllocateMemory (slot %u) failed (%d)\n",
                   slot_idx, (int)r);
        return false;
    }

    r = vk_fn.BindImageMemory(vk_device, slot->image, slot->memory, 0);
    if (r != VK_SUCCESS) {
        if (log_cb)
            log_cb(RETRO_LOG_ERROR,
                   "rhi-vk: BindImageMemory (slot %u) failed (%d)\n",
                   slot_idx, (int)r);
        return false;
    }

    memset(&view_ci, 0, sizeof(view_ci));
    view_ci.sType                       = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_ci.image                       = slot->image;
    view_ci.viewType                    = VK_IMAGE_VIEW_TYPE_2D;
    view_ci.format                      = VK_FORMAT_R8_UNORM;
    view_ci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_ci.subresourceRange.levelCount = 1;
    view_ci.subresourceRange.layerCount = 1;

    r = vk_fn.CreateImageView(vk_device, &view_ci, NULL, &slot->view);
    if (r != VK_SUCCESS) {
        if (log_cb)
            log_cb(RETRO_LOG_ERROR,
                   "rhi-vk: CreateImageView (slot %u) failed (%d)\n",
                   slot_idx, (int)r);
        return false;
    }

    /* Per-slot descriptor set out of the pool reserved at
     * pool-creation time.  Binding 0 -> this slot's view,
     * binding 1 -> shared palette, binding 2 -> vk_image
     * placeholder (DSL has the slot; the overlay FS
     * doesn't access it). */
    memset(&ds_alloc, 0, sizeof(ds_alloc));
    ds_alloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ds_alloc.descriptorPool     = vk_descriptor_pool;
    ds_alloc.descriptorSetCount = 1;
    ds_alloc.pSetLayouts        = &vk_descriptor_set_layout;

    r = vk_fn.AllocateDescriptorSets(vk_device, &ds_alloc, &slot->descriptor_set);
    if (r != VK_SUCCESS) {
        if (log_cb)
            log_cb(RETRO_LOG_ERROR,
                   "rhi-vk: AllocateDescriptorSets (slot %u) failed (%d)\n",
                   slot_idx, (int)r);
        return false;
    }

    memset(&ds_image_infos, 0, sizeof(ds_image_infos));
    ds_image_infos[0].sampler     = vk_sampler;
    ds_image_infos[0].imageView   = slot->view;
    ds_image_infos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    ds_image_infos[1].sampler     = vk_sampler;
    ds_image_infos[1].imageView   = vk_palette_texture_view;
    ds_image_infos[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    ds_image_infos[2].sampler     = VK_NULL_HANDLE;
    ds_image_infos[2].imageView   = vk_image_view;
    ds_image_infos[2].imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    memset(&ds_writes, 0, sizeof(ds_writes));
    ds_writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    ds_writes[0].dstSet          = slot->descriptor_set;
    ds_writes[0].dstBinding      = 0;
    ds_writes[0].descriptorCount = 1;
    ds_writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    ds_writes[0].pImageInfo      = &ds_image_infos[0];
    ds_writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    ds_writes[1].dstSet          = slot->descriptor_set;
    ds_writes[1].dstBinding      = 1;
    ds_writes[1].descriptorCount = 1;
    ds_writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    ds_writes[1].pImageInfo      = &ds_image_infos[1];
    ds_writes[2].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    ds_writes[2].dstSet          = slot->descriptor_set;
    ds_writes[2].dstBinding      = 2;
    ds_writes[2].descriptorCount = 1;
    ds_writes[2].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    ds_writes[2].pImageInfo      = &ds_image_infos[2];

    vk_fn.UpdateDescriptorSets(vk_device, 3, ds_writes, 0, NULL);

    /* Copy the pic into the staging buffer at the running
     * offset, then record barrier + copy + barrier into
     * the open command buffer. */
    memcpy((uint8_t *)vk_staging_ptr + vk_overlay_upload_offset, data, bytes);

    memset(&barrier, 0, sizeof(barrier));
    barrier.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask                   = 0;
    barrier.dstAccessMask                   = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.oldLayout                       = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout                       = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
    barrier.image                           = slot->image;
    barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount     = 1;
    barrier.subresourceRange.layerCount     = 1;

    vk_fn.CmdPipelineBarrier(vk_cmd_buffer,
                             VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0,
                             0, NULL, 0, NULL,
                             1, &barrier);

    memset(&region, 0, sizeof(region));
    region.bufferOffset                    = vk_overlay_upload_offset;
    region.imageSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount     = 1;
    region.imageExtent.width               = w;
    region.imageExtent.height              = h;
    region.imageExtent.depth               = 1;

    vk_fn.CmdCopyBufferToImage(vk_cmd_buffer,
                               vk_staging_buffer,
                               slot->image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               1, &region);

    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    vk_fn.CmdPipelineBarrier(vk_cmd_buffer,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0,
                             0, NULL, 0, NULL,
                             1, &barrier);

    slot->width  = w;
    slot->height = h;
    vk_overlay_upload_offset += bytes;
    return true;
}

/*
 * backend_vk_refresh_pic_slot -- re-upload the pixel
 * contents of an already-allocated slot in place.
 *
 * backend_vk_upload_pic_slot above creates a fresh slot
 * (image, memory, view, descriptor set) and uploads the
 * initial pixels.  That's wrong for the player-colour
 * preview's translated pic, which has constant
 * dimensions but pixel contents that change every time
 * the user navigates the colour picker -- we'd either
 * tear down and re-create the slot every frame (leaks
 * descriptor sets because the pool isn't flagged
 * FREE_DESCRIPTOR_SET_BIT), or refresh in place.  This
 * helper does the in-place refresh.
 *
 * Assumes the slot already exists (image, view,
 * descriptor set bound) and its current layout is
 * SHADER_READ_ONLY_OPTIMAL (the layout
 * backend_vk_upload_pic_slot leaves it in).  Restores
 * that layout at the end so subsequent overlay draws
 * sample correctly.  Width / height match the slot's
 * existing dimensions; caller is responsible for
 * passing a `data` buffer of the right size.
 *
 * Must be called inside a backend_vk_begin_uploads /
 * end_uploads pair, same as the upload helper.
 */
static qboolean
backend_vk_refresh_pic_slot(unsigned slot_idx, const uint8_t *data)
{
    struct overlay_slot  *slot;
    VkImageMemoryBarrier  barrier;
    VkBufferImageCopy     region;
    size_t                bytes;

    if (slot_idx >= OVERLAY_SLOT_MAX) {
        if (log_cb)
            log_cb(RETRO_LOG_ERROR,
                   "rhi-vk: refresh_pic_slot: slot_idx %u >= %d\n",
                   slot_idx, OVERLAY_SLOT_MAX);
        return false;
    }
    slot = &vk_overlay_slots[slot_idx];
    if (slot->image == VK_NULL_HANDLE) {
        if (log_cb)
            log_cb(RETRO_LOG_ERROR,
                   "rhi-vk: refresh_pic_slot: slot %u empty\n",
                   slot_idx);
        return false;
    }

    bytes = (size_t)slot->width * (size_t)slot->height;
    if (vk_overlay_upload_offset + bytes > vk_staging_size) {
        if (log_cb)
            log_cb(RETRO_LOG_ERROR,
                   "rhi-vk: refresh_pic_slot: staging overflow "
                   "(offset %llu + %llu > %llu)\n",
                   (unsigned long long)vk_overlay_upload_offset,
                   (unsigned long long)bytes,
                   (unsigned long long)vk_staging_size);
        return false;
    }

    /* SHADER_READ_ONLY_OPTIMAL -> TRANSFER_DST_OPTIMAL */
    memset(&barrier, 0, sizeof(barrier));
    barrier.sType                       = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask               = VK_ACCESS_SHADER_READ_BIT;
    barrier.dstAccessMask               = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.oldLayout                   = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.newLayout                   = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
    barrier.image                       = slot->image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;

    vk_fn.CmdPipelineBarrier(vk_cmd_buffer,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0,
                             0, NULL, 0, NULL,
                             1, &barrier);

    /* Staging copy + image copy */
    memcpy((uint8_t *)vk_staging_ptr + vk_overlay_upload_offset, data, bytes);

    memset(&region, 0, sizeof(region));
    region.bufferOffset                = vk_overlay_upload_offset;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent.width           = slot->width;
    region.imageExtent.height          = slot->height;
    region.imageExtent.depth           = 1;

    vk_fn.CmdCopyBufferToImage(vk_cmd_buffer,
                               vk_staging_buffer,
                               slot->image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               1, &region);

    /* TRANSFER_DST_OPTIMAL -> SHADER_READ_ONLY_OPTIMAL */
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    vk_fn.CmdPipelineBarrier(vk_cmd_buffer,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0,
                             0, NULL, 0, NULL,
                             1, &barrier);

    vk_overlay_upload_offset += bytes;
    return true;
}

static qboolean
backend_vk_end_uploads(void)
{
    VkSubmitInfo submit;
    VkResult     r;

    r = vk_fn.EndCommandBuffer(vk_cmd_buffer);
    if (r != VK_SUCCESS) {
        if (log_cb)
            log_cb(RETRO_LOG_ERROR,
                   "rhi-vk: EndCommandBuffer (upload batch) failed (%d)\n",
                   (int)r);
        return false;
    }

    memset(&submit, 0, sizeof(submit));
    submit.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers    = &vk_cmd_buffer;

    r = vk_fn.QueueSubmit(vk_queue, 1, &submit, VK_NULL_HANDLE);
    if (r != VK_SUCCESS) {
        if (log_cb)
            log_cb(RETRO_LOG_ERROR,
                   "rhi-vk: QueueSubmit (upload batch) failed (%d)\n",
                   (int)r);
        return false;
    }

    /* Wait for the upload batch to complete before
     * create_resources lets the frontend start rendering.
     * vk_cmd_buffer auto-resets on its next
     * BeginCommandBuffer (pool's
     * RESET_COMMAND_BUFFER_BIT covers it). */
    vk_fn.QueueWaitIdle(vk_queue);
    return true;
}

/*
 * Create the per-context render target (image + memory + view),
 * the compute-pipeline objects (shader module + pipeline layout
 * + compute pipeline), the texture / palette-texture / staging
 * resources, the descriptor set, and the command-buffer
 * infrastructure (pool + one primary buffer).  Per-frame
 * command recording (Phase 4e) is dispatched from
 * backend_vk_record_frame, not here.  On any failure rolls
 * back and leaves vk_resources_ready false; end_frame then
 * no-ops and the frontend falls back to dupe.
 */
static qboolean
backend_vk_create_resources(void)
{
    VkImageCreateInfo                       image_ci;
    VkMemoryRequirements                    mem_req;
    VkMemoryAllocateInfo                    alloc_info;
    VkImageViewCreateInfo                   view_ci;
    VkShaderModuleCreateInfo                sm_ci;
    VkPipelineLayoutCreateInfo              pl_ci;
    VkPipelineShaderStageCreateInfo         cs_stage;
    VkComputePipelineCreateInfo             cp_ci;
    VkCommandPoolCreateInfo                 pool_ci;
    VkCommandBufferAllocateInfo             cmd_alloc;
    VkSamplerCreateInfo                     sampler_ci;
    VkBufferCreateInfo                      buf_ci;
    VkDescriptorSetLayoutBinding            dsl_bindings[3];
    VkDescriptorSetLayoutCreateInfo         dsl_ci;
    VkDescriptorPoolSize                    dp_sizes[2];
    VkDescriptorPoolCreateInfo              dp_ci;
    VkDescriptorSetAllocateInfo             ds_alloc;
    VkDescriptorImageInfo                   ds_image_infos[3];
    VkWriteDescriptorSet                    ds_writes[3];
    /* Phase 4h: overlay render-pass + graphics-pipeline locals.
     * Kept inline rather than factored into a helper so the
     * single-function create-resources structure is preserved;
     * the overlay setup is mechanically distinct from the
     * compute setup above but uses the same conventions
     * (memset-to-zero, sType, populate, create, error-check). */
    VkAttachmentDescription                 ov_attachment;
    VkAttachmentReference                   ov_color_ref;
    VkSubpassDescription                    ov_subpass;
    VkSubpassDependency                     ov_dep;
    VkRenderPassCreateInfo                  ov_rp_ci;
    VkFramebufferCreateInfo                 ov_fb_ci;
    VkImageView                             ov_fb_attachments[1];
    VkPipelineShaderStageCreateInfo         ov_stages[2];
    VkVertexInputBindingDescription         ov_vb_binding;
    VkVertexInputAttributeDescription       ov_vb_attribs[2];
    VkPipelineVertexInputStateCreateInfo    ov_vi;
    VkPipelineInputAssemblyStateCreateInfo  ov_ia;
    VkViewport                              ov_viewport;
    VkRect2D                                ov_scissor;
    VkPipelineViewportStateCreateInfo       ov_vp;
    VkPipelineRasterizationStateCreateInfo  ov_rs;
    VkPipelineMultisampleStateCreateInfo    ov_ms;
    VkPipelineColorBlendAttachmentState     ov_cba;
    VkPipelineColorBlendStateCreateInfo     ov_cb;
    VkGraphicsPipelineCreateInfo            ov_gp_ci;
    VkBufferCreateInfo                      ov_vb_ci;
    VkMemoryRequirements                    ov_vb_mem_req;
    VkMemoryAllocateInfo                    ov_vb_alloc;
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
     * Compute shader module.  Just the SPIR-V bytecode
     * wrapped -- the module can be destroyed immediately
     * after the pipeline that uses it is created, but we
     * keep it around for the lifetime of the context to
     * keep teardown symmetric.  No per-frame cost; the
     * driver compiled the bytecode into its internal ISA at
     * pipeline creation. */
    memset(&sm_ci, 0, sizeof(sm_ci));
    sm_ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    sm_ci.codeSize = spv_textured_palette_cs_size;
    sm_ci.pCode    = spv_textured_palette_cs;
    r = vk_fn.CreateShaderModule(vk_device, &sm_ci, NULL, &vk_cs_module);
    if (r != VK_SUCCESS) {
        if (log_cb)
            log_cb(RETRO_LOG_ERROR,
                   "rhi-vk: CreateShaderModule (cs) failed (%d)\n", (int)r);
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
     * Source index texture for the palette compute shader to
     * sample from.  Sized 1:1 with the SW framebuffer (width x
     * height), R8_UINT (one byte per pixel, raw palette
     * index from vid.buffer).  OPTIMAL tiling, DEVICE_LOCAL
     * memory.  Usage covers the data path we need:
     * TRANSFER_DST for the CPU-side upload from vid.buffer
     * (still the producer when compute_rendering is off, and
     * the producer of the unported CPU stages when on),
     * SAMPLED for the palette compute shader's read, and
     * STORAGE so the Phase 5b compute SW rasterizer ports
     * (particles, alias, brush, sprite, sky, liquids, world
     * surfaces) can imageStore palette indices directly.
     *
     * Phase 5b-01 switched the format R8_UNORM -> R8_UINT
     * specifically to satisfy the STORAGE requirement:
     * Vulkan 1.0's mandatory format-support table requires
     * R8_UINT (and R8_SINT) to support storage-image use
     * unconditionally on every implementation; R8_UNORM is
     * optional for storage and a portable backend can't
     * assume it.  The format change is functionally
     * invisible -- vk_texture holds 8-bit palette indices
     * either way, and the palette compute shader was
     * updated in lockstep (sampler2D -> usampler2D, integer
     * texelFetch return) to consume the new format.  No
     * MUTABLE_FORMAT flag needed because every consumer
     * now agrees on the integer interpretation. */
    memset(&image_ci, 0, sizeof(image_ci));
    image_ci.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_ci.imageType     = VK_IMAGE_TYPE_2D;
    image_ci.format        = VK_FORMAT_R8_UINT;
    image_ci.extent.width  = width;
    image_ci.extent.height = height;
    image_ci.extent.depth  = 1;
    image_ci.mipLevels     = 1;
    image_ci.arrayLayers   = 1;
    image_ci.samples       = VK_SAMPLE_COUNT_1_BIT;
    image_ci.tiling        = VK_IMAGE_TILING_OPTIMAL;
    image_ci.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT
                           | VK_IMAGE_USAGE_SAMPLED_BIT
                           | VK_IMAGE_USAGE_STORAGE_BIT;
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
    view_ci.format           = VK_FORMAT_R8_UINT;
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
     * palette entry.  The palette compute shader fetches
     * this with texelFetch at u = idx (the R8_UINT index
     * value, which arrives as a uint in [0, 255] after the
     * Phase 5b-01 format change).  texelFetch with the
     * integer coord lands directly on the palette entry --
     * no normalised-coord rounding to worry about. */
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
     * Descriptor set layout (Phase 4g).  Three bindings,
     * matching textured_palette.comp's
     *   layout(set = 0, binding = 0) uniform sampler2D       u_index;
     *   layout(set = 0, binding = 1) uniform sampler2D       u_palette;
     *   layout(set = 0, binding = 2, rgba8) uniform writeonly image2D u_output;
     * stageFlags on the sampler bindings stay FRAGMENT |
     * COMPUTE -- COMPUTE because the compute pipeline reads
     * them, FRAGMENT preserved for a future Phase 4h
     * graphics pipeline that might want to sample the same
     * textures (e.g. an overlay pass that composites onto
     * the compute output).  The storage-image binding is
     * COMPUTE-only since fragment shaders write through
     * attachments, not storage descriptors. */
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
    dsl_bindings[2].binding         = 2;
    dsl_bindings[2].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    dsl_bindings[2].descriptorCount = 1;
    dsl_bindings[2].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    memset(&dsl_ci, 0, sizeof(dsl_ci));
    dsl_ci.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dsl_ci.bindingCount = 3;
    dsl_ci.pBindings    = dsl_bindings;

    r = vk_fn.CreateDescriptorSetLayout(vk_device, &dsl_ci, NULL,
                                        &vk_descriptor_set_layout);
    if (r != VK_SUCCESS) {
        if (log_cb)
            log_cb(RETRO_LOG_ERROR,
                   "rhi-vk: CreateDescriptorSetLayout failed (%d)\n", (int)r);
        return false;
    }

    /* Descriptor pool sized for the compute set plus one
     * per overlay slot.  Each set needs 2 sampler
     * descriptors (binding 0 = index, binding 1 =
     * palette) + 1 storage-image descriptor (binding 2;
     * overlay sets use a placeholder, but the DSL has
     * the slot so the pool must account for it). */
    memset(&dp_sizes, 0, sizeof(dp_sizes));
    dp_sizes[0].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    dp_sizes[0].descriptorCount = 2 * (1 + OVERLAY_SLOT_MAX);
    dp_sizes[1].type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    dp_sizes[1].descriptorCount = 1 + OVERLAY_SLOT_MAX;

    memset(&dp_ci, 0, sizeof(dp_ci));
    dp_ci.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dp_ci.maxSets       = 1 + OVERLAY_SLOT_MAX;
    dp_ci.poolSizeCount = 2;
    dp_ci.pPoolSizes    = dp_sizes;

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
    /* Per-slot overlay descriptor sets are allocated
     * lazily by backend_vk_upload_pic_slot when each slot
     * is first populated.  The pool reservation above
     * guarantees the allocations will succeed up to
     * OVERLAY_SLOT_MAX. */

    /* Point each binding at its image + (for the sampled
     * bindings) the shared sampler.  The imageLayout we
     * declare here is what the shader expects to find at
     * dispatch time.  record_frame's per-frame transitions
     * land vk_texture and vk_palette_texture in
     * SHADER_READ_ONLY_OPTIMAL for the compute reads, and
     * vk_image in GENERAL for the compute write.
     *
     * Binding 2 has no sampler: storage-image descriptors
     * sample-less, the shader uses imageStore directly. */
    memset(&ds_image_infos, 0, sizeof(ds_image_infos));
    ds_image_infos[0].sampler     = vk_sampler;
    ds_image_infos[0].imageView   = vk_texture_view;
    ds_image_infos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    ds_image_infos[1].sampler     = vk_sampler;
    ds_image_infos[1].imageView   = vk_palette_texture_view;
    ds_image_infos[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    ds_image_infos[2].sampler     = VK_NULL_HANDLE;
    ds_image_infos[2].imageView   = vk_image_view;
    ds_image_infos[2].imageLayout = VK_IMAGE_LAYOUT_GENERAL;

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
    ds_writes[2].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    ds_writes[2].dstSet          = vk_descriptor_set;
    ds_writes[2].dstBinding      = 2;
    ds_writes[2].dstArrayElement = 0;
    ds_writes[2].descriptorCount = 1;
    ds_writes[2].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    ds_writes[2].pImageInfo      = &ds_image_infos[2];

    vk_fn.UpdateDescriptorSets(vk_device, 3, ds_writes, 0, NULL);

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

    /* Compute pipeline.  No vertex / fragment / blend /
     * rasterizer / viewport state -- compute pipelines
     * carry just a single shader stage and the layout
     * (same vk_pipeline_layout that the graphics path used,
     * since it references the same DSL).  basePipelineHandle
     * VK_NULL_HANDLE because we don't derive from anything. */
    memset(&cs_stage, 0, sizeof(cs_stage));
    cs_stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cs_stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    cs_stage.module = vk_cs_module;
    cs_stage.pName  = "main";

    memset(&cp_ci, 0, sizeof(cp_ci));
    cp_ci.sType              = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cp_ci.stage              = cs_stage;
    cp_ci.layout             = vk_pipeline_layout;
    cp_ci.basePipelineHandle = VK_NULL_HANDLE;
    cp_ci.basePipelineIndex  = -1;

    r = vk_fn.CreateComputePipelines(vk_device, VK_NULL_HANDLE,
                                     1, &cp_ci, NULL, &vk_compute_pipeline);
    if (r != VK_SUCCESS) {
        if (log_cb)
            log_cb(RETRO_LOG_ERROR,
                   "rhi-vk: CreateComputePipelines failed (%d)\n", (int)r);
        return false;
    }

    /* ---------------------------------------------------------
     * Phase 4h: overlay graphics path.  Render pass +
     * framebuffer + graphics pipeline + vertex buffer for
     * drawing 2D quads on top of the compute output.
     *
     * Shader modules. */
    memset(&sm_ci, 0, sizeof(sm_ci));
    sm_ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    sm_ci.codeSize = spv_overlay_quad_vs_size;
    sm_ci.pCode    = spv_overlay_quad_vs;
    r = vk_fn.CreateShaderModule(vk_device, &sm_ci, NULL, &vk_overlay_vs_module);
    if (r != VK_SUCCESS) {
        if (log_cb)
            log_cb(RETRO_LOG_ERROR,
                   "rhi-vk: CreateShaderModule (overlay vs) failed (%d)\n", (int)r);
        return false;
    }

    memset(&sm_ci, 0, sizeof(sm_ci));
    sm_ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    sm_ci.codeSize = spv_overlay_quad_fs_size;
    sm_ci.pCode    = spv_overlay_quad_fs;
    r = vk_fn.CreateShaderModule(vk_device, &sm_ci, NULL, &vk_overlay_fs_module);
    if (r != VK_SUCCESS) {
        if (log_cb)
            log_cb(RETRO_LOG_ERROR,
                   "rhi-vk: CreateShaderModule (overlay fs) failed (%d)\n", (int)r);
        return false;
    }

    /* Render pass.  One COLOR attachment (the same image
     * the compute dispatch wrote to), loadOp = LOAD so the
     * compute output is preserved.  initialLayout =
     * GENERAL receives the image straight from the compute
     * dispatch -- the render pass's own attachment-layout
     * transition takes it to COLOR_ATTACHMENT_OPTIMAL for
     * the subpass, then to SHADER_READ_ONLY_OPTIMAL via
     * finalLayout for the frontend sample.
     *
     * Subpass dependency EXTERNAL -> 0 carries the memory
     * dependency between the compute write and the
     * attachment access: srcStage = COMPUTE_SHADER waits
     * for the dispatch, srcAccess = SHADER_WRITE makes
     * those writes visible; dstStage =
     * COLOR_ATTACHMENT_OUTPUT and dstAccess =
     * COLOR_ATTACHMENT_WRITE | COLOR_ATTACHMENT_READ
     * cover both the LOAD (read) and the subpass draw
     * (write+read for blend).  This replaces the explicit
     * pipeline barrier Phase 4g used to issue between
     * compute and the (then nonexistent) downstream
     * pass. */
    memset(&ov_attachment, 0, sizeof(ov_attachment));
    ov_attachment.format         = VK_FORMAT_R8G8B8A8_UNORM;
    ov_attachment.samples        = VK_SAMPLE_COUNT_1_BIT;
    ov_attachment.loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;
    ov_attachment.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    ov_attachment.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    ov_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    ov_attachment.initialLayout  = VK_IMAGE_LAYOUT_GENERAL;
    ov_attachment.finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    ov_color_ref.attachment = 0;
    ov_color_ref.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    memset(&ov_subpass, 0, sizeof(ov_subpass));
    ov_subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
    ov_subpass.colorAttachmentCount = 1;
    ov_subpass.pColorAttachments    = &ov_color_ref;

    memset(&ov_dep, 0, sizeof(ov_dep));
    ov_dep.srcSubpass    = VK_SUBPASS_EXTERNAL;
    ov_dep.dstSubpass    = 0;
    ov_dep.srcStageMask  = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    ov_dep.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    ov_dep.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    ov_dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                         | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;

    memset(&ov_rp_ci, 0, sizeof(ov_rp_ci));
    ov_rp_ci.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    ov_rp_ci.attachmentCount = 1;
    ov_rp_ci.pAttachments    = &ov_attachment;
    ov_rp_ci.subpassCount    = 1;
    ov_rp_ci.pSubpasses      = &ov_subpass;
    ov_rp_ci.dependencyCount = 1;
    ov_rp_ci.pDependencies   = &ov_dep;

    r = vk_fn.CreateRenderPass(vk_device, &ov_rp_ci, NULL, &vk_overlay_render_pass);
    if (r != VK_SUCCESS) {
        if (log_cb)
            log_cb(RETRO_LOG_ERROR,
                   "rhi-vk: CreateRenderPass (overlay) failed (%d)\n", (int)r);
        return false;
    }

    /* Framebuffer.  Wraps the same vk_image_view the
     * compute dispatch writes to, so the overlay's draws
     * land on top of the compute output. */
    ov_fb_attachments[0] = vk_image_view;
    memset(&ov_fb_ci, 0, sizeof(ov_fb_ci));
    ov_fb_ci.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    ov_fb_ci.renderPass      = vk_overlay_render_pass;
    ov_fb_ci.attachmentCount = 1;
    ov_fb_ci.pAttachments    = ov_fb_attachments;
    ov_fb_ci.width           = width;
    ov_fb_ci.height          = height;
    ov_fb_ci.layers          = 1;

    r = vk_fn.CreateFramebuffer(vk_device, &ov_fb_ci, NULL, &vk_overlay_framebuffer);
    if (r != VK_SUCCESS) {
        if (log_cb)
            log_cb(RETRO_LOG_ERROR,
                   "rhi-vk: CreateFramebuffer (overlay) failed (%d)\n", (int)r);
        return false;
    }

    /* Pipeline layout.  Phase 4i reuses vk_pipeline_layout
     * (the 3-binding compute layout) instead of creating a
     * separate empty one.  Both pipelines reference the
     * same DSL (binding 0 sampler u_index, binding 1
     * sampler u_palette, binding 2 storage_image
     * u_output), neither uses push constants -- so the
     * layouts are identical and one object covers both
     * uses.  The overlay shader ignores binding 2
     * (storage_image, COMPUTE-stage-only); the compute
     * shader ignores no bindings. */
    /* Graphics pipeline.
     *
     * Vertex input: one binding (binding 0) with stride
     * 24 bytes = sizeof(vec2) + sizeof(vec4); two
     * attributes (location 0 = R32G32_SFLOAT at offset 0
     * for position, location 1 = R32G32B32A32_SFLOAT at
     * offset 8 for colour).
     *
     * Input assembly: TRIANGLE_STRIP so 4 vertices
     * produce 2 triangles for a quad without an index
     * buffer.  Strip winding for a screen-aligned quad:
     *   v0 = bottom-left  (NDC -, +y in Vulkan = bottom)
     *   v1 = bottom-right
     *   v2 = top-left
     *   v3 = top-right
     * Note Vulkan NDC has y = -1 at the TOP of the
     * framebuffer; "bottom" above means larger y in NDC.
     *
     * Viewport / scissor: full vk_image extent; resolution
     * is fixed for the lifetime of the context (the
     * core option requires-restart), so static state is
     * fine, no VK_DYNAMIC_STATE_VIEWPORT needed.
     *
     * Rasterization: FILL, no culling (we don't care
     * about winding for 2D quads).
     *
     * Multisample: 1 sample.
     *
     * Color blend: standard alpha blending --
     *   color = src.rgb * src.a + dst.rgb * (1 - src.a)
     *   alpha = src.a       + dst.a * (1 - src.a)
     * so the overlay quad composites smoothly over the
     * compute output. */
    memset(&ov_stages[0], 0, sizeof(ov_stages[0]));
    ov_stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ov_stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    ov_stages[0].module = vk_overlay_vs_module;
    ov_stages[0].pName  = "main";
    memset(&ov_stages[1], 0, sizeof(ov_stages[1]));
    ov_stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ov_stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    ov_stages[1].module = vk_overlay_fs_module;
    ov_stages[1].pName  = "main";

    memset(&ov_vb_binding, 0, sizeof(ov_vb_binding));
    ov_vb_binding.binding   = 0;
    ov_vb_binding.stride    = 16;  /* vec2 position + vec2 uv */
    ov_vb_binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    memset(&ov_vb_attribs, 0, sizeof(ov_vb_attribs));
    ov_vb_attribs[0].location = 0;
    ov_vb_attribs[0].binding  = 0;
    ov_vb_attribs[0].format   = VK_FORMAT_R32G32_SFLOAT;
    ov_vb_attribs[0].offset   = 0;
    ov_vb_attribs[1].location = 1;
    ov_vb_attribs[1].binding  = 0;
    ov_vb_attribs[1].format   = VK_FORMAT_R32G32_SFLOAT;
    ov_vb_attribs[1].offset   = 8;

    memset(&ov_vi, 0, sizeof(ov_vi));
    ov_vi.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    ov_vi.vertexBindingDescriptionCount   = 1;
    ov_vi.pVertexBindingDescriptions      = &ov_vb_binding;
    ov_vi.vertexAttributeDescriptionCount = 2;
    ov_vi.pVertexAttributeDescriptions    = ov_vb_attribs;

    memset(&ov_ia, 0, sizeof(ov_ia));
    ov_ia.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ov_ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;

    memset(&ov_viewport, 0, sizeof(ov_viewport));
    ov_viewport.x        = 0.0f;
    ov_viewport.y        = 0.0f;
    ov_viewport.width    = (float)width;
    ov_viewport.height   = (float)height;
    ov_viewport.minDepth = 0.0f;
    ov_viewport.maxDepth = 1.0f;

    memset(&ov_scissor, 0, sizeof(ov_scissor));
    ov_scissor.extent.width  = width;
    ov_scissor.extent.height = height;

    memset(&ov_vp, 0, sizeof(ov_vp));
    ov_vp.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    ov_vp.viewportCount = 1;
    ov_vp.pViewports    = &ov_viewport;
    ov_vp.scissorCount  = 1;
    ov_vp.pScissors     = &ov_scissor;

    memset(&ov_rs, 0, sizeof(ov_rs));
    ov_rs.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    ov_rs.polygonMode = VK_POLYGON_MODE_FILL;
    ov_rs.cullMode    = VK_CULL_MODE_NONE;
    ov_rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    ov_rs.lineWidth   = 1.0f;

    memset(&ov_ms, 0, sizeof(ov_ms));
    ov_ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ov_ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    memset(&ov_cba, 0, sizeof(ov_cba));
    ov_cba.blendEnable         = VK_TRUE;
    ov_cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    ov_cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    ov_cba.colorBlendOp        = VK_BLEND_OP_ADD;
    ov_cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    ov_cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    ov_cba.alphaBlendOp        = VK_BLEND_OP_ADD;
    ov_cba.colorWriteMask      = VK_COLOR_COMPONENT_R_BIT
                               | VK_COLOR_COMPONENT_G_BIT
                               | VK_COLOR_COMPONENT_B_BIT
                               | VK_COLOR_COMPONENT_A_BIT;

    memset(&ov_cb, 0, sizeof(ov_cb));
    ov_cb.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    ov_cb.attachmentCount = 1;
    ov_cb.pAttachments    = &ov_cba;

    memset(&ov_gp_ci, 0, sizeof(ov_gp_ci));
    ov_gp_ci.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    ov_gp_ci.stageCount          = 2;
    ov_gp_ci.pStages             = ov_stages;
    ov_gp_ci.pVertexInputState   = &ov_vi;
    ov_gp_ci.pInputAssemblyState = &ov_ia;
    ov_gp_ci.pViewportState      = &ov_vp;
    ov_gp_ci.pRasterizationState = &ov_rs;
    ov_gp_ci.pMultisampleState   = &ov_ms;
    ov_gp_ci.pColorBlendState    = &ov_cb;
    ov_gp_ci.layout              = vk_pipeline_layout;
    ov_gp_ci.renderPass          = vk_overlay_render_pass;
    ov_gp_ci.subpass             = 0;
    ov_gp_ci.basePipelineHandle  = VK_NULL_HANDLE;
    ov_gp_ci.basePipelineIndex   = -1;

    r = vk_fn.CreateGraphicsPipelines(vk_device, VK_NULL_HANDLE,
                                      1, &ov_gp_ci, NULL, &vk_overlay_pipeline);
    if (r != VK_SUCCESS) {
        if (log_cb)
            log_cb(RETRO_LOG_ERROR,
                   "rhi-vk: CreateGraphicsPipelines (overlay) failed (%d)\n", (int)r);
        return false;
    }

    /* Vertex buffer.  HOST_VISIBLE + HOST_COHERENT so we
     * can write straight to it from the CPU; persistently
     * mapped so the vk_overlay_vertex_buffer_ptr stays
     * valid for the lifetime of the resources.  Size = 4
     * vertices * 24 bytes = 96 bytes.
     *
     * Contents at create_resources time: one quad in the
     * upper-left of the screen (NDC x in [-0.9, -0.7], y
     * in [-0.9, -0.7]; Vulkan NDC y goes negative
     * upward), with a different bright colour at each
     * corner so the FS interpolation produces a visible
     * gradient.  Alpha = 0.5 so the underlying content
     * shows through.
     *
     * Phase 4i will reuse this buffer for dynamic per-
     * frame quad streams (the Draw_Pic / Draw_Character
     * intercept fills it with as many quads as the SW
     * 2D drawing path requested that frame); for now
     * it's static and written once. */
    memset(&ov_vb_ci, 0, sizeof(ov_vb_ci));
    ov_vb_ci.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    ov_vb_ci.size        = OVERLAY_DRAW_MAX * 4 * 16;
                                     /* OVERLAY_DRAW_MAX quads *
                                      * 4 verts/quad * 16 bytes/vert
                                      * (vec2 pos + vec2 uv) */
    ov_vb_ci.usage       = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    ov_vb_ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    r = vk_fn.CreateBuffer(vk_device, &ov_vb_ci, NULL, &vk_overlay_vertex_buffer);
    if (r != VK_SUCCESS) {
        if (log_cb)
            log_cb(RETRO_LOG_ERROR,
                   "rhi-vk: CreateBuffer (overlay vertex) failed (%d)\n", (int)r);
        return false;
    }

    vk_fn.GetBufferMemoryRequirements(vk_device,
                                      vk_overlay_vertex_buffer,
                                      &ov_vb_mem_req);

    mem_type = backend_vk_find_memory_type(
        ov_vb_mem_req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
      | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mem_type == 0xFFFFFFFFu) {
        if (log_cb)
            log_cb(RETRO_LOG_ERROR,
                   "rhi-vk: no HOST_VISIBLE | HOST_COHERENT memory for overlay vertex buffer\n");
        return false;
    }

    memset(&ov_vb_alloc, 0, sizeof(ov_vb_alloc));
    ov_vb_alloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ov_vb_alloc.allocationSize  = ov_vb_mem_req.size;
    ov_vb_alloc.memoryTypeIndex = mem_type;

    r = vk_fn.AllocateMemory(vk_device, &ov_vb_alloc, NULL,
                             &vk_overlay_vertex_memory);
    if (r != VK_SUCCESS) {
        if (log_cb)
            log_cb(RETRO_LOG_ERROR,
                   "rhi-vk: AllocateMemory (overlay vertex) failed (%d)\n", (int)r);
        return false;
    }

    r = vk_fn.BindBufferMemory(vk_device, vk_overlay_vertex_buffer,
                               vk_overlay_vertex_memory, 0);
    if (r != VK_SUCCESS) {
        if (log_cb)
            log_cb(RETRO_LOG_ERROR,
                   "rhi-vk: BindBufferMemory (overlay vertex) failed (%d)\n", (int)r);
        return false;
    }

    vk_overlay_vertex_ptr = NULL;
    r = vk_fn.MapMemory(vk_device, vk_overlay_vertex_memory, 0,
                        ov_vb_ci.size, 0, &vk_overlay_vertex_ptr);
    if (r != VK_SUCCESS || !vk_overlay_vertex_ptr) {
        if (log_cb)
            log_cb(RETRO_LOG_ERROR,
                   "rhi-vk: MapMemory (overlay vertex) failed (%d)\n", (int)r);
        return false;
    }
    /* Buffer contents are written below after the demo
     * draw list is populated; the persistent mapping
     * lives until vkFreeMemory at teardown. */
    /* Don't UnmapMemory: HOST_COHERENT, persistent map
     * is fine (and required to avoid retracted memory
     * mapping invariants in some drivers).  vkFreeMemory
     * at teardown implicitly unmaps. */

    /* ---------------------------------------------------------
     * Phase 5b-02: GPU compute particle rasterizer resources.
     *
     * Order:
     *   1. vk_zbuffer image + memory + view.
     *   2. vk_particles_buffer (SSBO) + memory + map.
     *   3. vk_particles_zstaging (host-visible staging for
     *      d_pzbuffer uploads) + memory + map.
     *   4. vk_particles_cs_module from the precompiled SPIR-V.
     *   5. vk_particles_dsl (3-binding layout: SSBO + 2 storage
     *      images).
     *   6. vk_particles_pool + AllocateDescriptorSets ->
     *      vk_particles_set.
     *   7. UpdateDescriptorSets with the buffer + view handles.
     *   8. vk_particles_pipeline_layout (DSL + 96 B push constants).
     *   9. CreateComputePipelines -> vk_particles_pipeline.
     *
     * Each step on failure jumps to the function's "return
     * false" path; destroy_resources unwinds whatever got
     * built. */
    {
        VkImageCreateInfo            zb_ci;
        VkMemoryRequirements         zb_mem_req;
        VkMemoryAllocateInfo         zb_mem_ai;
        VkImageViewCreateInfo        zb_view_ci;
        VkBufferCreateInfo           pb_ci;
        VkMemoryRequirements         pb_mem_req;
        VkMemoryAllocateInfo         pb_mem_ai;
        VkBufferCreateInfo           zs_ci;
        VkMemoryRequirements         zs_mem_req;
        VkMemoryAllocateInfo         zs_mem_ai;
        VkShaderModuleCreateInfo     psm_ci;
        VkDescriptorSetLayoutBinding p_dsl_bindings[3];
        VkDescriptorSetLayoutCreateInfo p_dsl_ci;
        VkDescriptorPoolSize         p_pool_sizes[2];
        VkDescriptorPoolCreateInfo   p_pool_ci;
        VkDescriptorSetAllocateInfo  p_set_alloc;
        VkDescriptorBufferInfo       p_buf_info;
        VkDescriptorImageInfo        p_img_info[2];
        VkWriteDescriptorSet         p_writes[3];
        VkPushConstantRange          p_push_range;
        VkPipelineLayoutCreateInfo   p_pl_ci;
        VkComputePipelineCreateInfo  p_cp_ci;
        VkPipelineShaderStageCreateInfo p_cs_stage;
        uint32_t                     zb_mem_type;
        uint32_t                     pb_mem_type;
        uint32_t                     zs_mem_type;

        /* 1. vk_zbuffer image (R16_UINT storage + transfer
         *    destination + sampled).  Mirrors d_pzbuffer's
         *    layout: width x height, one 16-bit value per
         *    pixel.  R16_UINT is in the Vulkan 1.0 mandatory
         *    storage-image format table. */
        memset(&zb_ci, 0, sizeof(zb_ci));
        zb_ci.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        zb_ci.imageType     = VK_IMAGE_TYPE_2D;
        zb_ci.format        = VK_FORMAT_R16_UINT;
        zb_ci.extent.width  = width;
        zb_ci.extent.height = height;
        zb_ci.extent.depth  = 1;
        zb_ci.mipLevels     = 1;
        zb_ci.arrayLayers   = 1;
        zb_ci.samples       = VK_SAMPLE_COUNT_1_BIT;
        zb_ci.tiling        = VK_IMAGE_TILING_OPTIMAL;
        zb_ci.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT
                            | VK_IMAGE_USAGE_STORAGE_BIT;
        zb_ci.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        zb_ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        r = vk_fn.CreateImage(vk_device, &zb_ci, NULL, &vk_zbuffer);
        if (r != VK_SUCCESS) {
            if (log_cb)
                log_cb(RETRO_LOG_ERROR,
                       "rhi-vk: CreateImage (zbuffer) failed (%d)\n", (int)r);
            return false;
        }

        memset(&zb_mem_req, 0, sizeof(zb_mem_req));
        vk_fn.GetImageMemoryRequirements(vk_device, vk_zbuffer, &zb_mem_req);
        zb_mem_type = backend_vk_find_memory_type(
            zb_mem_req.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (zb_mem_type == UINT32_MAX) {
            if (log_cb)
                log_cb(RETRO_LOG_ERROR,
                       "rhi-vk: no DEVICE_LOCAL memory type for zbuffer\n");
            return false;
        }

        memset(&zb_mem_ai, 0, sizeof(zb_mem_ai));
        zb_mem_ai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        zb_mem_ai.allocationSize  = zb_mem_req.size;
        zb_mem_ai.memoryTypeIndex = zb_mem_type;

        r = vk_fn.AllocateMemory(vk_device, &zb_mem_ai, NULL, &vk_zbuffer_memory);
        if (r != VK_SUCCESS) {
            if (log_cb)
                log_cb(RETRO_LOG_ERROR,
                       "rhi-vk: AllocateMemory (zbuffer) failed (%d)\n", (int)r);
            return false;
        }

        r = vk_fn.BindImageMemory(vk_device, vk_zbuffer, vk_zbuffer_memory, 0);
        if (r != VK_SUCCESS) {
            if (log_cb)
                log_cb(RETRO_LOG_ERROR,
                       "rhi-vk: BindImageMemory (zbuffer) failed (%d)\n", (int)r);
            return false;
        }

        memset(&zb_view_ci, 0, sizeof(zb_view_ci));
        zb_view_ci.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        zb_view_ci.image                           = vk_zbuffer;
        zb_view_ci.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
        zb_view_ci.format                          = VK_FORMAT_R16_UINT;
        zb_view_ci.components.r                    = VK_COMPONENT_SWIZZLE_IDENTITY;
        zb_view_ci.components.g                    = VK_COMPONENT_SWIZZLE_IDENTITY;
        zb_view_ci.components.b                    = VK_COMPONENT_SWIZZLE_IDENTITY;
        zb_view_ci.components.a                    = VK_COMPONENT_SWIZZLE_IDENTITY;
        zb_view_ci.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        zb_view_ci.subresourceRange.baseMipLevel   = 0;
        zb_view_ci.subresourceRange.levelCount     = 1;
        zb_view_ci.subresourceRange.baseArrayLayer = 0;
        zb_view_ci.subresourceRange.layerCount     = 1;

        r = vk_fn.CreateImageView(vk_device, &zb_view_ci, NULL, &vk_zbuffer_view);
        if (r != VK_SUCCESS) {
            if (log_cb)
                log_cb(RETRO_LOG_ERROR,
                       "rhi-vk: CreateImageView (zbuffer) failed (%d)\n", (int)r);
            return false;
        }

        /* 2. vk_particles_buffer (SSBO).  Host-visible +
         *    coherent so dispatch_3d_particles can memcpy
         *    GpuParticle records directly with no Flush. */
        memset(&pb_ci, 0, sizeof(pb_ci));
        pb_ci.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        pb_ci.size        = VK_PARTICLES_BYTES;
        pb_ci.usage       = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        pb_ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        r = vk_fn.CreateBuffer(vk_device, &pb_ci, NULL, &vk_particles_buffer);
        if (r != VK_SUCCESS) {
            if (log_cb)
                log_cb(RETRO_LOG_ERROR,
                       "rhi-vk: CreateBuffer (particles SSBO) failed (%d)\n", (int)r);
            return false;
        }

        vk_fn.GetBufferMemoryRequirements(vk_device,
                                          vk_particles_buffer, &pb_mem_req);
        pb_mem_type = backend_vk_find_memory_type(
            pb_mem_req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
          | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (pb_mem_type == UINT32_MAX) {
            if (log_cb)
                log_cb(RETRO_LOG_ERROR,
                       "rhi-vk: no HOST_VISIBLE|COHERENT memory for particles\n");
            return false;
        }

        memset(&pb_mem_ai, 0, sizeof(pb_mem_ai));
        pb_mem_ai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        pb_mem_ai.allocationSize  = pb_mem_req.size;
        pb_mem_ai.memoryTypeIndex = pb_mem_type;

        r = vk_fn.AllocateMemory(vk_device, &pb_mem_ai, NULL, &vk_particles_memory);
        if (r != VK_SUCCESS) {
            if (log_cb)
                log_cb(RETRO_LOG_ERROR,
                       "rhi-vk: AllocateMemory (particles) failed (%d)\n", (int)r);
            return false;
        }

        r = vk_fn.BindBufferMemory(vk_device, vk_particles_buffer,
                                   vk_particles_memory, 0);
        if (r != VK_SUCCESS) {
            if (log_cb)
                log_cb(RETRO_LOG_ERROR,
                       "rhi-vk: BindBufferMemory (particles) failed (%d)\n", (int)r);
            return false;
        }

        r = vk_fn.MapMemory(vk_device, vk_particles_memory,
                            0, VK_WHOLE_SIZE, 0, &vk_particles_ptr);
        if (r != VK_SUCCESS) {
            if (log_cb)
                log_cb(RETRO_LOG_ERROR,
                       "rhi-vk: MapMemory (particles) failed (%d)\n", (int)r);
            return false;
        }

        /* 3. vk_particles_zstaging.  Host-visible buffer
         *    sized to width*height*sizeof(uint16_t) for the
         *    per-frame d_pzbuffer upload. */
        memset(&zs_ci, 0, sizeof(zs_ci));
        zs_ci.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        zs_ci.size        = (VkDeviceSize)width * (VkDeviceSize)height * 2u;
        zs_ci.usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        zs_ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        r = vk_fn.CreateBuffer(vk_device, &zs_ci, NULL, &vk_particles_zstaging);
        if (r != VK_SUCCESS) {
            if (log_cb)
                log_cb(RETRO_LOG_ERROR,
                       "rhi-vk: CreateBuffer (zbuffer staging) failed (%d)\n", (int)r);
            return false;
        }

        vk_fn.GetBufferMemoryRequirements(vk_device,
                                          vk_particles_zstaging, &zs_mem_req);
        zs_mem_type = backend_vk_find_memory_type(
            zs_mem_req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
          | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (zs_mem_type == UINT32_MAX) {
            if (log_cb)
                log_cb(RETRO_LOG_ERROR,
                       "rhi-vk: no HOST_VISIBLE|COHERENT memory for zbuffer staging\n");
            return false;
        }

        memset(&zs_mem_ai, 0, sizeof(zs_mem_ai));
        zs_mem_ai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        zs_mem_ai.allocationSize  = zs_mem_req.size;
        zs_mem_ai.memoryTypeIndex = zs_mem_type;

        r = vk_fn.AllocateMemory(vk_device, &zs_mem_ai, NULL,
                                 &vk_particles_zstaging_memory);
        if (r != VK_SUCCESS) {
            if (log_cb)
                log_cb(RETRO_LOG_ERROR,
                       "rhi-vk: AllocateMemory (zbuffer staging) failed (%d)\n",
                       (int)r);
            return false;
        }

        r = vk_fn.BindBufferMemory(vk_device, vk_particles_zstaging,
                                   vk_particles_zstaging_memory, 0);
        if (r != VK_SUCCESS) {
            if (log_cb)
                log_cb(RETRO_LOG_ERROR,
                       "rhi-vk: BindBufferMemory (zbuffer staging) failed (%d)\n",
                       (int)r);
            return false;
        }

        r = vk_fn.MapMemory(vk_device, vk_particles_zstaging_memory,
                            0, VK_WHOLE_SIZE, 0, &vk_particles_zstaging_ptr);
        if (r != VK_SUCCESS) {
            if (log_cb)
                log_cb(RETRO_LOG_ERROR,
                       "rhi-vk: MapMemory (zbuffer staging) failed (%d)\n", (int)r);
            return false;
        }

        /* 4. Shader module from precompiled SPIR-V. */
        memset(&psm_ci, 0, sizeof(psm_ci));
        psm_ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        psm_ci.codeSize = spv_particles_cs_size;
        psm_ci.pCode    = spv_particles_cs;

        r = vk_fn.CreateShaderModule(vk_device, &psm_ci, NULL,
                                     &vk_particles_cs_module);
        if (r != VK_SUCCESS) {
            if (log_cb)
                log_cb(RETRO_LOG_ERROR,
                       "rhi-vk: CreateShaderModule (particles) failed (%d)\n",
                       (int)r);
            return false;
        }

        /* 5. Descriptor set layout: SSBO + 2 storage images,
         *    all COMPUTE-only.  Distinct from vk_descriptor_
         *    set_layout (which is for the palette compute) --
         *    different bindings, different types. */
        memset(&p_dsl_bindings, 0, sizeof(p_dsl_bindings));
        p_dsl_bindings[0].binding         = 0;
        p_dsl_bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        p_dsl_bindings[0].descriptorCount = 1;
        p_dsl_bindings[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
        p_dsl_bindings[1].binding         = 1;
        p_dsl_bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        p_dsl_bindings[1].descriptorCount = 1;
        p_dsl_bindings[1].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
        p_dsl_bindings[2].binding         = 2;
        p_dsl_bindings[2].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        p_dsl_bindings[2].descriptorCount = 1;
        p_dsl_bindings[2].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

        memset(&p_dsl_ci, 0, sizeof(p_dsl_ci));
        p_dsl_ci.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        p_dsl_ci.bindingCount = 3;
        p_dsl_ci.pBindings    = p_dsl_bindings;

        r = vk_fn.CreateDescriptorSetLayout(vk_device, &p_dsl_ci, NULL,
                                            &vk_particles_dsl);
        if (r != VK_SUCCESS) {
            if (log_cb)
                log_cb(RETRO_LOG_ERROR,
                       "rhi-vk: CreateDescriptorSetLayout (particles) failed (%d)\n",
                       (int)r);
            return false;
        }

        /* 6. Descriptor pool + set allocation. */
        memset(&p_pool_sizes, 0, sizeof(p_pool_sizes));
        p_pool_sizes[0].type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        p_pool_sizes[0].descriptorCount = 1;
        p_pool_sizes[1].type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        p_pool_sizes[1].descriptorCount = 2;

        memset(&p_pool_ci, 0, sizeof(p_pool_ci));
        p_pool_ci.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        p_pool_ci.maxSets       = 1;
        p_pool_ci.poolSizeCount = 2;
        p_pool_ci.pPoolSizes    = p_pool_sizes;

        r = vk_fn.CreateDescriptorPool(vk_device, &p_pool_ci, NULL,
                                       &vk_particles_pool);
        if (r != VK_SUCCESS) {
            if (log_cb)
                log_cb(RETRO_LOG_ERROR,
                       "rhi-vk: CreateDescriptorPool (particles) failed (%d)\n",
                       (int)r);
            return false;
        }

        memset(&p_set_alloc, 0, sizeof(p_set_alloc));
        p_set_alloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        p_set_alloc.descriptorPool     = vk_particles_pool;
        p_set_alloc.descriptorSetCount = 1;
        p_set_alloc.pSetLayouts        = &vk_particles_dsl;

        r = vk_fn.AllocateDescriptorSets(vk_device, &p_set_alloc,
                                         &vk_particles_set);
        if (r != VK_SUCCESS) {
            if (log_cb)
                log_cb(RETRO_LOG_ERROR,
                       "rhi-vk: AllocateDescriptorSets (particles) failed (%d)\n",
                       (int)r);
            return false;
        }

        /* 7. Update the set with handles.  Storage images
         *    use imageLayout = GENERAL because that's the
         *    layout they'll be in during the dispatch
         *    (record_frame transitions them there before
         *    binding this set). */
        memset(&p_buf_info, 0, sizeof(p_buf_info));
        p_buf_info.buffer = vk_particles_buffer;
        p_buf_info.offset = 0;
        p_buf_info.range  = VK_WHOLE_SIZE;

        memset(&p_img_info, 0, sizeof(p_img_info));
        p_img_info[0].sampler     = VK_NULL_HANDLE;
        p_img_info[0].imageView   = vk_texture_view;
        p_img_info[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        p_img_info[1].sampler     = VK_NULL_HANDLE;
        p_img_info[1].imageView   = vk_zbuffer_view;
        p_img_info[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        memset(&p_writes, 0, sizeof(p_writes));
        p_writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        p_writes[0].dstSet          = vk_particles_set;
        p_writes[0].dstBinding      = 0;
        p_writes[0].descriptorCount = 1;
        p_writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        p_writes[0].pBufferInfo     = &p_buf_info;
        p_writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        p_writes[1].dstSet          = vk_particles_set;
        p_writes[1].dstBinding      = 1;
        p_writes[1].descriptorCount = 1;
        p_writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        p_writes[1].pImageInfo      = &p_img_info[0];
        p_writes[2].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        p_writes[2].dstSet          = vk_particles_set;
        p_writes[2].dstBinding      = 2;
        p_writes[2].descriptorCount = 1;
        p_writes[2].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        p_writes[2].pImageInfo      = &p_img_info[1];

        vk_fn.UpdateDescriptorSets(vk_device, 3, p_writes, 0, NULL);

        /* 8. Pipeline layout: DSL + push constants. */
        memset(&p_push_range, 0, sizeof(p_push_range));
        p_push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        p_push_range.offset     = 0;
        p_push_range.size       = sizeof(struct vk_particles_pc);

        memset(&p_pl_ci, 0, sizeof(p_pl_ci));
        p_pl_ci.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        p_pl_ci.setLayoutCount         = 1;
        p_pl_ci.pSetLayouts            = &vk_particles_dsl;
        p_pl_ci.pushConstantRangeCount = 1;
        p_pl_ci.pPushConstantRanges    = &p_push_range;

        r = vk_fn.CreatePipelineLayout(vk_device, &p_pl_ci, NULL,
                                       &vk_particles_pipeline_layout);
        if (r != VK_SUCCESS) {
            if (log_cb)
                log_cb(RETRO_LOG_ERROR,
                       "rhi-vk: CreatePipelineLayout (particles) failed (%d)\n",
                       (int)r);
            return false;
        }

        /* 9. Compute pipeline. */
        memset(&p_cs_stage, 0, sizeof(p_cs_stage));
        p_cs_stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        p_cs_stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
        p_cs_stage.module = vk_particles_cs_module;
        p_cs_stage.pName  = "main";

        memset(&p_cp_ci, 0, sizeof(p_cp_ci));
        p_cp_ci.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        p_cp_ci.stage  = p_cs_stage;
        p_cp_ci.layout = vk_particles_pipeline_layout;

        r = vk_fn.CreateComputePipelines(vk_device, VK_NULL_HANDLE,
                                         1, &p_cp_ci, NULL,
                                         &vk_particles_pipeline);
        if (r != VK_SUCCESS) {
            if (log_cb)
                log_cb(RETRO_LOG_ERROR,
                       "rhi-vk: CreateComputePipelines (particles) failed (%d)\n",
                       (int)r);
            return false;
        }
    }

    /* ---------------------------------------------------------
     * Phase 5b-03: GPU compute water-warp resources.
     *
     * Order:
     *   1. vk_warp_table_buffer (UBO, DEVICE_LOCAL).
     *   2. Stage and upload intsintable into it via a one-shot
     *      host-visible buffer + CmdCopyBuffer in a one-time
     *      command submission.
     *   3. vk_warp_cs_module from precompiled SPIR-V.
     *   4. vk_warp_dsl (4 bindings: 2 sampled, 1 storage, 1 UBO).
     *   5. vk_warp_pool + AllocateDescriptorSets -> vk_warp_set.
     *   6. UpdateDescriptorSets with the four handles
     *      (vk_texture_view sampled, vk_palette_texture_view
     *      sampled, vk_image_view storage, vk_warp_table_buffer
     *      UBO).
     *   7. vk_warp_pipeline_layout (DSL + 32 B push constants).
     *   8. CreateComputePipelines -> vk_warp_pipeline.
     */
    {
        VkBufferCreateInfo            wt_ci;
        VkMemoryRequirements          wt_mem_req;
        VkMemoryAllocateInfo          wt_mem_ai;
        VkBufferCreateInfo            ws_ci;
        VkMemoryRequirements          ws_mem_req;
        VkMemoryAllocateInfo          ws_mem_ai;
        VkBuffer                      wt_staging;
        VkDeviceMemory                wt_staging_memory;
        void                         *wt_staging_ptr;
        VkCommandBuffer               wt_upload_cmd;
        VkCommandBufferAllocateInfo   wt_cmd_alloc;
        VkCommandBufferBeginInfo      wt_begin;
        VkBufferCopy                  wt_copy;
        VkSubmitInfo                  wt_si;
        VkShaderModuleCreateInfo      wsm_ci;
        VkDescriptorSetLayoutBinding  w_dsl_bindings[4];
        VkDescriptorSetLayoutCreateInfo w_dsl_ci;
        VkDescriptorPoolSize          w_pool_sizes[3];
        VkDescriptorPoolCreateInfo    w_pool_ci;
        VkDescriptorSetAllocateInfo   w_set_alloc;
        VkDescriptorImageInfo         w_img_info[3];
        VkDescriptorBufferInfo        w_buf_info;
        VkWriteDescriptorSet          w_writes[4];
        VkPushConstantRange           w_push_range;
        VkPipelineLayoutCreateInfo    w_pl_ci;
        VkComputePipelineCreateInfo   w_cp_ci;
        VkPipelineShaderStageCreateInfo w_cs_stage;
        uint32_t                      wt_mem_type;
        uint32_t                      ws_mem_type;
        int                           si;

        /* 1. vk_warp_table_buffer: UBO sized for 64 ivec4
         *    slots (4 bytes/lane * 4 lanes/slot * 64 slots
         *    = 1 KiB).  TRANSFER_DST so the one-shot
         *    upload can write to it.  DEVICE_LOCAL because
         *    it's read every frame by the warp dispatch
         *    and never modified by the host after init. */
        memset(&wt_ci, 0, sizeof(wt_ci));
        wt_ci.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        wt_ci.size        = VK_WARP_TABLE_BYTES;
        wt_ci.usage       = VK_BUFFER_USAGE_TRANSFER_DST_BIT
                          | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        wt_ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        r = vk_fn.CreateBuffer(vk_device, &wt_ci, NULL,
                               &vk_warp_table_buffer);
        if (r != VK_SUCCESS) {
            if (log_cb)
                log_cb(RETRO_LOG_ERROR,
                       "rhi-vk: CreateBuffer (warp table UBO) failed (%d)\n",
                       (int)r);
            return false;
        }

        vk_fn.GetBufferMemoryRequirements(vk_device, vk_warp_table_buffer,
                                          &wt_mem_req);
        wt_mem_type = backend_vk_find_memory_type(
            wt_mem_req.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (wt_mem_type == UINT32_MAX) {
            if (log_cb)
                log_cb(RETRO_LOG_ERROR,
                       "rhi-vk: no DEVICE_LOCAL memory for warp table UBO\n");
            return false;
        }

        memset(&wt_mem_ai, 0, sizeof(wt_mem_ai));
        wt_mem_ai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        wt_mem_ai.allocationSize  = wt_mem_req.size;
        wt_mem_ai.memoryTypeIndex = wt_mem_type;

        r = vk_fn.AllocateMemory(vk_device, &wt_mem_ai, NULL,
                                 &vk_warp_table_memory);
        if (r != VK_SUCCESS) {
            if (log_cb)
                log_cb(RETRO_LOG_ERROR,
                       "rhi-vk: AllocateMemory (warp table UBO) failed (%d)\n",
                       (int)r);
            return false;
        }

        r = vk_fn.BindBufferMemory(vk_device, vk_warp_table_buffer,
                                   vk_warp_table_memory, 0);
        if (r != VK_SUCCESS) {
            if (log_cb)
                log_cb(RETRO_LOG_ERROR,
                       "rhi-vk: BindBufferMemory (warp table UBO) failed (%d)\n",
                       (int)r);
            return false;
        }

        /* 2. Stage + upload intsintable via a one-shot
         *    HOST_VISIBLE buffer + CmdCopyBuffer.  The
         *    table is 256 ints from R_InitTurb (which has
         *    already run by this point -- R_Init is called
         *    from libretro's retro_load_game ahead of the
         *    backend's context_reset).  Pack as 64 ivec4
         *    slots to match the shader's std140 layout
         *    (each int occupies its own 16-byte slot in
         *    std140; we pack 4 ints per slot using ivec4).
         */
        memset(&ws_ci, 0, sizeof(ws_ci));
        ws_ci.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        ws_ci.size        = VK_WARP_TABLE_BYTES;
        ws_ci.usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        ws_ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        r = vk_fn.CreateBuffer(vk_device, &ws_ci, NULL, &wt_staging);
        if (r != VK_SUCCESS) {
            if (log_cb)
                log_cb(RETRO_LOG_ERROR,
                       "rhi-vk: CreateBuffer (warp table staging) failed (%d)\n",
                       (int)r);
            return false;
        }

        vk_fn.GetBufferMemoryRequirements(vk_device, wt_staging, &ws_mem_req);
        ws_mem_type = backend_vk_find_memory_type(
            ws_mem_req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
          | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (ws_mem_type == UINT32_MAX) {
            if (log_cb)
                log_cb(RETRO_LOG_ERROR,
                       "rhi-vk: no HOST_VISIBLE|COHERENT memory for warp staging\n");
            vk_fn.DestroyBuffer(vk_device, wt_staging, NULL);
            return false;
        }

        memset(&ws_mem_ai, 0, sizeof(ws_mem_ai));
        ws_mem_ai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ws_mem_ai.allocationSize  = ws_mem_req.size;
        ws_mem_ai.memoryTypeIndex = ws_mem_type;

        r = vk_fn.AllocateMemory(vk_device, &ws_mem_ai, NULL, &wt_staging_memory);
        if (r != VK_SUCCESS) {
            if (log_cb)
                log_cb(RETRO_LOG_ERROR,
                       "rhi-vk: AllocateMemory (warp table staging) failed (%d)\n",
                       (int)r);
            vk_fn.DestroyBuffer(vk_device, wt_staging, NULL);
            return false;
        }

        r = vk_fn.BindBufferMemory(vk_device, wt_staging, wt_staging_memory, 0);
        if (r != VK_SUCCESS) {
            if (log_cb)
                log_cb(RETRO_LOG_ERROR,
                       "rhi-vk: BindBufferMemory (warp table staging) failed (%d)\n",
                       (int)r);
            vk_fn.FreeMemory(vk_device, wt_staging_memory, NULL);
            vk_fn.DestroyBuffer(vk_device, wt_staging, NULL);
            return false;
        }

        r = vk_fn.MapMemory(vk_device, wt_staging_memory,
                            0, VK_WHOLE_SIZE, 0, &wt_staging_ptr);
        if (r != VK_SUCCESS) {
            if (log_cb)
                log_cb(RETRO_LOG_ERROR,
                       "rhi-vk: MapMemory (warp table staging) failed (%d)\n",
                       (int)r);
            vk_fn.FreeMemory(vk_device, wt_staging_memory, NULL);
            vk_fn.DestroyBuffer(vk_device, wt_staging, NULL);
            return false;
        }

        /* Pack intsintable (256 ints) into 64 ivec4 slots.
         * Slot i holds entries [4i .. 4i+3].  std140 layout
         * in the shader matches: ivec4 packed[64] is 64
         * 16-byte slots, each containing four int lanes. */
        {
            int32_t *slot = (int32_t *)wt_staging_ptr;
            for (si = 0; si < (int)VK_WARP_TABLE_SIZE; si++)
                slot[si] = (int32_t)intsintable[si];
        }

        /* Allocate a one-shot command buffer to issue the
         * CopyBuffer (we already have vk_cmd_pool from up
         * above? -- no, vk_cmd_pool is created below this
         * block.  We need our own short-lived pool here,
         * or we can hand-roll the command buffer with a
         * temporary pool.  Simplest: create a temporary
         * single-use pool that's destroyed immediately
         * after submit + WaitIdle).
         *
         * Actually vk_cmd_pool IS created below, after
         * this block -- which means it doesn't exist yet
         * when we get here.  Defer the upload: write the
         * staging bytes here, but issue the copy at the
         * end of create_resources after vk_cmd_pool is
         * up.  That's what the next sub-block below does. */

        /* 3. Shader module. */
        memset(&wsm_ci, 0, sizeof(wsm_ci));
        wsm_ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        wsm_ci.codeSize = spv_warpscreen_cs_size;
        wsm_ci.pCode    = spv_warpscreen_cs;

        r = vk_fn.CreateShaderModule(vk_device, &wsm_ci, NULL,
                                     &vk_warp_cs_module);
        if (r != VK_SUCCESS) {
            if (log_cb)
                log_cb(RETRO_LOG_ERROR,
                       "rhi-vk: CreateShaderModule (warp) failed (%d)\n",
                       (int)r);
            vk_fn.UnmapMemory(vk_device, wt_staging_memory);
            vk_fn.FreeMemory(vk_device, wt_staging_memory, NULL);
            vk_fn.DestroyBuffer(vk_device, wt_staging, NULL);
            return false;
        }

        /* 4. Descriptor set layout: 4 bindings, all
         *    COMPUTE-only.  Bindings 0-2 mirror the
         *    palette compute's set; binding 3 is the
         *    sin-table UBO. */
        memset(&w_dsl_bindings, 0, sizeof(w_dsl_bindings));
        w_dsl_bindings[0].binding         = 0;
        w_dsl_bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w_dsl_bindings[0].descriptorCount = 1;
        w_dsl_bindings[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
        w_dsl_bindings[1].binding         = 1;
        w_dsl_bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w_dsl_bindings[1].descriptorCount = 1;
        w_dsl_bindings[1].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
        w_dsl_bindings[2].binding         = 2;
        w_dsl_bindings[2].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w_dsl_bindings[2].descriptorCount = 1;
        w_dsl_bindings[2].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
        w_dsl_bindings[3].binding         = 3;
        w_dsl_bindings[3].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w_dsl_bindings[3].descriptorCount = 1;
        w_dsl_bindings[3].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

        memset(&w_dsl_ci, 0, sizeof(w_dsl_ci));
        w_dsl_ci.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        w_dsl_ci.bindingCount = 4;
        w_dsl_ci.pBindings    = w_dsl_bindings;

        r = vk_fn.CreateDescriptorSetLayout(vk_device, &w_dsl_ci, NULL,
                                            &vk_warp_dsl);
        if (r != VK_SUCCESS) {
            if (log_cb)
                log_cb(RETRO_LOG_ERROR,
                       "rhi-vk: CreateDescriptorSetLayout (warp) failed (%d)\n",
                       (int)r);
            vk_fn.UnmapMemory(vk_device, wt_staging_memory);
            vk_fn.FreeMemory(vk_device, wt_staging_memory, NULL);
            vk_fn.DestroyBuffer(vk_device, wt_staging, NULL);
            return false;
        }

        /* 5. Pool + set allocation. */
        memset(&w_pool_sizes, 0, sizeof(w_pool_sizes));
        w_pool_sizes[0].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w_pool_sizes[0].descriptorCount = 2;
        w_pool_sizes[1].type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w_pool_sizes[1].descriptorCount = 1;
        w_pool_sizes[2].type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w_pool_sizes[2].descriptorCount = 1;

        memset(&w_pool_ci, 0, sizeof(w_pool_ci));
        w_pool_ci.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        w_pool_ci.maxSets       = 1;
        w_pool_ci.poolSizeCount = 3;
        w_pool_ci.pPoolSizes    = w_pool_sizes;

        r = vk_fn.CreateDescriptorPool(vk_device, &w_pool_ci, NULL,
                                       &vk_warp_pool);
        if (r != VK_SUCCESS) {
            if (log_cb)
                log_cb(RETRO_LOG_ERROR,
                       "rhi-vk: CreateDescriptorPool (warp) failed (%d)\n",
                       (int)r);
            vk_fn.UnmapMemory(vk_device, wt_staging_memory);
            vk_fn.FreeMemory(vk_device, wt_staging_memory, NULL);
            vk_fn.DestroyBuffer(vk_device, wt_staging, NULL);
            return false;
        }

        memset(&w_set_alloc, 0, sizeof(w_set_alloc));
        w_set_alloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        w_set_alloc.descriptorPool     = vk_warp_pool;
        w_set_alloc.descriptorSetCount = 1;
        w_set_alloc.pSetLayouts        = &vk_warp_dsl;

        r = vk_fn.AllocateDescriptorSets(vk_device, &w_set_alloc, &vk_warp_set);
        if (r != VK_SUCCESS) {
            if (log_cb)
                log_cb(RETRO_LOG_ERROR,
                       "rhi-vk: AllocateDescriptorSets (warp) failed (%d)\n",
                       (int)r);
            vk_fn.UnmapMemory(vk_device, wt_staging_memory);
            vk_fn.FreeMemory(vk_device, wt_staging_memory, NULL);
            vk_fn.DestroyBuffer(vk_device, wt_staging, NULL);
            return false;
        }

        /* 6. Update set bindings.  vk_texture sampled via
         *    SHADER_READ_ONLY (the regular palette layout
         *    transition leaves it there; ditto vk_palette_
         *    texture).  vk_image storage via GENERAL.  The
         *    UBO uses the buffer info union member. */
        memset(&w_img_info, 0, sizeof(w_img_info));
        w_img_info[0].sampler     = vk_sampler;
        w_img_info[0].imageView   = vk_texture_view;
        w_img_info[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        w_img_info[1].sampler     = vk_sampler;
        w_img_info[1].imageView   = vk_palette_texture_view;
        w_img_info[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        w_img_info[2].sampler     = VK_NULL_HANDLE;
        w_img_info[2].imageView   = vk_image_view;
        w_img_info[2].imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        memset(&w_buf_info, 0, sizeof(w_buf_info));
        w_buf_info.buffer = vk_warp_table_buffer;
        w_buf_info.offset = 0;
        w_buf_info.range  = VK_WHOLE_SIZE;

        memset(&w_writes, 0, sizeof(w_writes));
        w_writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w_writes[0].dstSet          = vk_warp_set;
        w_writes[0].dstBinding      = 0;
        w_writes[0].descriptorCount = 1;
        w_writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w_writes[0].pImageInfo      = &w_img_info[0];
        w_writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w_writes[1].dstSet          = vk_warp_set;
        w_writes[1].dstBinding      = 1;
        w_writes[1].descriptorCount = 1;
        w_writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w_writes[1].pImageInfo      = &w_img_info[1];
        w_writes[2].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w_writes[2].dstSet          = vk_warp_set;
        w_writes[2].dstBinding      = 2;
        w_writes[2].descriptorCount = 1;
        w_writes[2].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w_writes[2].pImageInfo      = &w_img_info[2];
        w_writes[3].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w_writes[3].dstSet          = vk_warp_set;
        w_writes[3].dstBinding      = 3;
        w_writes[3].descriptorCount = 1;
        w_writes[3].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w_writes[3].pBufferInfo     = &w_buf_info;

        vk_fn.UpdateDescriptorSets(vk_device, 4, w_writes, 0, NULL);

        /* 7. Pipeline layout. */
        memset(&w_push_range, 0, sizeof(w_push_range));
        w_push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        w_push_range.offset     = 0;
        w_push_range.size       = sizeof(struct vk_warp_pc);

        memset(&w_pl_ci, 0, sizeof(w_pl_ci));
        w_pl_ci.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        w_pl_ci.setLayoutCount         = 1;
        w_pl_ci.pSetLayouts            = &vk_warp_dsl;
        w_pl_ci.pushConstantRangeCount = 1;
        w_pl_ci.pPushConstantRanges    = &w_push_range;

        r = vk_fn.CreatePipelineLayout(vk_device, &w_pl_ci, NULL,
                                       &vk_warp_pipeline_layout);
        if (r != VK_SUCCESS) {
            if (log_cb)
                log_cb(RETRO_LOG_ERROR,
                       "rhi-vk: CreatePipelineLayout (warp) failed (%d)\n",
                       (int)r);
            vk_fn.UnmapMemory(vk_device, wt_staging_memory);
            vk_fn.FreeMemory(vk_device, wt_staging_memory, NULL);
            vk_fn.DestroyBuffer(vk_device, wt_staging, NULL);
            return false;
        }

        /* 8. Compute pipeline. */
        memset(&w_cs_stage, 0, sizeof(w_cs_stage));
        w_cs_stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        w_cs_stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
        w_cs_stage.module = vk_warp_cs_module;
        w_cs_stage.pName  = "main";

        memset(&w_cp_ci, 0, sizeof(w_cp_ci));
        w_cp_ci.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        w_cp_ci.stage  = w_cs_stage;
        w_cp_ci.layout = vk_warp_pipeline_layout;

        r = vk_fn.CreateComputePipelines(vk_device, VK_NULL_HANDLE,
                                         1, &w_cp_ci, NULL,
                                         &vk_warp_pipeline);
        if (r != VK_SUCCESS) {
            if (log_cb)
                log_cb(RETRO_LOG_ERROR,
                       "rhi-vk: CreateComputePipelines (warp) failed (%d)\n",
                       (int)r);
            vk_fn.UnmapMemory(vk_device, wt_staging_memory);
            vk_fn.FreeMemory(vk_device, wt_staging_memory, NULL);
            vk_fn.DestroyBuffer(vk_device, wt_staging, NULL);
            return false;
        }

        /* Stash staging handles in file-static scope so
         * the post-cmd-pool block below can issue the
         * one-shot copy + tear them down.  Use simple
         * leak-style locals: we'll free them at the end
         * of create_resources after the upload is done. */
        (void)wt_upload_cmd;
        (void)wt_cmd_alloc;
        (void)wt_begin;
        (void)wt_copy;
        (void)wt_si;

        /* Move the staging handles to a temporary holding
         * spot via file-static globals -- this is a
         * one-shot upload, so don't bother with a full
         * struct; cast through static dummies that the
         * post-cmd-pool sub-block reads.  Keeping the
         * upload deferred avoids needing a second cmd
         * pool here. */
        vk_warp_table_staging         = wt_staging;
        vk_warp_table_staging_memory  = wt_staging_memory;
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

    /* Phase 5b-03: one-shot upload of intsintable into
     * vk_warp_table_buffer (DEVICE_LOCAL).  Uses
     * vk_cmd_buffer (just allocated above) for a single-
     * use CopyBuffer + QueueWaitIdle, then frees the
     * staging buffer.  The table doesn't change after
     * R_InitTurb so this is a once-per-context-reset
     * cost. */
    if (vk_warp_table_staging != VK_NULL_HANDLE) {
        VkCommandBufferBeginInfo wt_begin;
        VkBufferCopy             wt_copy;
        VkSubmitInfo             wt_si;
        VkBufferMemoryBarrier    wt_bar;

        memset(&wt_begin, 0, sizeof(wt_begin));
        wt_begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        wt_begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

        r = vk_fn.BeginCommandBuffer(vk_cmd_buffer, &wt_begin);
        if (r != VK_SUCCESS) {
            if (log_cb)
                log_cb(RETRO_LOG_ERROR,
                       "rhi-vk: warp-table upload BeginCommandBuffer failed (%d)\n",
                       (int)r);
            return false;
        }

        memset(&wt_copy, 0, sizeof(wt_copy));
        wt_copy.size = VK_WARP_TABLE_BYTES;

        vk_fn.CmdCopyBuffer(vk_cmd_buffer,
                            vk_warp_table_staging,
                            vk_warp_table_buffer,
                            1, &wt_copy);

        /* TRANSFER_WRITE -> UNIFORM_READ for the UBO so
         * the first frame's warp dispatch sees the data
         * coherently.  Buffer barrier so the table-
         * specific dependency is clear; alternative would
         * be a global memory barrier. */
        memset(&wt_bar, 0, sizeof(wt_bar));
        wt_bar.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        wt_bar.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
        wt_bar.dstAccessMask       = VK_ACCESS_UNIFORM_READ_BIT;
        wt_bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        wt_bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        wt_bar.buffer              = vk_warp_table_buffer;
        wt_bar.offset              = 0;
        wt_bar.size                = VK_WARP_TABLE_BYTES;

        vk_fn.CmdPipelineBarrier(vk_cmd_buffer,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 0,
                                 0, NULL,
                                 1, &wt_bar,
                                 0, NULL);

        r = vk_fn.EndCommandBuffer(vk_cmd_buffer);
        if (r != VK_SUCCESS) {
            if (log_cb)
                log_cb(RETRO_LOG_ERROR,
                       "rhi-vk: warp-table upload EndCommandBuffer failed (%d)\n",
                       (int)r);
            return false;
        }

        memset(&wt_si, 0, sizeof(wt_si));
        wt_si.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        wt_si.commandBufferCount = 1;
        wt_si.pCommandBuffers    = &vk_cmd_buffer;

        r = vk_fn.QueueSubmit(vk_queue, 1, &wt_si, VK_NULL_HANDLE);
        if (r != VK_SUCCESS) {
            if (log_cb)
                log_cb(RETRO_LOG_ERROR,
                       "rhi-vk: warp-table upload QueueSubmit failed (%d)\n",
                       (int)r);
            return false;
        }

        vk_fn.QueueWaitIdle(vk_queue);

        /* Staging buffer's job is done; free it.  Mapped
         * memory is implicitly unmapped by FreeMemory. */
        vk_fn.UnmapMemory(vk_device, vk_warp_table_staging_memory);
        vk_fn.DestroyBuffer(vk_device, vk_warp_table_staging, NULL);
        vk_fn.FreeMemory(vk_device, vk_warp_table_staging_memory, NULL);
        vk_warp_table_staging         = VK_NULL_HANDLE;
        vk_warp_table_staging_memory  = VK_NULL_HANDLE;
    }

    vk_resources_ready = true;
    if (log_cb)
        log_cb(RETRO_LOG_INFO,
               "rhi-vk: render target %ux%u + R8 index + 256x1 palette + "
               "staging %llu bytes ready; compute palette LUT + Draw_Pic intercept active\n",
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

    /* Phase 5b-03 warp pipeline / resources teardown.
     * Reverse-create order; pool destruction implicitly
     * frees vk_warp_set.  The staging buffer / memory
     * pair, if non-NULL, is leftover state from a failed
     * create_resources -- destroy them too. */
    if (vk_warp_pipeline != VK_NULL_HANDLE) {
        if (vk_fn.DestroyPipeline)
            vk_fn.DestroyPipeline(vk_device, vk_warp_pipeline, NULL);
        vk_warp_pipeline = VK_NULL_HANDLE;
    }
    if (vk_warp_pipeline_layout != VK_NULL_HANDLE) {
        if (vk_fn.DestroyPipelineLayout)
            vk_fn.DestroyPipelineLayout(vk_device,
                                        vk_warp_pipeline_layout, NULL);
        vk_warp_pipeline_layout = VK_NULL_HANDLE;
    }
    if (vk_warp_pool != VK_NULL_HANDLE) {
        if (vk_fn.DestroyDescriptorPool)
            vk_fn.DestroyDescriptorPool(vk_device, vk_warp_pool, NULL);
        vk_warp_pool = VK_NULL_HANDLE;
        vk_warp_set  = VK_NULL_HANDLE;
    }
    if (vk_warp_dsl != VK_NULL_HANDLE) {
        if (vk_fn.DestroyDescriptorSetLayout)
            vk_fn.DestroyDescriptorSetLayout(vk_device, vk_warp_dsl, NULL);
        vk_warp_dsl = VK_NULL_HANDLE;
    }
    if (vk_warp_cs_module != VK_NULL_HANDLE) {
        if (vk_fn.DestroyShaderModule)
            vk_fn.DestroyShaderModule(vk_device, vk_warp_cs_module, NULL);
        vk_warp_cs_module = VK_NULL_HANDLE;
    }
    if (vk_warp_table_staging != VK_NULL_HANDLE) {
        if (vk_fn.DestroyBuffer)
            vk_fn.DestroyBuffer(vk_device, vk_warp_table_staging, NULL);
        vk_warp_table_staging = VK_NULL_HANDLE;
    }
    if (vk_warp_table_staging_memory != VK_NULL_HANDLE) {
        if (vk_fn.FreeMemory)
            vk_fn.FreeMemory(vk_device, vk_warp_table_staging_memory, NULL);
        vk_warp_table_staging_memory = VK_NULL_HANDLE;
    }
    if (vk_warp_table_buffer != VK_NULL_HANDLE) {
        if (vk_fn.DestroyBuffer)
            vk_fn.DestroyBuffer(vk_device, vk_warp_table_buffer, NULL);
        vk_warp_table_buffer = VK_NULL_HANDLE;
    }
    if (vk_warp_table_memory != VK_NULL_HANDLE) {
        if (vk_fn.FreeMemory)
            vk_fn.FreeMemory(vk_device, vk_warp_table_memory, NULL);
        vk_warp_table_memory = VK_NULL_HANDLE;
    }
    vk_warp_active = false;

    /* Phase 5b-02 particle pipeline / resources teardown.
     * Reverse-create order; pool destruction implicitly
     * frees vk_particles_set.  Mapped memory is implicitly
     * unmapped by FreeMemory. */
    if (vk_particles_pipeline != VK_NULL_HANDLE) {
        if (vk_fn.DestroyPipeline)
            vk_fn.DestroyPipeline(vk_device, vk_particles_pipeline, NULL);
        vk_particles_pipeline = VK_NULL_HANDLE;
    }
    if (vk_particles_pipeline_layout != VK_NULL_HANDLE) {
        if (vk_fn.DestroyPipelineLayout)
            vk_fn.DestroyPipelineLayout(vk_device,
                                        vk_particles_pipeline_layout, NULL);
        vk_particles_pipeline_layout = VK_NULL_HANDLE;
    }
    if (vk_particles_pool != VK_NULL_HANDLE) {
        if (vk_fn.DestroyDescriptorPool)
            vk_fn.DestroyDescriptorPool(vk_device, vk_particles_pool, NULL);
        vk_particles_pool = VK_NULL_HANDLE;
        vk_particles_set  = VK_NULL_HANDLE;
    }
    if (vk_particles_dsl != VK_NULL_HANDLE) {
        if (vk_fn.DestroyDescriptorSetLayout)
            vk_fn.DestroyDescriptorSetLayout(vk_device,
                                             vk_particles_dsl, NULL);
        vk_particles_dsl = VK_NULL_HANDLE;
    }
    if (vk_particles_cs_module != VK_NULL_HANDLE) {
        if (vk_fn.DestroyShaderModule)
            vk_fn.DestroyShaderModule(vk_device,
                                      vk_particles_cs_module, NULL);
        vk_particles_cs_module = VK_NULL_HANDLE;
    }
    if (vk_particles_zstaging != VK_NULL_HANDLE) {
        if (vk_fn.DestroyBuffer)
            vk_fn.DestroyBuffer(vk_device, vk_particles_zstaging, NULL);
        vk_particles_zstaging = VK_NULL_HANDLE;
    }
    if (vk_particles_zstaging_memory != VK_NULL_HANDLE) {
        if (vk_fn.FreeMemory)
            vk_fn.FreeMemory(vk_device, vk_particles_zstaging_memory, NULL);
        vk_particles_zstaging_memory = VK_NULL_HANDLE;
        vk_particles_zstaging_ptr    = NULL;
    }
    if (vk_particles_buffer != VK_NULL_HANDLE) {
        if (vk_fn.DestroyBuffer)
            vk_fn.DestroyBuffer(vk_device, vk_particles_buffer, NULL);
        vk_particles_buffer = VK_NULL_HANDLE;
    }
    if (vk_particles_memory != VK_NULL_HANDLE) {
        if (vk_fn.FreeMemory)
            vk_fn.FreeMemory(vk_device, vk_particles_memory, NULL);
        vk_particles_memory = VK_NULL_HANDLE;
        vk_particles_ptr    = NULL;
    }
    if (vk_zbuffer_view != VK_NULL_HANDLE) {
        if (vk_fn.DestroyImageView)
            vk_fn.DestroyImageView(vk_device, vk_zbuffer_view, NULL);
        vk_zbuffer_view = VK_NULL_HANDLE;
    }
    if (vk_zbuffer != VK_NULL_HANDLE) {
        if (vk_fn.DestroyImage)
            vk_fn.DestroyImage(vk_device, vk_zbuffer, NULL);
        vk_zbuffer = VK_NULL_HANDLE;
    }
    if (vk_zbuffer_memory != VK_NULL_HANDLE) {
        if (vk_fn.FreeMemory)
            vk_fn.FreeMemory(vk_device, vk_zbuffer_memory, NULL);
        vk_zbuffer_memory = VK_NULL_HANDLE;
    }
    vk_pending_particle_count = 0;

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
    /* Phase 4h overlay objects: reverse-create order
     * within this group, and this whole group goes before
     * the Phase 4g compute objects.  The overlay pipeline
     * references the overlay layout (which has no
     * descriptors so isn't tied to the compute DSL), the
     * overlay render pass, and the overlay shader
     * modules; the framebuffer references the overlay
     * render pass + vk_image_view; the vertex buffer
     * stands alone. */
    if (vk_overlay_vertex_buffer != VK_NULL_HANDLE) {
        if (vk_fn.DestroyBuffer)
            vk_fn.DestroyBuffer(vk_device, vk_overlay_vertex_buffer, NULL);
        vk_overlay_vertex_buffer = VK_NULL_HANDLE;
    }
    if (vk_overlay_vertex_memory != VK_NULL_HANDLE) {
        if (vk_fn.FreeMemory)
            vk_fn.FreeMemory(vk_device, vk_overlay_vertex_memory, NULL);
        vk_overlay_vertex_memory = VK_NULL_HANDLE;
    }
    if (vk_overlay_pipeline != VK_NULL_HANDLE) {
        if (vk_fn.DestroyPipeline)
            vk_fn.DestroyPipeline(vk_device, vk_overlay_pipeline, NULL);
        vk_overlay_pipeline = VK_NULL_HANDLE;
    }
    /* No vk_overlay_pipeline_layout to destroy at Phase 4i+;
     * the overlay pipeline shares vk_pipeline_layout with
     * the compute pipeline, which is torn down below. */
    {
        /* Phase 4j: loop the slot array tearing down every
         * populated entry.  An unused slot has image ==
         * VK_NULL_HANDLE (BSS zero-init guarantees this on
         * first create_resources; we re-zero after each
         * destroy so a second create_resources won't see
         * stale handles).  descriptor_set is freed
         * implicitly when vk_descriptor_pool is destroyed
         * below. */
        unsigned si;
        for (si = 0; si < OVERLAY_SLOT_MAX; si++) {
            struct overlay_slot *slot = &vk_overlay_slots[si];
            if (slot->view != VK_NULL_HANDLE) {
                if (vk_fn.DestroyImageView)
                    vk_fn.DestroyImageView(vk_device, slot->view, NULL);
            }
            if (slot->image != VK_NULL_HANDLE) {
                if (vk_fn.DestroyImage)
                    vk_fn.DestroyImage(vk_device, slot->image, NULL);
            }
            if (slot->memory != VK_NULL_HANDLE) {
                if (vk_fn.FreeMemory)
                    vk_fn.FreeMemory(vk_device, slot->memory, NULL);
            }
            slot->view           = VK_NULL_HANDLE;
            slot->image          = VK_NULL_HANDLE;
            slot->memory         = VK_NULL_HANDLE;
            slot->descriptor_set = VK_NULL_HANDLE;
            slot->width          = 0;
            slot->height         = 0;
            slot->key            = NULL;
        }
        vk_overlay_draw_count    = 0;
        vk_overlay_upload_offset = 0;
    }
    if (vk_overlay_framebuffer != VK_NULL_HANDLE) {
        if (vk_fn.DestroyFramebuffer)
            vk_fn.DestroyFramebuffer(vk_device, vk_overlay_framebuffer, NULL);
        vk_overlay_framebuffer = VK_NULL_HANDLE;
    }
    if (vk_overlay_render_pass != VK_NULL_HANDLE) {
        if (vk_fn.DestroyRenderPass)
            vk_fn.DestroyRenderPass(vk_device, vk_overlay_render_pass, NULL);
        vk_overlay_render_pass = VK_NULL_HANDLE;
    }
    if (vk_overlay_fs_module != VK_NULL_HANDLE) {
        if (vk_fn.DestroyShaderModule)
            vk_fn.DestroyShaderModule(vk_device, vk_overlay_fs_module, NULL);
        vk_overlay_fs_module = VK_NULL_HANDLE;
    }
    if (vk_overlay_vs_module != VK_NULL_HANDLE) {
        if (vk_fn.DestroyShaderModule)
            vk_fn.DestroyShaderModule(vk_device, vk_overlay_vs_module, NULL);
        vk_overlay_vs_module = VK_NULL_HANDLE;
    }
    /* Phase 4g pipeline objects: reverse-create order.  The
     * compute pipeline references vk_pipeline_layout, which
     * references the DSL; the shader module can go any time
     * after pipeline destruction. */
    if (vk_compute_pipeline != VK_NULL_HANDLE) {
        if (vk_fn.DestroyPipeline)
            vk_fn.DestroyPipeline(vk_device, vk_compute_pipeline, NULL);
        vk_compute_pipeline = VK_NULL_HANDLE;
    }
    if (vk_pipeline_layout != VK_NULL_HANDLE) {
        if (vk_fn.DestroyPipelineLayout)
            vk_fn.DestroyPipelineLayout(vk_device, vk_pipeline_layout, NULL);
        vk_pipeline_layout = VK_NULL_HANDLE;
    }
    if (vk_cs_module != VK_NULL_HANDLE) {
        if (vk_fn.DestroyShaderModule)
            vk_fn.DestroyShaderModule(vk_device, vk_cs_module, NULL);
        vk_cs_module = VK_NULL_HANDLE;
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
        /* Phase 4b for shader/pipeline-layout, Phase 4g
         * for the compute pipeline, Phase 4h for the
         * overlay graphics pipeline + render pass +
         * framebuffer + vertex-buffer bind. */
        LOAD_DEV(CreateShaderModule);
        LOAD_DEV(DestroyShaderModule);
        LOAD_DEV(CreatePipelineLayout);
        LOAD_DEV(DestroyPipelineLayout);
        LOAD_DEV(CreateComputePipelines);
        LOAD_DEV(CreateGraphicsPipelines);
        LOAD_DEV(DestroyPipeline);
        LOAD_DEV(CreateRenderPass);
        LOAD_DEV(DestroyRenderPass);
        LOAD_DEV(CreateFramebuffer);
        LOAD_DEV(DestroyFramebuffer);
        LOAD_DEV(CmdBindPipeline);
        LOAD_DEV(CmdDispatch);
        LOAD_DEV(CmdBeginRenderPass);
        LOAD_DEV(CmdEndRenderPass);
        LOAD_DEV(CmdDraw);
        LOAD_DEV(CmdBindVertexBuffers);
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
        LOAD_DEV(CmdPushConstants);
        LOAD_DEV(CmdCopyBuffer);
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

/*
 * Per-frame: walk vk_overlay_draws and write 4 vertices
 * per entry into vk_overlay_vertex_buffer.  Phase 4j used
 * to fill this once at create_resources; Phase 4k moves
 * the fill here so the draw list can change per frame
 * (which it will, now that backend_vk_queue_2d_pic
 * populates it from Draw_Pic intercepts during retro_run).
 *
 * Buffer is HOST_VISIBLE + HOST_COHERENT and persistently
 * mapped (vk_overlay_vertex_ptr), so the write is just
 * memory; no flush required.  Strip winding is the same
 * as in Phase 4i: v0 bottom-left, v1 bottom-right, v2
 * top-left, v3 top-right, four floats per vertex
 * (pos.x pos.y u v).
 */
static void
backend_vk_fill_overlay_vb(void)
{
    float   *dst = (float *)vk_overlay_vertex_ptr;
    unsigned di;

    for (di = 0; di < vk_overlay_draw_count; di++) {
        const struct overlay_draw *d = &vk_overlay_draws[di];
        /* bottom-left  */
        dst[ 0] = d->x0; dst[ 1] = d->y1; dst[ 2] = d->u0; dst[ 3] = d->v1;
        /* bottom-right */
        dst[ 4] = d->x1; dst[ 5] = d->y1; dst[ 6] = d->u1; dst[ 7] = d->v1;
        /* top-left     */
        dst[ 8] = d->x0; dst[ 9] = d->y0; dst[10] = d->u0; dst[11] = d->v0;
        /* top-right    */
        dst[12] = d->x1; dst[13] = d->y0; dst[14] = d->u1; dst[15] = d->v0;
        dst += 16;
    }
}

/*
 * backend_vk_queue_2d_pic -- the Phase 4k Draw_Pic
 * intercept body.
 *
 * Wired up to render_backend_t::queue_2d_pic and called
 * from common/draw.c::Draw_Pic before the SW
 * memcpy-into-vid.buffer path runs.  Caches the pic in
 * vk_overlay_slots (linear-scan lookup by qpic_t pointer)
 * and appends an entry to vk_overlay_draws for
 * record_frame to consume at end-of-frame.  Uploads on
 * cache miss are synchronous (begin_uploads /
 * upload_pic_slot / end_uploads -- the helper trio
 * QueueWaitIdles); a level load's worth of new pics will
 * cost one slow frame, then steady state hits the cache.
 *
 * Silently no-ops if resources aren't ready (the
 * intercept can fire before create_resources finishes if
 * a draw happens during init), if pic is NULL, or if the
 * cache / draw list is full (no eviction yet -- Phase 4l
 * adds invalidation hooks if it turns out to be needed).
 *
 * Coordinate conversion: Quake screen space goes
 * (0..vid.width, 0..vid.height) with y = 0 at the top of
 * the framebuffer.  Vulkan NDC goes (-1..+1) with y = -1
 * at the top of the framebuffer (because the compute
 * output samples vid.buffer with v = 0 at top and writes
 * to render-target y = 0 at top, then the swapchain
 * presents y = 0 as the top of the window).  So the
 * conversion is a plain rescale:
 *
 *   ndc = 2 * screen / extent - 1
 *
 * with no flips on either axis.
 */
static void
backend_vk_queue_2d_pic(int x, int y, const qpic_t *pic)
{
    unsigned             slot_idx;
    unsigned             si;
    struct overlay_draw *draw;
    int                  pw, ph;

    if (!vk_resources_ready || !pic)
        return;

    pw = pic->width;
    ph = pic->height;
    if (pw <= 0 || ph <= 0)
        return;

    /* Cache lookup: linear scan over populated slots.
     * The qpic_t pointer is the key.  Slots with
     * key == NULL are empty.  OVERLAY_SLOT_MAX is small
     * enough (128) that the scan is microseconds even
     * called many times per frame. */
    slot_idx = OVERLAY_SLOT_MAX;
    for (si = 0; si < OVERLAY_SLOT_MAX; si++) {
        if (vk_overlay_slots[si].key == (const void *)pic) {
            slot_idx = si;
            break;
        }
    }

    if (slot_idx == OVERLAY_SLOT_MAX) {
        /* Cache miss: find the first empty slot and
         * upload pic->data there.  Empty == key NULL
         * AND image VK_NULL_HANDLE (defensive double-
         * check: a freshly-created slot has both, a
         * torn-down slot has both, and we never zero
         * one without the other). */
        for (si = 0; si < OVERLAY_SLOT_MAX; si++) {
            if (vk_overlay_slots[si].key == NULL &&
                vk_overlay_slots[si].image == VK_NULL_HANDLE) {
                slot_idx = si;
                break;
            }
        }
        if (slot_idx == OVERLAY_SLOT_MAX) {
            /* Cache full.  Drop the draw silently --
             * Phase 4k has no eviction policy; if Lib
             * hits this in practice, Phase 4l adds one
             * (LRU is the obvious choice).  The SW
             * path still runs in Draw_Pic, so the user
             * sees the pic via the compute upload --
             * just not the crisp Vulkan-overlay copy. */
            return;
        }

        if (!backend_vk_begin_uploads())
            return;
        if (!backend_vk_upload_pic_slot(slot_idx,
                                        (unsigned)pw, (unsigned)ph,
                                        pic->data)) {
            /* upload_pic_slot may have partially
             * populated the slot; clear the key so
             * the next lookup treats it as empty
             * (the partially-created image / memory
             * / view get torn down at
             * destroy_resources time via the slot
             * loop, gated on each handle being
             * non-null). */
            vk_overlay_slots[slot_idx].key = NULL;
            return;
        }
        if (!backend_vk_end_uploads()) {
            vk_overlay_slots[slot_idx].key = NULL;
            return;
        }
        vk_overlay_slots[slot_idx].key = (const void *)pic;
    }

    /* Append a draw entry.  Drop silently if the draw
     * list is full -- Phase 4k caps at OVERLAY_DRAW_MAX
     * (256), which is enough for the HUD + a typical
     * menu; busier scenes (multi-line console with
     * dozens of characters) will need a larger cap or
     * Phase 4l's character-glyph batching. */
    if (vk_overlay_draw_count >= OVERLAY_DRAW_MAX)
        return;

    draw           = &vk_overlay_draws[vk_overlay_draw_count++];
    draw->slot_idx = slot_idx;
    draw->x0       = (float)(2.0  * (double)x        / (double)width  - 1.0);
    draw->y0       = (float)(2.0  * (double)y        / (double)height - 1.0);
    draw->x1       = (float)(2.0  * (double)(x + pw) / (double)width  - 1.0);
    draw->y1       = (float)(2.0  * (double)(y + ph) / (double)height - 1.0);
    /* Pic draws sample the whole slot. */
    draw->u0       = 0.0f;
    draw->v0       = 0.0f;
    draw->u1       = 1.0f;
    draw->v1       = 1.0f;
}

/*
 * backend_vk_queue_2d_pic_scaled -- Phase 4q scale > 1
 * Draw_PicScaled / Draw_TransPicScaled intercept body.
 *
 * Identical cache / upload / queue-append logic as
 * backend_vk_queue_2d_pic above; the only difference is
 * that the on-screen rect uses (pw * scale) x (ph *
 * scale) so the scaled pic fills the right physical
 * pixels.  Sampling stays at full UV (0,0)-(1,1) -- the
 * overlay sampler's GPU-side filtering produces the
 * stretched result the SW `for (sy) for (sx) replicate
 * byte` loop produces in CPU.  (GL_NEAREST sampling
 * matches the SW byte-replicate exactly; if the slot
 * sampler ends up filtering linearly the edges will be
 * softer than the SW stretch, which the user would
 * notice on HUD digit-pic boundaries.  The existing
 * slot creation in backend_vk_upload_pic_slot uses
 * VK_FILTER_NEAREST -- see the sampler creation there
 * -- so we're aligned.)
 *
 * For Draw_TransPicScaled the overlay FS discards on
 * byte 255 (== TRANSPARENT_COLOR), which produces the
 * `if (b != TRANSPARENT_COLOR) ...` skip the SW path
 * performs per-pixel.  Same discard handles
 * Draw_PicScaled correctly too because opaque pics
 * (the gfx lmp set used at scale > 1: sb_sbar,
 * sb_ibar, qplaque, ttl_main, mainmenu, the face
 * pics) don't contain byte 255 in their palette
 * indices.
 */
static void
backend_vk_queue_2d_pic_scaled(int x, int y,
                               const qpic_t *pic, int scale)
{
    unsigned             slot_idx;
    unsigned             si;
    struct overlay_draw *draw;
    int                  pw, ph;
    int                  dw, dh;

    if (!vk_resources_ready || !pic)
        return;
    if (scale < 1)
        scale = 1;

    pw = pic->width;
    ph = pic->height;
    if (pw <= 0 || ph <= 0)
        return;

    dw = pw * scale;
    dh = ph * scale;

    /* Cache lookup keyed by pic pointer.  Identical to
     * queue_2d_pic: scale doesn't affect the cached
     * pixels, only the on-screen rect, so two calls
     * with different scales for the same pic share the
     * same slot. */
    slot_idx = OVERLAY_SLOT_MAX;
    for (si = 0; si < OVERLAY_SLOT_MAX; si++) {
        if (vk_overlay_slots[si].key == (const void *)pic) {
            slot_idx = si;
            break;
        }
    }

    if (slot_idx == OVERLAY_SLOT_MAX) {
        for (si = 0; si < OVERLAY_SLOT_MAX; si++) {
            if (vk_overlay_slots[si].key == NULL &&
                vk_overlay_slots[si].image == VK_NULL_HANDLE) {
                slot_idx = si;
                break;
            }
        }
        if (slot_idx == OVERLAY_SLOT_MAX)
            return;

        if (!backend_vk_begin_uploads())
            return;
        if (!backend_vk_upload_pic_slot(slot_idx,
                                        (unsigned)pw, (unsigned)ph,
                                        pic->data)) {
            vk_overlay_slots[slot_idx].key = NULL;
            return;
        }
        if (!backend_vk_end_uploads()) {
            vk_overlay_slots[slot_idx].key = NULL;
            return;
        }
        vk_overlay_slots[slot_idx].key = (const void *)pic;
    }

    if (vk_overlay_draw_count >= OVERLAY_DRAW_MAX)
        return;

    draw           = &vk_overlay_draws[vk_overlay_draw_count++];
    draw->slot_idx = slot_idx;
    draw->x0       = (float)(2.0  * (double)x        / (double)width  - 1.0);
    draw->y0       = (float)(2.0  * (double)y        / (double)height - 1.0);
    draw->x1       = (float)(2.0  * (double)(x + dw) / (double)width  - 1.0);
    draw->y1       = (float)(2.0  * (double)(y + dh) / (double)height - 1.0);
    draw->u0       = 0.0f;
    draw->v0       = 0.0f;
    draw->u1       = 1.0f;
    draw->v1       = 1.0f;
}

/*
 * backend_vk_queue_2d_pic_translate_scaled -- Phase 4s
 * Draw_TransPicTranslateScaled intercept body.
 *
 * Quake's multiplayer Setup menu has the user pick a
 * shirt and pants colour and previews the result by
 * rendering the gfx/menuplyr.lmp player sprite with a
 * 256-byte translation table applied per pixel (the
 * table re-maps the TOP_RANGE and BOTTOM_RANGE palette
 * windows to the user's chosen colours; everything
 * else passes through unchanged).  M_BuildTranslation
 * Table in menu.c builds the table from the current
 * setup_top / setup_bottom values; M_DrawTransPic
 * Translate calls Draw_TransPicTranslateScaled with
 * the table and pic each frame.
 *
 * Because the pic data and the translation are both
 * inputs, the cached slot pixels can't be keyed by
 * qpic_t pointer the way queue_2d_pic does -- two
 * frames in a row with different setup_top values
 * need different pixels in the slot.  This function
 * dedicates one slot to the translated pic via a
 * sentinel key (distinct from any qpic_t pointer and
 * from the conchars &draw_chars sentinel), allocates
 * it on the first call, and refreshes its pixels
 * in place on every subsequent call via
 * backend_vk_refresh_pic_slot.  Re-creating the slot
 * each frame would leak descriptor sets -- the pool
 * doesn't have VK_DESCRIPTOR_POOL_CREATE_FREE_
 * DESCRIPTOR_SET_BIT.
 *
 * Translation is done in CPU: walk pic->data, apply
 * translation[b] per byte, into a stack scratch buffer
 * sized to MAX_TRANSLATE_BYTES (large enough for any
 * plausible single qpic_t; menuplyr.lmp is ~64x80 =
 * 5120 bytes, the cap is much higher).  Byte 255 stays
 * 255 (M_BuildTranslationTable initialises the table
 * with identityTable, then only overwrites TOP_RANGE
 * and BOTTOM_RANGE -- both far from 255), so the
 * overlay FS discard-on-255 path handles transparency
 * the same way it does for Draw_TransPicScaled.
 */
#define MAX_TRANSLATE_BYTES 65536

static void
backend_vk_queue_2d_pic_translate_scaled(int x, int y,
                                         const qpic_t *pic,
                                         const byte *translation,
                                         int scale)
{
    /* Sentinel object: address is unique to this
     * function, used as the slot cache key.  Distinct
     * from any qpic_t pointer and from &draw_chars. */
    static const char    translate_slot_marker;
    const void          *key = &translate_slot_marker;

    unsigned             slot_idx;
    unsigned             si;
    struct overlay_draw *draw;
    int                  pw, ph, dw, dh;
    int                  i, n;
    uint8_t              translated[MAX_TRANSLATE_BYTES];

    if (!vk_resources_ready || !pic || !translation)
        return;
    if (scale < 1)
        scale = 1;

    pw = pic->width;
    ph = pic->height;
    if (pw <= 0 || ph <= 0)
        return;

    n = pw * ph;
    if (n > MAX_TRANSLATE_BYTES) {
        /* Defensive: never expected to fire (menuplyr is
         * tiny).  Caller's SW fallback would have been
         * dropped anyway; if a future caller pushes a
         * huge pic through here, expand the cap. */
        if (log_cb)
            log_cb(RETRO_LOG_WARN,
                   "rhi-vk: queue_2d_pic_translate_scaled: "
                   "pic %dx%d (%d bytes) exceeds scratch cap (%d)\n",
                   pw, ph, n, MAX_TRANSLATE_BYTES);
        return;
    }

    dw = pw * scale;
    dh = ph * scale;

    /* Translate */
    for (i = 0; i < n; i++)
        translated[i] = translation[pic->data[i]];

    /* Slot lookup by sentinel key. */
    slot_idx = OVERLAY_SLOT_MAX;
    for (si = 0; si < OVERLAY_SLOT_MAX; si++) {
        if (vk_overlay_slots[si].key == key) {
            slot_idx = si;
            break;
        }
    }

    if (slot_idx == OVERLAY_SLOT_MAX) {
        /* First call: allocate slot + upload via the
         * standard path. */
        for (si = 0; si < OVERLAY_SLOT_MAX; si++) {
            if (vk_overlay_slots[si].key == NULL &&
                vk_overlay_slots[si].image == VK_NULL_HANDLE) {
                slot_idx = si;
                break;
            }
        }
        if (slot_idx == OVERLAY_SLOT_MAX)
            return;

        if (!backend_vk_begin_uploads())
            return;
        if (!backend_vk_upload_pic_slot(slot_idx,
                                        (unsigned)pw, (unsigned)ph,
                                        translated)) {
            vk_overlay_slots[slot_idx].key = NULL;
            return;
        }
        if (!backend_vk_end_uploads()) {
            vk_overlay_slots[slot_idx].key = NULL;
            return;
        }
        vk_overlay_slots[slot_idx].key = key;
    } else {
        /* Subsequent call: refresh pixels in place.
         * Slot dimensions are guaranteed to match pic
         * dimensions because menuplyr.lmp is the only
         * caller and never changes size between calls. */
        if (!backend_vk_begin_uploads())
            return;
        if (!backend_vk_refresh_pic_slot(slot_idx, translated))
            return;
        if (!backend_vk_end_uploads())
            return;
    }

    if (vk_overlay_draw_count >= OVERLAY_DRAW_MAX)
        return;

    draw           = &vk_overlay_draws[vk_overlay_draw_count++];
    draw->slot_idx = slot_idx;
    draw->x0       = (float)(2.0  * (double)x        / (double)width  - 1.0);
    draw->y0       = (float)(2.0  * (double)y        / (double)height - 1.0);
    draw->x1       = (float)(2.0  * (double)(x + dw) / (double)width  - 1.0);
    draw->y1       = (float)(2.0  * (double)(y + dh) / (double)height - 1.0);
    draw->u0       = 0.0f;
    draw->v0       = 0.0f;
    draw->u1       = 1.0f;
    draw->v1       = 1.0f;
}

/*
 * backend_vk_queue_2d_fill -- Phase 4t Draw_Fill
 * intercept body.
 *
 * Caches one 1x1 R8 slot per distinct palette colour
 * seen.  Slot's single byte is the palette index `c`;
 * the existing overlay FS samples that index out of
 * the slot, looks it up in the shared palette
 * texture, and writes the resulting RGBA to the
 * swapchain.  That reproduces the SW path's
 * `dest[u] = c` byte-for-byte.
 *
 * Key is the address of color_slot_markers[c]; each
 * of the 256 array bytes has a distinct address so
 * the per-colour cache key is unique without
 * colliding with any qpic_t pointer or other sentinel
 * (e.g. &draw_chars for the conchars atlas).
 *
 * Defensive note: a fill with c == 255 would land in
 * a slot whose single byte is 255, which the overlay
 * FS would discard -- the rect would render fully
 * transparent.  Quake never calls Draw_Fill with c ==
 * 255 (255 is the palette transparency marker the
 * pic-blit paths use, never a fill colour), so this
 * is academic, but worth being aware of.
 */
static void
backend_vk_queue_2d_fill(int x, int y, int w, int h, int c)
{
    /* 256-byte sentinel array: each byte's address
     * uniquely identifies one palette colour. */
    static const char    color_slot_markers[256];
    const void          *key;
    uint8_t              cb;
    unsigned             slot_idx;
    unsigned             si;
    struct overlay_draw *draw;

    if (!vk_resources_ready || w <= 0 || h <= 0)
        return;
    if (c < 0 || c > 255)
        return;

    cb  = (uint8_t)c;
    key = (const void *)&color_slot_markers[cb];

    slot_idx = OVERLAY_SLOT_MAX;
    for (si = 0; si < OVERLAY_SLOT_MAX; si++) {
        if (vk_overlay_slots[si].key == key) {
            slot_idx = si;
            break;
        }
    }

    if (slot_idx == OVERLAY_SLOT_MAX) {
        for (si = 0; si < OVERLAY_SLOT_MAX; si++) {
            if (vk_overlay_slots[si].key == NULL &&
                vk_overlay_slots[si].image == VK_NULL_HANDLE) {
                slot_idx = si;
                break;
            }
        }
        if (slot_idx == OVERLAY_SLOT_MAX)
            return;

        if (!backend_vk_begin_uploads())
            return;
        if (!backend_vk_upload_pic_slot(slot_idx, 1, 1, &cb)) {
            vk_overlay_slots[slot_idx].key = NULL;
            return;
        }
        if (!backend_vk_end_uploads()) {
            vk_overlay_slots[slot_idx].key = NULL;
            return;
        }
        vk_overlay_slots[slot_idx].key = key;
    }

    if (vk_overlay_draw_count >= OVERLAY_DRAW_MAX)
        return;

    draw           = &vk_overlay_draws[vk_overlay_draw_count++];
    draw->slot_idx = slot_idx;
    draw->x0       = (float)(2.0  * (double)x       / (double)width  - 1.0);
    draw->y0       = (float)(2.0  * (double)y       / (double)height - 1.0);
    draw->x1       = (float)(2.0  * (double)(x + w) / (double)width  - 1.0);
    draw->y1       = (float)(2.0  * (double)(y + h) / (double)height - 1.0);
    draw->u0       = 0.0f;
    draw->v0       = 0.0f;
    draw->u1       = 1.0f;
    draw->v1       = 1.0f;
}

/*
 * backend_vk_queue_2d_fade_screen -- Phase 4t
 * Draw_FadeScreen intercept body.
 *
 * The SW path writes byte 0 (palette index 0 == black)
 * to 3 of every 4 pixels in a 4x2 checkerboard pattern,
 * leaving the 4th pixel (the world / HUD underneath)
 * unchanged.  The preserved pixels are at
 * (x, y) where (x & 3) == ((y & 1) << 1):
 *
 *   y=0 (t=0): x%4 == 0 preserved  ->  X . . . X . . .
 *   y=1 (t=2): x%4 == 2 preserved  ->  . . X . . . X .
 *
 * Implementation: stage a single full-screen mask slot
 * (vid.width x vid.height bytes) where:
 *
 *   mask[y][x] = ((x & 3) == ((y & 1) << 1)) ? 255 : 0
 *
 * 255 -> overlay FS discards -> compute-pass world /
 *        Sbar pic backgrounds underneath show through.
 * 0   -> palette[0] == black -> opaque black pixel.
 *
 * The full-screen mask is 1920*1080 = ~2 MiB at Lib's
 * setup, which fits within vk_staging_size
 * (vid.width * vid.height + VK_PALETTE_BYTES).  Mask
 * generation runs once, on the first Draw_FadeScreen
 * call, and the slot survives for the run lifetime.
 *
 * A REPEAT-sampler 4x2 tile would be 2 MiB smaller but
 * needs a separate sampler -- not worth the complexity
 * for a one-shot upload.
 */
static void
backend_vk_queue_2d_fade_screen(void)
{
    static const char    fade_screen_marker;
    const void          *key = &fade_screen_marker;
    unsigned             slot_idx;
    unsigned             si;
    struct overlay_draw *draw;

    if (!vk_resources_ready || width == 0 || height == 0)
        return;

    slot_idx = OVERLAY_SLOT_MAX;
    for (si = 0; si < OVERLAY_SLOT_MAX; si++) {
        if (vk_overlay_slots[si].key == key) {
            slot_idx = si;
            break;
        }
    }

    if (slot_idx == OVERLAY_SLOT_MAX) {
        uint8_t  *mask;
        unsigned  x, y;
        size_t    n;

        for (si = 0; si < OVERLAY_SLOT_MAX; si++) {
            if (vk_overlay_slots[si].key == NULL &&
                vk_overlay_slots[si].image == VK_NULL_HANDLE) {
                slot_idx = si;
                break;
            }
        }
        if (slot_idx == OVERLAY_SLOT_MAX)
            return;

        n    = (size_t)width * (size_t)height;
        mask = (uint8_t *)malloc(n);
        if (!mask)
            return;

        for (y = 0; y < height; y++) {
            unsigned t = (y & 1u) << 1u;
            uint8_t *row = mask + (size_t)y * (size_t)width;
            for (x = 0; x < width; x++)
                row[x] = ((x & 3u) == t) ? (uint8_t)255 : (uint8_t)0;
        }

        if (!backend_vk_begin_uploads()) {
            free(mask);
            return;
        }
        if (!backend_vk_upload_pic_slot(slot_idx, width, height, mask)) {
            free(mask);
            vk_overlay_slots[slot_idx].key = NULL;
            return;
        }
        if (!backend_vk_end_uploads()) {
            free(mask);
            vk_overlay_slots[slot_idx].key = NULL;
            return;
        }
        free(mask);
        vk_overlay_slots[slot_idx].key = key;
    }

    if (vk_overlay_draw_count >= OVERLAY_DRAW_MAX)
        return;

    draw           = &vk_overlay_draws[vk_overlay_draw_count++];
    draw->slot_idx = slot_idx;
    draw->x0       = -1.0f;
    draw->y0       = -1.0f;
    draw->x1       =  1.0f;
    draw->y1       =  1.0f;
    draw->u0       = 0.0f;
    draw->v0       = 0.0f;
    draw->u1       = 1.0f;
    draw->v1       = 1.0f;
}

/*
 * backend_vk_queue_2d_char -- Phase 4o Draw_Character /
 * Draw_String intercept body.
 *
 * The conchars atlas is one 128x128 image (16x16 cells of
 * 8x8 chars) that backs every visible character in
 * console / menu / scoreboard / centerstring / finale
 * text.  We cache it in exactly one overlay slot keyed by
 * the address of the global draw_chars pointer (stable
 * for the process lifetime, distinct from any qpic_t
 * pointer so it never collides with pic-slot keys).
 *
 * Transparency handling
 * =====================
 * The conchars atlas uses palette byte 0 as its
 * transparency marker (Draw_Character only writes
 * source[i] when source[i] != 0).  That conflicts with
 * the pic / TransPic intercept, which uses byte 255
 * (TRANSPARENT_COLOR) as transparency and whose
 * overlay_quad.frag discards on idx > 254.5 / 255.
 *
 * Rather than introduce a second pipeline / shader or a
 * push-constant, we remap byte 0 to byte 255 during the
 * one-time upload of draw_chars into the slot.  After
 * remap, the existing discard catches the formerly-zero
 * pixels.  This is safe iff conchars does not contain
 * byte 255 as a legitimate (non-transparent) palette
 * index -- if it did, those pixels would be incorrectly
 * discarded.  We scan during upload and log a warning if
 * we observe any byte-255 in the source data; if Lib
 * ever sees that warning fire in practice, the proper
 * fix is a push-constant transparency key or a second
 * pipeline.  In the standard Quake conchars the count is
 * zero.
 */
static void
backend_vk_queue_2d_char(int x, int y, int num, int scale)
{
    /* Sentinel key: address of the global draw_chars
     * pointer.  Stable for process lifetime; distinct
     * from every qpic_t pointer (it lives in .data /
     * .bss rather than the hunk allocator that hands
     * out qpic_t storage). */
    static const void *const conchars_key =
        (const void *)&draw_chars;

    unsigned             slot_idx;
    unsigned             si;
    struct overlay_draw *draw;
    int                  row, col;
    int                  w, h;

    if (!vk_resources_ready || !draw_chars)
        return;
    if (scale < 1)
        scale = 1;

    num &= 255;

    /* Cache lookup by the conchars sentinel. */
    slot_idx = OVERLAY_SLOT_MAX;
    for (si = 0; si < OVERLAY_SLOT_MAX; si++) {
        if (vk_overlay_slots[si].key == conchars_key) {
            slot_idx = si;
            break;
        }
    }

    if (slot_idx == OVERLAY_SLOT_MAX) {
        /* First call: find a free slot, remap byte 0
         * -> byte 255 into a scratch buffer, upload. */
        uint8_t *remapped;
        size_t   i;
        size_t   b255_count = 0;
        qboolean ok;

        for (si = 0; si < OVERLAY_SLOT_MAX; si++) {
            if (vk_overlay_slots[si].key == NULL &&
                vk_overlay_slots[si].image == VK_NULL_HANDLE) {
                slot_idx = si;
                break;
            }
        }
        if (slot_idx == OVERLAY_SLOT_MAX)
            return;

        remapped = (uint8_t *)malloc(128u * 128u);
        if (!remapped)
            return;
        for (i = 0; i < 128u * 128u; i++) {
            byte b = draw_chars[i];
            if (b == 0)
                remapped[i] = 255;
            else
                remapped[i] = b;
            if (b == 255)
                b255_count++;
        }
        if (b255_count && log_cb)
            log_cb(RETRO_LOG_WARN,
                   "rhi-vk: conchars contains %u byte-255 pixels; "
                   "those will be incorrectly discarded as transparent. "
                   "Expect visual artefacts in console / menu text. "
                   "(If you see this, push-constant transparency is the "
                   "right next step.)\n",
                   (unsigned)b255_count);

        ok = backend_vk_begin_uploads();
        if (ok)
            ok = backend_vk_upload_pic_slot(slot_idx,
                                            128u, 128u, remapped);
        if (ok)
            ok = backend_vk_end_uploads();
        free(remapped);
        if (!ok) {
            /* Partial upload may have created some
             * resources; clear key so the slot is
             * treated as empty next time and the
             * destroy_resources loop tears down what's
             * there. */
            vk_overlay_slots[slot_idx].key = NULL;
            return;
        }
        vk_overlay_slots[slot_idx].key = conchars_key;
    }

    if (vk_overlay_draw_count >= OVERLAY_DRAW_MAX)
        return;

    w   = 8 * scale;
    h   = 8 * scale;
    row = num >> 4;
    col = num & 15;

    draw           = &vk_overlay_draws[vk_overlay_draw_count++];
    draw->slot_idx = slot_idx;
    draw->x0       = (float)(2.0  * (double)x       / (double)width  - 1.0);
    draw->y0       = (float)(2.0  * (double)y       / (double)height - 1.0);
    draw->x1       = (float)(2.0  * (double)(x + w) / (double)width  - 1.0);
    draw->y1       = (float)(2.0  * (double)(y + h) / (double)height - 1.0);
    /* Sub-UV: pick one 8x8 cell out of the 16x16 atlas. */
    draw->u0       = (float)col         / 16.0f;
    draw->v0       = (float)row         / 16.0f;
    draw->u1       = (float)(col + 1)   / 16.0f;
    draw->v1       = (float)(row + 1)   / 16.0f;
}

/*
 * backend_vk_queue_2d_console_background -- Phase 4r
 * (re-attempt of 67c8f47 / Phase 4p) Draw_Console
 * Background intercept body.
 *
 * The original Phase 4p attempt landed and was reverted
 * (02c3181) because at that time M_DrawPic / M_Draw
 * TransPic at scale > 1 still SW-wrote menu pics into
 * vid.buffer; queuing the conback as an overlay quad
 * then covered the menu pics in vid.buffer instead of
 * sitting underneath them in queue order.  Phase 4q
 * (8a9268d) routes scale > 1 pics through the overlay
 * queue, which fixes the ordering: M_Draw now queues
 * conback first then the menu pics, and the menu pics
 * correctly draw on top of the conback as overlay
 * entries.  The implementation here is otherwise
 * unchanged from 67c8f47.
 *
 * The SW Draw_ConsoleBackground stretches the gfx/conback
 * .lmp pic vertically to fit (vid.width, lines), sampling
 * the bottom `lines / vid.height` fraction of the source.
 * The relevant SW math:
 *
 *   for (y = 0; y < lines; y++)
 *     v = (vid.conheight - lines + y) * conback->height
 *         / vid.conheight;
 *
 * so as y sweeps [0, lines) the source v sweeps
 * [(vid.conheight - lines) * conback->height / vid.conheight,
 *  ~conback->height).  Normalised to [0, 1] UV that's
 *
 *   v0 = (vid.height - lines) / vid.height
 *   v1 = 1.0
 *
 * which is what we set on the overlay quad below.  At
 * lines == vid.height (full-screen console, the
 * con_forcedup case in screen.c::SCR_SetUpToDrawConsole)
 * v0 collapses to 0 and the whole pic is sampled.
 *
 * Caching is keyed by the pic pointer (Draw_CachePic
 * returns a stable hunk pointer; same convention queue_2d
 * _pic uses for HUD / menu pics).  Caller already ran
 * Draw_ConbackString before the intercept, so the cached
 * upload captures the TYR_VERSION text baked into the
 * pic.  TYR_VERSION is constant per run, so the cached
 * version stays accurate across all subsequent frames.
 */
static void
backend_vk_queue_2d_console_background(int lines, const qpic_t *pic)
{
    unsigned             slot_idx;
    unsigned             si;
    struct overlay_draw *draw;
    int                  pw, ph;
    float                v0;

    if (!vk_resources_ready || !pic || lines <= 0)
        return;

    pw = pic->width;
    ph = pic->height;
    if (pw <= 0 || ph <= 0)
        return;

    /* Cache lookup -- same linear scan keyed by pic
     * pointer that queue_2d_pic uses. */
    slot_idx = OVERLAY_SLOT_MAX;
    for (si = 0; si < OVERLAY_SLOT_MAX; si++) {
        if (vk_overlay_slots[si].key == (const void *)pic) {
            slot_idx = si;
            break;
        }
    }

    if (slot_idx == OVERLAY_SLOT_MAX) {
        for (si = 0; si < OVERLAY_SLOT_MAX; si++) {
            if (vk_overlay_slots[si].key == NULL &&
                vk_overlay_slots[si].image == VK_NULL_HANDLE) {
                slot_idx = si;
                break;
            }
        }
        if (slot_idx == OVERLAY_SLOT_MAX)
            return;

        if (!backend_vk_begin_uploads())
            return;
        if (!backend_vk_upload_pic_slot(slot_idx,
                                        (unsigned)pw, (unsigned)ph,
                                        pic->data)) {
            vk_overlay_slots[slot_idx].key = NULL;
            return;
        }
        if (!backend_vk_end_uploads()) {
            vk_overlay_slots[slot_idx].key = NULL;
            return;
        }
        vk_overlay_slots[slot_idx].key = (const void *)pic;
    }

    if (vk_overlay_draw_count >= OVERLAY_DRAW_MAX)
        return;

    if (lines > (int)height)
        lines = (int)height;
    v0 = (float)((int)height - lines) / (float)height;

    draw           = &vk_overlay_draws[vk_overlay_draw_count++];
    draw->slot_idx = slot_idx;
    draw->x0       = -1.0f;
    draw->y0       = -1.0f;
    draw->x1       =  1.0f;
    draw->y1       = (float)(2.0 * (double)lines / (double)height - 1.0);
    draw->u0       = 0.0f;
    draw->v0       = v0;
    draw->u1       = 1.0f;
    draw->v1       = 1.0f;
}
#endif /* RHI_HAVE_VULKAN */

/*
 * Vtable entry point for queue_2d_pic.  Lives outside the
 * RHI_HAVE_VULKAN block so the symbol is always defined
 * (the vtable references it unconditionally; the body
 * is a no-op in the SW-only build).
 */
static void
backend_vk_queue_2d_pic_entry(int x, int y, const qpic_t *pic)
{
#ifdef RHI_HAVE_VULKAN
    backend_vk_queue_2d_pic(x, y, pic);
#else
    (void)x; (void)y; (void)pic;
#endif
}

/*
 * Vtable entry point for queue_2d_char.  Same #ifdef
 * pattern as the pic entry above: always defined so the
 * vtable links, body no-ops in SW-only builds.
 */
static void
backend_vk_queue_2d_char_entry(int x, int y, int num, int scale)
{
#ifdef RHI_HAVE_VULKAN
    backend_vk_queue_2d_char(x, y, num, scale);
#else
    (void)x; (void)y; (void)num; (void)scale;
#endif
}

/*
 * Vtable entry point for queue_2d_pic_scaled.  Same
 * #ifdef pattern.
 */
static void
backend_vk_queue_2d_pic_scaled_entry(int x, int y,
                                     const qpic_t *pic, int scale)
{
#ifdef RHI_HAVE_VULKAN
    backend_vk_queue_2d_pic_scaled(x, y, pic, scale);
#else
    (void)x; (void)y; (void)pic; (void)scale;
#endif
}

/*
 * Vtable entry point for queue_2d_console_background.
 * Same #ifdef pattern as the entries above.
 */
static void
backend_vk_queue_2d_console_background_entry(int lines, const qpic_t *pic)
{
#ifdef RHI_HAVE_VULKAN
    backend_vk_queue_2d_console_background(lines, pic);
#else
    (void)lines; (void)pic;
#endif
}

/*
 * Vtable entry point for queue_2d_pic_translate_scaled.
 * Same #ifdef pattern.
 */
static void
backend_vk_queue_2d_pic_translate_scaled_entry(int x, int y,
                                               const qpic_t *pic,
                                               const byte *translation,
                                               int scale)
{
#ifdef RHI_HAVE_VULKAN
    backend_vk_queue_2d_pic_translate_scaled(x, y, pic, translation, scale);
#else
    (void)x; (void)y; (void)pic; (void)translation; (void)scale;
#endif
}

/*
 * Vtable entry point for queue_2d_fill.  Same #ifdef
 * pattern.
 */
static void
backend_vk_queue_2d_fill_entry(int x, int y, int w, int h, int c)
{
#ifdef RHI_HAVE_VULKAN
    backend_vk_queue_2d_fill(x, y, w, h, c);
#else
    (void)x; (void)y; (void)w; (void)h; (void)c;
#endif
}

/*
 * Vtable entry point for queue_2d_fade_screen.  Same
 * #ifdef pattern.
 */
static void
backend_vk_queue_2d_fade_screen_entry(void)
{
#ifdef RHI_HAVE_VULKAN
    backend_vk_queue_2d_fade_screen();
#endif
}

/*
 * Phase 5b-02: stage the active particle list for the
 * frame's GPU dispatch.
 *
 * Vtable entry called by R_DrawParticles when
 * g_rhi_compute_rendering is true and this entry is non-
 * NULL.  Walks the linked list, builds a flat array of
 * struct vk_gpu_particle records in the host-mapped
 * vk_particles_buffer, snapshots the SW raster state into
 * vk_particles_push, and sets vk_pending_particle_count.
 * record_frame later sees the non-zero count and emits
 * the dispatch + barrier sequence.
 *
 * Caps at VK_PARTICLES_MAX records.  Overflow particles
 * (beyond the cap) are silently dropped -- preferable to
 * a runtime allocation or a hard failure mode.  Quake's
 * default MAX_PARTICLES is 2048; the cap of 8192 leaves
 * 4x headroom even in pathological cases.
 *
 * The push-constants snapshot has to happen here (not in
 * record_frame) because record_frame runs in end_frame
 * after retro_run's input handling, possibly after the
 * world / camera have moved on -- the CPU SW raster
 * state at dispatch-stage time is what the shader's
 * arithmetic must match, so we capture it at the moment
 * R_DrawParticles called us.
 *
 * Defined at file scope outside the big #ifdef
 * RHI_HAVE_VULKAN body block (same #ifdef pattern as the
 * queue_2d_*_entry functions above): the vtable
 * references this name unconditionally so it must
 * resolve in the SW-only build too.  The body is a no-op
 * in that build.
 */
static void
backend_vk_dispatch_3d_particles_impl(struct particle_s *head)
{
#ifdef RHI_HAVE_VULKAN
    struct vk_gpu_particle *dst;
    struct particle_s      *p;
    uint32_t                count;

    if (!vk_resources_ready || !vk_particles_ptr)
        return;

    dst   = (struct vk_gpu_particle *)vk_particles_ptr;
    count = 0;
    for (p = head; p && count < VK_PARTICLES_MAX; p = p->next) {
        dst[count].origin[0] = p->org[0];
        dst[count].origin[1] = p->org[1];
        dst[count].origin[2] = p->org[2];
        /* particle_t::color is a float for legacy reasons --
         * the SW path casts to byte at draw time
         * (pdest[0] = pparticle->color).  Cast here to
         * preserve the same truncation semantics. */
        dst[count].color = (uint32_t)(uint8_t)p->color;
        count++;
    }

    if (count == 0)
        return;

    /* Snapshot push constants.  These globals can change
     * between now (mid-R_RenderView on the CPU) and the
     * command-buffer record in end_frame, so we capture
     * them by value. */
    memset(&vk_particles_push, 0, sizeof(vk_particles_push));
    vk_particles_push.r_origin[0] = r_origin[0];
    vk_particles_push.r_origin[1] = r_origin[1];
    vk_particles_push.r_origin[2] = r_origin[2];
    vk_particles_push.xcenter     = xcenter;
    vk_particles_push.r_pright[0] = r_pright[0];
    vk_particles_push.r_pright[1] = r_pright[1];
    vk_particles_push.r_pright[2] = r_pright[2];
    vk_particles_push.ycenter     = ycenter;
    vk_particles_push.r_pup[0]    = r_pup[0];
    vk_particles_push.r_pup[1]    = r_pup[1];
    vk_particles_push.r_pup[2]    = r_pup[2];
    vk_particles_push.d_y_aspect_shift = d_y_aspect_shift;
    vk_particles_push.r_ppn[0]    = r_ppn[0];
    vk_particles_push.r_ppn[1]    = r_ppn[1];
    vk_particles_push.r_ppn[2]    = r_ppn[2];
    vk_particles_push.d_pix_shift = d_pix_shift;
    vk_particles_push.d_vrectx               = d_vrectx;
    vk_particles_push.d_vrecty               = d_vrecty;
    vk_particles_push.d_vrectright_particle  = d_vrectright_particle;
    vk_particles_push.d_vrectbottom_particle = d_vrectbottom_particle;
    vk_particles_push.d_pix_min              = d_pix_min;
    vk_particles_push.d_pix_max              = d_pix_max;
    vk_particles_push.particle_count         = count;

    vk_pending_particle_count = count;
#else
    (void)head;
#endif
}

/*
 * Phase 5b-03: stage a water-warp dispatch for the
 * frame.
 *
 * Vtable entry called by R_RenderView at the point it
 * would have called D_WarpScreen (replacing the CPU
 * warp).  Snapshots the current warp phase ((cl.time *
 * TURB_SPEED) & 127) and the scr_vrect bounds into
 * vk_warp_push, sets vk_warp_active true.  record_frame
 * sees the flag and dispatches the fused warp+palette
 * compute shader instead of the regular palette compute.
 *
 * No GPU work happens here -- this just records "we want
 * a warp this frame" -- the actual compute dispatch
 * appears later in the per-frame command buffer.
 *
 * Defined outside the big #ifdef RHI_HAVE_VULKAN body
 * block because the vtable references this name
 * unconditionally; body is a no-op in the SW-only build.
 */
static void
backend_vk_dispatch_3d_warp_screen_impl(void)
{
#ifdef RHI_HAVE_VULKAN
    int phase;

    if (!vk_resources_ready)
        return;

    /* TURB_SPEED == 20, TURB_CYCLE == 128.  phase wraps
     * at TURB_CYCLE; turb[i] index ranges 0..127, which
     * combined with the +i offset (also 0..127) keeps
     * the intsintable[] access in [0, 255]. */
    phase = (int)(cl.time * 20.0) & 127;

    vk_warp_push.phase = phase;
    vk_warp_push.scr_x = scr_vrect.x;
    vk_warp_push.scr_y = scr_vrect.y;
    vk_warp_push.scr_w = scr_vrect.width;
    vk_warp_push.scr_h = scr_vrect.height;

    vk_warp_active = true;
#endif
}

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
    /* Phase 5 scaffolding: branch on the user's compute-
     * vs-graphics preference.  Both branches currently
     * fall through to the existing CPU SW rasterizer
     * (R_RenderView fills vid.buffer; the per-frame
     * compute palette-upload path in backend_vk_end_frame
     * turns that into the swapchain image).  Subsequent
     * commits replace each branch with its real
     * implementation:
     *
     *   compute branch  (Phase 5b+):
     *     GPU compute dispatches that port the SW
     *     rasterizer line-for-line (per-span affine
     *     texturing, surface cache, alias edge stepping
     *     -- the d_*.c / r_*.c hot loops translated into
     *     GLSL).  Writes directly into the same
     *     vid.buffer GPU image the compute palette path
     *     reads, so end_frame's downstream stages don't
     *     need to know which branch produced the pixels.
     *
     *   graphics branch (Phase 5a):
     *     Vulkan graphics pipelines for world surfaces,
     *     alias / brush / sprite models, sky, liquids,
     *     particles, shadows.  Renders to a colour +
     *     depth attachment pair sized to the user's
     *     `tyrquake_resolution`; the compute palette
     *     stage either reads from a different source or
     *     becomes a no-op when this branch owns the
     *     output (tbd, depending on how the colour
     *     attachment's format ends up plumbed). */
    if (g_rhi_compute_rendering) {
        /* Phase 5b: compute rasterizer.  Placeholder --
         * still runs the CPU rasterizer until the GLSL
         * port lands. */
        R_RenderView();
    } else {
        /* Phase 5a: graphics pipelines.  Placeholder --
         * still runs the CPU rasterizer until the
         * pipelines land. */
        R_RenderView();
    }
#endif
}

#ifdef RHI_HAVE_VULKAN
/*
 * Per-frame upload (Phase 4f).  Copies the raw 8bpp vid.buffer
 * into the index region of the staging buffer (bytes
 * [0, width*height)) and d_8to24table_shifted into the palette
 * region (bytes [width*height, width*height + VK_PALETTE_BYTES)).
 * No per-pixel arithmetic on the host -- the FS does the
 * index-into-palette lookup at sample time.
 *
 * d_8to24table_shifted (not d_8to24table) is the source for
 * the palette upload: it's the table that VID_SetPalette
 * keeps in sync with d_8to16table, so it tracks damage
 * flashes, bonus flashes, underwater shifts, and quad-damage
 * tinting just like the SW present path does.  d_8to24table
 * is the base palette (set once at startup, never updated),
 * needed by the SW renderer's alias/surface RGB-light math
 * but not by us.
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
    memcpy(pal_dst, d_8to24table_shifted, VK_PALETTE_BYTES);
}

/*
 * Phase 5b-02: stage d_pzbuffer into the host-visible
 * vk_particles_zstaging buffer so the per-frame command
 * recording can CopyBufferToImage it into vk_zbuffer.
 *
 * Called from backend_vk_end_frame only when
 * vk_pending_particle_count > 0 (i.e. there's a particle
 * dispatch waiting that needs the Z values to test
 * against).  At end_frame time, R_RenderView has finished
 * and d_pzbuffer holds the world / alias / brush Z
 * values the CPU SW raster wrote -- exactly what the GPU
 * particles need.
 *
 * The SW raster's d_pzbuffer is `short *` (signed 16-bit),
 * but the GPU treats vk_zbuffer as R16_UINT.  The bit
 * patterns match for non-negative shorts; izi values that
 * arise during normal Quake gameplay never overflow into
 * the negative range, so the unsigned interpretation is
 * lossless for every realistic camera position.  (A
 * particle extremely close to camera could in principle
 * push izi past 32767 and wrap the SW comparison, but
 * that's a pre-existing SW edge case; the GPU path
 * mirrors the same arithmetic.)
 *
 * vid.rowbytes is bytes per row; for the SW Z-buffer it's
 * d_zwidth * sizeof(short).  d_zwidth equals vid.width
 * (D_ViewChanged sets it), so the staging copy is a
 * single contiguous memcpy of width*height*2 bytes.
 */
static void
backend_vk_upload_zbuffer(void)
{
    if (!vk_particles_zstaging_ptr || !d_pzbuffer)
        return;

    memcpy(vk_particles_zstaging_ptr,
           d_pzbuffer,
           (size_t)width * (size_t)height * sizeof(short));
}

/*
 * Record the per-frame command buffer.  Called by end_frame
 * after the host-side staging upload.  Sequence (Phase 4g):
 *
 *   Begin (ONE_TIME_SUBMIT)
 *     Barrier UNDEFINED -> TRANSFER_DST_OPTIMAL on
 *       vk_texture + vk_palette_texture (batched).
 *       UNDEFINED-as-discard handles both the first frame
 *       (texture genuinely UNDEFINED out of create_resources)
 *       and subsequent frames (texture in
 *       SHADER_READ_ONLY_OPTIMAL from the previous frame's
 *       record).  Wrapping the previous content as garbage
 *       is correct: we overwrite the entire texture in the
 *       copy that follows.
 *     CmdCopyBufferToImage staging -> vk_texture.
 *     CmdCopyBufferToImage staging -> vk_palette_texture.
 *     Barrier TRANSFER_DST -> SHADER_READ_ONLY_OPTIMAL on
 *       both textures (batched) with dstStage =
 *       COMPUTE_SHADER (Phase 4f had FRAGMENT_SHADER here;
 *       the consumer is now the compute dispatch, not a
 *       graphics pipeline's FS).
 *     Barrier UNDEFINED -> GENERAL on vk_image with dstStage
 *       = COMPUTE_SHADER so the storage-image write the
 *       compute shader does is ordered after the layout
 *       transition.  Same UNDEFINED-as-discard pattern as
 *       the textures -- previous frame's content is going
 *       to be entirely overwritten.
 *     CmdBindPipeline (COMPUTE) + CmdBindDescriptorSets
 *       (COMPUTE) + CmdDispatch.  Dispatch dimensions are
 *       ceil(width / 16) x ceil(height / 16) x 1; the in-
 *       shader bounds check catches the right/bottom edges
 *       when width or height isn't a multiple of 16.
 *     CmdBeginRenderPass (overlay) + CmdBindPipeline
 *       (GRAPHICS, overlay) + CmdBindVertexBuffers (binding
 *       0 = vk_overlay_vertex_buffer) + CmdDraw(4, 1, 0, 0)
 *       + CmdEndRenderPass.  The render pass's
 *       EXTERNAL -> 0 subpass dependency carries the
 *       compute-write -> color-attachment-access memory
 *       dependency that Phase 4g used to issue as an
 *       explicit pipeline barrier; the attachment's
 *       initialLayout = GENERAL takes the image straight
 *       from the dispatch, finalLayout =
 *       SHADER_READ_ONLY_OPTIMAL hands it to the frontend.
 *       Phase 4h replaces the explicit compute-exit barrier
 *       Phase 4g used to issue.
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
    VkImageMemoryBarrier      barriers[3];
    VkImageMemoryBarrier      image_barrier;
    VkBufferImageCopy         regions[2];
    VkBufferImageCopy         zb_region;
    VkRenderPassBeginInfo     rpbi;
    VkDeviceSize              vb_offset;
    uint32_t                  group_count_x;
    uint32_t                  group_count_y;
    qboolean                  particles_active;
    VkResult                  r;

    /* Phase 5b-02: latch the dispatch count for this
     * record.  The implementation reads vk_pending_
     * particle_count multiple times below; pinning it
     * to a local avoids any chance of mid-record
     * mutation if a future change adds a path that
     * touches the global. */
    particles_active = vk_pending_particle_count > 0;

    memset(&begin_info, 0, sizeof(begin_info));
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    r = vk_fn.BeginCommandBuffer(vk_cmd_buffer, &begin_info);
    if (r != VK_SUCCESS) {
        if (log_cb)
            log_cb(RETRO_LOG_ERROR, "rhi-vk: BeginCommandBuffer failed (%d)\n", (int)r);
        return false;
    }

    /* Textures + (when particles active) zbuffer:
     * UNDEFINED -> TRANSFER_DST_OPTIMAL in a single
     * CmdPipelineBarrier.  UNDEFINED-as-discard is the
     * right old layout for the textures (whole content
     * overwritten by the copies below) and for the
     * zbuffer (whole content overwritten by the zbuffer
     * staging copy). */
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
    barriers[2]       = barriers[0];
    barriers[2].image = vk_zbuffer;

    vk_fn.CmdPipelineBarrier(vk_cmd_buffer,
                             VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0,
                             0, NULL,
                             0, NULL,
                             particles_active ? 3 : 2, barriers);

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

    /* Phase 5b-02: when particles are active, also copy
     * d_pzbuffer (staged earlier in end_frame) into the
     * GPU vk_zbuffer.  Same TRANSFER_DST_OPTIMAL layout
     * the barrier above prepared. */
    if (particles_active) {
        memset(&zb_region, 0, sizeof(zb_region));
        zb_region.bufferOffset                    = 0;
        zb_region.bufferRowLength                 = 0;
        zb_region.bufferImageHeight               = 0;
        zb_region.imageSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        zb_region.imageSubresource.mipLevel       = 0;
        zb_region.imageSubresource.baseArrayLayer = 0;
        zb_region.imageSubresource.layerCount     = 1;
        zb_region.imageExtent.width               = width;
        zb_region.imageExtent.height              = height;
        zb_region.imageExtent.depth               = 1;

        vk_fn.CmdCopyBufferToImage(vk_cmd_buffer,
                                   vk_particles_zstaging,
                                   vk_zbuffer,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   1, &zb_region);
    }

    if (particles_active) {
        /* Phase 5b-02 particle dispatch path.
         *
         * Post-copy layout transitions:
         *   vk_texture        : TRANSFER_DST -> GENERAL
         *                       (compute shader will
         *                       imageStore into it)
         *   vk_palette_texture: TRANSFER_DST -> SHADER_
         *                       READ_ONLY (read by the
         *                       downstream palette
         *                       compute dispatch
         *                       unchanged)
         *   vk_zbuffer        : TRANSFER_DST -> GENERAL
         *                       (compute shader will
         *                       imageLoad + imageStore
         *                       for Z-test)
         *
         * All three batched in one CmdPipelineBarrier
         * with dstStage = COMPUTE_SHADER (the next
         * consumer for all three is the particles
         * dispatch). */
        barriers[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT
                                  | VK_ACCESS_SHADER_WRITE_BIT;
        barriers[0].oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barriers[0].newLayout     = VK_IMAGE_LAYOUT_GENERAL;
        barriers[0].image         = vk_texture;

        barriers[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barriers[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barriers[1].oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barriers[1].newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barriers[1].image         = vk_palette_texture;

        barriers[2].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barriers[2].dstAccessMask = VK_ACCESS_SHADER_READ_BIT
                                  | VK_ACCESS_SHADER_WRITE_BIT;
        barriers[2].oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barriers[2].newLayout     = VK_IMAGE_LAYOUT_GENERAL;
        barriers[2].image         = vk_zbuffer;

        vk_fn.CmdPipelineBarrier(vk_cmd_buffer,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 0,
                                 0, NULL,
                                 0, NULL,
                                 3, barriers);

        /* Particles dispatch.  One workgroup invocation
         * per particle; local_size_x = 64 (declared in
         * particles.comp), so dispatch ceil(count/64)
         * workgroups.  The in-shader pid >= count early-
         * out handles the tail. */
        vk_fn.CmdBindPipeline(vk_cmd_buffer,
                              VK_PIPELINE_BIND_POINT_COMPUTE,
                              vk_particles_pipeline);
        vk_fn.CmdBindDescriptorSets(vk_cmd_buffer,
                                    VK_PIPELINE_BIND_POINT_COMPUTE,
                                    vk_particles_pipeline_layout,
                                    0,
                                    1, &vk_particles_set,
                                    0, NULL);
        vk_fn.CmdPushConstants(vk_cmd_buffer,
                               vk_particles_pipeline_layout,
                               VK_SHADER_STAGE_COMPUTE_BIT,
                               0, sizeof(vk_particles_push),
                               &vk_particles_push);
        vk_fn.CmdDispatch(vk_cmd_buffer,
                          (vk_pending_particle_count + 63u) / 64u,
                          1, 1);

        /* After the particle dispatch, vk_texture is in
         * GENERAL.  The downstream palette compute
         * dispatch reads it as a sampled image (the
         * existing pipeline binds vk_texture_view via
         * descriptor set 0 binding 0 with imageLayout =
         * SHADER_READ_ONLY_OPTIMAL).  Transition GENERAL
         * -> SHADER_READ_ONLY_OPTIMAL, with srcStage =
         * dstStage = COMPUTE_SHADER so the particle
         * dispatch's writes flush before the palette
         * dispatch's reads.  srcAccess = SHADER_WRITE
         * (what the particle dispatch did); dstAccess =
         * SHADER_READ (what the palette dispatch will
         * do). */
        memset(&barriers[0], 0, sizeof(barriers[0]));
        barriers[0].sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barriers[0].srcAccessMask                   = VK_ACCESS_SHADER_WRITE_BIT;
        barriers[0].dstAccessMask                   = VK_ACCESS_SHADER_READ_BIT;
        barriers[0].oldLayout                       = VK_IMAGE_LAYOUT_GENERAL;
        barriers[0].newLayout                       = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barriers[0].srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
        barriers[0].dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
        barriers[0].image                           = vk_texture;
        barriers[0].subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        barriers[0].subresourceRange.baseMipLevel   = 0;
        barriers[0].subresourceRange.levelCount     = 1;
        barriers[0].subresourceRange.baseArrayLayer = 0;
        barriers[0].subresourceRange.layerCount     = 1;

        vk_fn.CmdPipelineBarrier(vk_cmd_buffer,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 0,
                                 0, NULL,
                                 0, NULL,
                                 1, barriers);
    } else {
        /* No-particles fast path: both textures
         * TRANSFER_DST -> SHADER_READ_ONLY directly.
         * dstStage = COMPUTE_SHADER (was FRAGMENT_SHADER
         * in Phase 4f) -- the consumer is now the
         * compute dispatch below. */
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
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 0,
                                 0, NULL,
                                 0, NULL,
                                 2, barriers);
    }

    /* Render target: UNDEFINED -> GENERAL.  UNDEFINED-as-
     * discard because the compute dispatch will write
     * every pixel via imageStore.  dstStage = COMPUTE_SHADER
     * orders the layout transition before the dispatch's
     * SHADER_WRITE. */
    memset(&image_barrier, 0, sizeof(image_barrier));
    image_barrier.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    image_barrier.srcAccessMask                   = 0;
    image_barrier.dstAccessMask                   = VK_ACCESS_SHADER_WRITE_BIT;
    image_barrier.oldLayout                       = VK_IMAGE_LAYOUT_UNDEFINED;
    image_barrier.newLayout                       = VK_IMAGE_LAYOUT_GENERAL;
    image_barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
    image_barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
    image_barrier.image                           = vk_image;
    image_barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    image_barrier.subresourceRange.baseMipLevel   = 0;
    image_barrier.subresourceRange.levelCount     = 1;
    image_barrier.subresourceRange.baseArrayLayer = 0;
    image_barrier.subresourceRange.layerCount     = 1;

    vk_fn.CmdPipelineBarrier(vk_cmd_buffer,
                             VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0,
                             0, NULL,
                             0, NULL,
                             1, &image_barrier);

    /* Compute dispatch.  Workgroup size 16x16 (declared in
     * the shader); dispatch ceil(width/16) x ceil(height/16)
     * workgroups so we cover the whole image, with the
     * shader's bounds check handling the overshoot when
     * width or height isn't a multiple of 16.
     *
     * Phase 5b-03: when vk_warp_active, dispatch the fused
     * warp + palette pipeline instead of the regular
     * palette pipeline.  Same workgroup layout, same
     * dispatch grid, same image-layout requirements
     * (vk_texture SHADER_READ_ONLY, vk_palette_texture
     * SHADER_READ_ONLY, vk_image GENERAL) -- the only
     * differences are the pipeline / pipeline-layout /
     * descriptor-set handles and the additional push-
     * constants payload (phase + scr_vrect bounds).  The
     * warp pipeline reads its sin-table UBO as binding 3
     * of its own descriptor set; nothing to coordinate
     * with the rest of the frame's barrier sequence
     * (UBO reads don't need a layout transition). */
    if (vk_warp_active) {
        vk_fn.CmdBindPipeline(vk_cmd_buffer,
                              VK_PIPELINE_BIND_POINT_COMPUTE,
                              vk_warp_pipeline);
        vk_fn.CmdBindDescriptorSets(vk_cmd_buffer,
                                    VK_PIPELINE_BIND_POINT_COMPUTE,
                                    vk_warp_pipeline_layout,
                                    0,
                                    1, &vk_warp_set,
                                    0, NULL);
        vk_fn.CmdPushConstants(vk_cmd_buffer,
                               vk_warp_pipeline_layout,
                               VK_SHADER_STAGE_COMPUTE_BIT,
                               0, sizeof(vk_warp_push),
                               &vk_warp_push);
    } else {
        vk_fn.CmdBindPipeline(vk_cmd_buffer,
                              VK_PIPELINE_BIND_POINT_COMPUTE,
                              vk_compute_pipeline);
        vk_fn.CmdBindDescriptorSets(vk_cmd_buffer,
                                    VK_PIPELINE_BIND_POINT_COMPUTE,
                                    vk_pipeline_layout,
                                    0,                /* firstSet */
                                    1, &vk_descriptor_set,
                                    0, NULL);          /* no dynamic offsets */
    }

    group_count_x = (width  + 15u) / 16u;
    group_count_y = (height + 15u) / 16u;
    vk_fn.CmdDispatch(vk_cmd_buffer, group_count_x, group_count_y, 1);

    /* Phase 4h: overlay render pass.  No explicit
     * pipeline barrier between the compute dispatch and
     * the render pass; the render pass's EXTERNAL -> 0
     * subpass dependency carries the memory dependency
     * (srcStage = COMPUTE_SHADER, srcAccess = SHADER_
     * WRITE -> dstStage = COLOR_ATTACHMENT_OUTPUT,
     * dstAccess = COLOR_ATTACHMENT_WRITE | _READ), and
     * the attachment description's
     * initialLayout = GENERAL receives the image straight
     * from the dispatch.
     *
     * The render pass's finalLayout = SHADER_READ_ONLY_
     * OPTIMAL is what hands the image to the frontend in
     * the right layout; no explicit post-render-pass
     * barrier needed (the render pass implicitly does
     * the COLOR_ATTACHMENT_OPTIMAL ->
     * SHADER_READ_ONLY_OPTIMAL transition at end).
     *
     * The Draw is 4 vertices, 1 instance: triangle-strip
     * topology turns those into the 2 triangles of the
     * gradient quad. */
    memset(&rpbi, 0, sizeof(rpbi));
    rpbi.sType                    = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpbi.renderPass               = vk_overlay_render_pass;
    rpbi.framebuffer              = vk_overlay_framebuffer;
    rpbi.renderArea.extent.width  = width;
    rpbi.renderArea.extent.height = height;
    rpbi.clearValueCount          = 0;
    rpbi.pClearValues             = NULL;

    /* Phase 4k: write the vertex buffer from the draw
     * list Draw_Pic intercepts populated during retro_run.
     * Phase 4j had this fill happen once at create_
     * resources; moving it here lets the draw list change
     * each frame.  Safe to call with vk_overlay_draw_count
     * == 0 (the loop simply does nothing and the per-
     * frame CmdDraw count below stays zero too -- the
     * render pass still runs but draws nothing, which is
     * cheap). */
    backend_vk_fill_overlay_vb();

    vk_fn.CmdBeginRenderPass(vk_cmd_buffer, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
    vk_fn.CmdBindPipeline(vk_cmd_buffer,
                          VK_PIPELINE_BIND_POINT_GRAPHICS,
                          vk_overlay_pipeline);
    /* Phase 4j: bind the overlay vertex buffer once, then
     * loop the draw list -- each entry rebinds its slot's
     * descriptor set (different texture per pic) and
     * draws 4 verts at the right firstVertex offset.  The
     * GRAPHICS bind point is independent of the COMPUTE
     * bind point used earlier in the frame, so the
     * compute set stays bound where the compute dispatch
     * left it. */
    vb_offset = 0;
    vk_fn.CmdBindVertexBuffers(vk_cmd_buffer,
                               0,                          /* firstBinding */
                               1, &vk_overlay_vertex_buffer,
                               &vb_offset);
    {
        unsigned di;
        for (di = 0; di < vk_overlay_draw_count; di++) {
            const struct overlay_draw *d   = &vk_overlay_draws[di];
            const struct overlay_slot *slt = &vk_overlay_slots[d->slot_idx];
            vk_fn.CmdBindDescriptorSets(vk_cmd_buffer,
                                        VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        vk_pipeline_layout,
                                        0,
                                        1, &slt->descriptor_set,
                                        0, NULL);
            vk_fn.CmdDraw(vk_cmd_buffer, 4, 1, di * 4, 0);
        }
    }
    vk_fn.CmdEndRenderPass(vk_cmd_buffer);

    r = vk_fn.EndCommandBuffer(vk_cmd_buffer);
    if (r != VK_SUCCESS) {
        if (log_cb)
            log_cb(RETRO_LOG_ERROR, "rhi-vk: EndCommandBuffer failed (%d)\n", (int)r);
        return false;
    }

    /* Phase 4k: reset the draw list now that the command
     * buffer has captured everything it needs.  The next
     * frame's retro_run will populate it from scratch via
     * Draw_Pic intercepts.  Slot data stays put -- only
     * the per-frame queue resets; the cache lives until
     * destroy_resources. */
    vk_overlay_draw_count = 0;

    /* Phase 5b-02: reset the particle dispatch count now
     * that the command buffer has captured the dispatch.
     * Next frame's R_DrawParticles will repopulate it if
     * GPU particles run again; otherwise the no-particles
     * fast path takes over. */
    vk_pending_particle_count = 0;

    /* Phase 5b-03: reset the warp flag too.  Next frame's
     * R_RenderView will re-set it via dispatch_3d_warp_
     * screen if the view is still in water. */
    vk_warp_active = false;

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

    /* Phase 5b-02: if a particle dispatch was staged this
     * frame, also push d_pzbuffer into the GPU-side zbuffer
     * staging buffer so record_frame can CopyBufferToImage
     * it into vk_zbuffer.  Conditional because the upload
     * is ~4 MiB at 1080p -- worth skipping on frames with
     * no GPU particles (the staging buffer's previous
     * contents are irrelevant; nothing reads them). */
    if (vk_pending_particle_count > 0)
        backend_vk_upload_zbuffer();

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
    backend_vk_end_frame,
    backend_vk_queue_2d_pic_entry,
    backend_vk_queue_2d_char_entry,
    backend_vk_queue_2d_pic_scaled_entry,
    backend_vk_queue_2d_console_background_entry,
    backend_vk_queue_2d_pic_translate_scaled_entry,
    backend_vk_queue_2d_fill_entry,
    backend_vk_queue_2d_fade_screen_entry,
    backend_vk_dispatch_3d_particles_impl,
    backend_vk_dispatch_3d_warp_screen_impl
};
