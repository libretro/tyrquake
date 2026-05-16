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
 * E ~= 1.5 F for a closed mesh).  The runtime vertex / triangle
 * caps below cover three passes applied to a small-to-medium source
 * mesh, or two passes applied to a max-sized source mesh.  Meshes
 * that would exceed these caps fall back to fewer passes; see
 * R_SubdivideAliasMesh.
 */
#define MAXALIASVERTS_RUNTIME 32768
#define MAXALIASTRIS_RUNTIME  65536

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
 *   numposes    - frame poses; each contributes numverts_in trivertx_t.
 *   skinwidth   - the model's full skin width in texels.  Used to
 *                 resolve which texture half a new midpoint vertex
 *                 sits on (front = [0, skinwidth/2), back =
 *                 [skinwidth/2, skinwidth]); see the stvert build
 *                 loop in r_subdiv.c.
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
			      int numposes,
			      int skinwidth,
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
