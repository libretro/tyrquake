/*
Copyright (C) 1996-1997 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

#ifndef R_SUBDIV_H
#define R_SUBDIV_H

#include "cvar.h"
#include "model.h"
#include "modelgen.h"
#include "qtypes.h"

/*
 * Maximum subdivision passes the user may request via r_polysubdiv.
 * Each pass quadruples the triangle count of an alias mesh, so three
 * passes is the practical ceiling before working-buffer growth and
 * per-frame transform cost become prohibitive.
 */
#define R_POLYSUBDIV_MAX_PASSES 3

/*
 * Per-pass subdivision multiplies the source mesh size by ~4x for
 * triangles and ~3x for vertices in the limit (V_after = V + E,
 * E ~= 1.5 F for a closed mesh).  The runtime caps below cap each
 * subdivided model at roughly 3.6 MB of Hunk image (8192 trivertx
 * poses * ~100 frames + 16384 tris + 8192 stverts).  At the original
 * cap of 32768 / 65536 a single max-sized model could reach 14 MB,
 * which on the default 32 MB libretro heap was enough to push
 * Cache_AllocPadded into Sys_Error("out of memory") once several
 * subdivided monsters became visible.  The new caps also reclaim
 * ~400 KB of BSS from the MAXALIASVERTS_RUNTIME-sized static
 * working buffers in r_alias.c.
 *
 * Practical effect on the user-facing setting:
 *   - 3 passes still works on small models (player.mdl head, weapon
 *     barrels, projectile sprites).
 *   - Mid-size models (most monsters at ~500 verts) cap at 2 passes;
 *     R_SubdivideAliasMesh drops the third pass and returns the
 *     2-pass result.
 *   - Max-sized source meshes cap at 1 pass.
 *
 * The cap-fallback in R_SubdivideAliasMesh tries passes one at a
 * time so a model that overshoots at pass N still keeps the result
 * of pass N-1; you never end up with an unsubdivided model just
 * because you asked for one pass too many.
 */
#define MAXALIASVERTS_RUNTIME 8192
#define MAXALIASTRIS_RUNTIME  16384

/* User-controlled subdivision level cvar (0..R_POLYSUBDIV_MAX_PASSES). */
extern cvar_t r_polysubdiv;

/*
 * Read r_polysubdiv with clamping, so consumers don't need to repeat
 * the range check.  Returns 0 when the cvar is out of range or
 * unregistered.
 */
int R_PolySubdivPasses(void);

/*
 * Apply 'passes' rounds of Catmull-style 1-to-4 polygon subdivision
 * to a triangle mesh.  See r_subdiv.c for the precise weighting
 * scheme and seam-handling rules.
 *
 * Inputs (read-only, caller-owned):
 *   passes      - number of subdivision passes to apply, >= 1.
 *   phong_tess  - if true, edge midpoints are placed via Phong
 *                 tessellation (Boubekeur & Alexa, SIGGRAPH Asia
 *                 2008): project the linear midpoint onto the
 *                 tangent planes at the two endpoint normals and
 *                 blend.  Rounds convex silhouettes outward.  When
 *                 false, midpoints sit on the original edge line.
 *   numposes    - frame poses; each contributes numverts_in trivertx_t.
 *   skinwidth   - the model's full skin width in texels.  Used to
 *                 resolve which texture half a new midpoint vertex
 *                 sits on (front = [0, skinwidth/2), back =
 *                 [skinwidth/2, skinwidth]); see the stvert build
 *                 loop in r_subdiv.c.
 *   scale       - the model's per-axis byte-to-model-space scale
 *                 (aliashdr_t.scale).  Used by the Phong tessellation
 *                 path to convert displacement back to byte units;
 *                 ignored when phong_tess is false.  May be NULL in
 *                 that case.
 *   numverts_in - source vertex count.
 *   numtris_in  - source triangle count.
 *   stverts_in  - [numverts_in] texture-coord verts.
 *   tris_in     - [numtris_in] triangle indices.
 *   poseverts_in- [numposes] array of pose pointers, each pointing
 *                 to numverts_in trivertx_t (i.e. an array-of-arrays
 *                 like the source MDL layout).
 *
 * Outputs (malloc'd on success, caller must free):
 *   *numverts_out, *numtris_out - resulting counts.
 *   *stverts_out                - [numverts_out].
 *   *tris_out                   - [numtris_out].
 *   *poses_out                  - flat [numposes * numverts_out].
 *
 * Returns true on success.  Returns false if any allocation fails or
 * the resulting mesh would exceed MAXALIASVERTS_RUNTIME /
 * MAXALIASTRIS_RUNTIME, after dropping passes one at a time to try
 * to stay under the caps.  If 'passes' is reduced to zero by the cap
 * check, false is returned (the caller should treat this as "subdivide
 * disabled for this model" and emit the source mesh unchanged).
 *
 * The effective number of passes actually applied is returned via
 * *passes_applied (may be NULL).
 */
qboolean R_SubdivideAliasMesh(int passes,
			      qboolean phong_tess,
			      int numposes,
			      int skinwidth,
			      const float *scale,
			      int numverts_in,
			      int numtris_in,
			      const stvert_t *stverts_in,
			      const mtriangle_t *tris_in,
			      const trivertx_t **poseverts_in,
			      int *numverts_out,
			      int *numtris_out,
			      stvert_t **stverts_out,
			      mtriangle_t **tris_out,
			      trivertx_t **poses_out,
			      int *passes_applied);

#endif /* R_SUBDIV_H */
