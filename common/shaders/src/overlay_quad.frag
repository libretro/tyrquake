/*
 * Fragment shader for the Phase 4i 2D overlay path.
 *
 * Samples an R8_UNORM palette-index texture at the
 * interpolated UV, then looks the index up in the same
 * 256x1 RGBA8 palette texture the compute palette LUT
 * uses (Phase 4f vk_palette_texture, propagated through
 * d_8to24table_shifted to track damage / quad / under-
 * water shifts).  Output is fully opaque -- the overlay
 * pipeline's blend stage is still enabled, but with
 * alpha = 1.0 the result is "src replaces dst" which is
 * fine for the Phase 4i demo and matches how Quake's
 * mostly-opaque HUD pics will composite once Phase 4j's
 * Draw_Pic intercept is in place.
 *
 * Why palette-indexed and not pre-converted RGBA: every
 * GPU-side pic uploaded from Quake's qpic_t will be 8bpp
 * (that's the source format), so doing the index ->
 * palette lookup on the GPU avoids a per-pixel CPU
 * convert at upload time, mirrors the compute path
 * exactly (every pic tracks runtime palette shifts the
 * same way the main framebuffer does), and keeps the
 * VRAM footprint small.
 *
 * Sampler binding details:
 *   set = 0, binding = 0:  R8_UNORM    index     (per-quad texture)
 *   set = 0, binding = 1:  R8G8B8A8_UNORM palette  (256x1, shared)
 *
 * Index sampling uses NEAREST filtering so Quake's
 * pixel-perfect 2D look survives any upscaling the
 * frontend does.  Palette sampling also uses NEAREST so
 * the 256 entries don't smear into each other; we already
 * trust the centered-texel mapping (u = b/255 in [0, 1]
 * with width 256 lands on texel b for every byte b).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 The libretro team
 */
#version 450

layout(set = 0, binding = 0) uniform sampler2D u_index;
layout(set = 0, binding = 1) uniform sampler2D u_palette;

layout(location = 0) in  vec2 f_uv;
layout(location = 0) out vec4 out_color;

void main()
{
    float idx = texture(u_index, f_uv).r;
    vec3  rgb = texture(u_palette, vec2(idx, 0.5)).rgb;

    out_color = vec4(rgb, 1.0);
}
