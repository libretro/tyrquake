/*
 * Fragment shader for the Phase 4h 2D overlay path.
 *
 * Outputs the per-vertex colour as interpolated across
 * the primitive.  Alpha is preserved so the pipeline's
 * blend stage (SRC_ALPHA / ONE_MINUS_SRC_ALPHA) can
 * composite the quad over whatever's already in the
 * render target -- which on the Phase 4g compute path
 * is the SW renderer's output translated through the
 * palette LUT.
 *
 * No texture sample, no light, no fog.  This is the
 * minimum-viable FS for the overlay path; Phase 4i will
 * add texture sampling for real Draw_Pic content.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 The libretro team
 */
#version 450

layout(location = 0) in  vec4 f_color;
layout(location = 0) out vec4 out_color;

void main()
{
    out_color = f_color;
}
