/*
 * Vertex shader for the Phase 4h 2D overlay path.
 *
 * Reads a 2D position (already in NDC) and an RGBA colour
 * from vertex input.  Writes gl_Position and forwards the
 * colour to the FS for per-pixel interpolation.
 *
 * Position is in NDC directly (no projection) so the
 * caller controls placement.  Vulkan NDC has y = -1 at
 * the top of the framebuffer and y = +1 at the bottom,
 * so a small rectangle in the upper-left of the screen
 * uses negative x and negative y near -1.
 *
 * Future phases will likely add a projection matrix push
 * constant so Quake's 320x200-based screen coordinates
 * can be passed in directly without manual NDC mapping,
 * and uv attributes so textured pics work.  For Phase 4h
 * the path is solid-colour only -- the first real
 * Draw_Pic intercept (Phase 4i) will add the uv stream
 * and a sampler binding.
 *
 * Layout bindings:
 *   location 0:  vec2  position  (NDC, x in [-1, +1], y in [-1, +1])
 *   location 1:  vec4  colour    (linear RGB + alpha, alpha in [0, 1])
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 The libretro team
 */
#version 450

layout(location = 0) in vec2 v_pos;
layout(location = 1) in vec4 v_color;

layout(location = 0) out vec4 f_color;

void main()
{
    gl_Position = vec4(v_pos, 0.0, 1.0);
    f_color     = v_color;
}
