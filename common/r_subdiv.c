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
/* r_subdiv.c: Catmull-style 1-to-4 polygon subdivision for alias models.
 *
 * Quake alias models are low-poly meshes designed for 1996 hardware.
 * On a modern host we can afford to spend extra CPU and memory turning
 * those flat-shaded silhouettes into smoother surfaces.  The classic
 * approach for triangle meshes is Loop subdivision (Charles Loop,
 * 1987), which is the triangle analogue of Catmull-Clark subdivision
 * for quad meshes; "Catmull subdivision" is the casual term and is
 * what's exposed in the menu.
 *
 * What we actually implement is a hybrid:
 *
 *   - Topology: pure 1-to-4 split.  Every triangle ABC produces three
 *     new edge-midpoint vertices and four sub-triangles
 *     (A, ab, ca), (ab, B, bc), (ca, bc, C), (ab, bc, ca).  Edges are
 *     deduplicated across triangles so adjacent faces share their
 *     midpoint vertex; the mesh stays watertight.
 *
 *   - Geometry: edge midpoints are the plain byte-quantised midpoint
 *     of the two endpoint positions.  An earlier revision used the
 *     standard Loop interior mask 3/8(a + b) + 1/8(c + d), which
 *     converges to a smooth limit surface but visibly shrinks convex
 *     silhouettes -- shotgun stocks, monster horns, axe blades --
 *     on the low-poly Quake meshes this code targets.  Plain
 *     midpoints keep every new vertex exactly on the original edge
 *     polyline, so the model's outline is preserved bit-for-bit;
 *     visual smoothing comes entirely from the higher triangle
 *     count feeding Gouraud-interpolated shading, see below.
 *
 *   - Vertex normals: trivertx_t.lightnormalindex picks one of the
 *     162 precomputed directions in r_avertexnormals[].  For a
 *     midpoint we want the normal closest to the average of the two
 *     endpoint normals so per-vertex Gouraud light values stay
 *     continuous across the subdivided faces.  Inheriting a single
 *     endpoint's index (the obvious cheap shortcut) makes adjacent
 *     sub-triangles each snap to one parent's flat-shaded light
 *     value, producing the very dark/light banding subdivision is
 *     supposed to remove.  We precompute a 162x162 lookup table on
 *     first call so the per-midpoint normal lookup is a single
 *     indirection.
 *
 * Existing vertices are NOT smoothed.  Pure Loop subdivision would
 * also reposition every old vertex via a weighted average of its
 * neighbours; we skip that step ("interpolating" subdivision).  Per
 * the geometry note above, this preserves the model silhouette
 * exactly and avoids visible shrinkage Loop produces around
 * extremities, for 1-3 passes on Quake-scale meshes.
 *
 * Per-pose data:
 *
 *   Each pose frame has its own array of trivertx_t (byte-packed
 *   xyz + lightnormalindex).  The triangle/stvert topology is shared
 *   across all poses.  We compute the new edge topology once, then
 *   evaluate the weighted vertex positions independently for each
 *   pose; new vertices stay quantised to bytes.
 *
 * Seam handling (stvert_t.onseam):
 *
 *   A new midpoint inherits ALIAS_ONSEAM only when both endpoints are
 *   onseam.  This is correct for edges that run along the texture
 *   seam itself; midpoints of edges crossing the seam may produce
 *   small s-coordinate discontinuities on back-facing triangles but
 *   that's also what happens for the unsubdivided mesh, just at a
 *   different scale.
 */

#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "console.h"
#include "cvar.h"
#include "model.h"
#include "modelgen.h"
#include "qtypes.h"
#include "r_subdiv.h"
#include "sys.h"

cvar_t r_polysubdiv = { "r_polysubdiv", "0", true };

/*
 * Quake's 162-direction lightnormal table lives in r_alias.c; we
 * only need the extern declaration here, and a local copy of the
 * count.  Duplicating the constant matches the convention in
 * r_part.c, which has the same need and avoids dragging the full
 * rasterizer state through r_local.h into this module.
 */
#define NUMVERTEXNORMALS 162
extern float r_avertexnormals[NUMVERTEXNORMALS][3];

/*
 * mid_normal_lut[a][b] = lightnormalindex closest to
 * r_avertexnormals[a] + r_avertexnormals[b], i.e. the best
 * representative for the averaged normal of an edge whose
 * endpoints have lightnormalindex a and b.
 *
 * 162 * 162 = 26244 entries, 26KB total.  Built lazily on first
 * subdivision so cores that never enable r_polysubdiv pay zero
 * setup cost.  Build cost is ~4M dot products (a few ms) paid
 * once per process.
 */
static byte mid_normal_lut[NUMVERTEXNORMALS][NUMVERTEXNORMALS];
static qboolean mid_normal_lut_ready = false;

static byte
ClosestNormalIndex(float nx, float ny, float nz)
{
    int best = 0;
    float best_dot = -2.0f;	/* below any unit-dot range, so first
				 * sample always wins */
    int k;
    for (k = 0; k < NUMVERTEXNORMALS; k++) {
	float d = nx * r_avertexnormals[k][0]
		+ ny * r_avertexnormals[k][1]
		+ nz * r_avertexnormals[k][2];
	if (d > best_dot) {
	    best_dot = d;
	    best = k;
	}
    }
    return (byte)best;
}

static void
BuildMidNormalLUT(void)
{
    int i, j;
    if (mid_normal_lut_ready)
	return;
    for (i = 0; i < NUMVERTEXNORMALS; i++) {
	for (j = 0; j < NUMVERTEXNORMALS; j++) {
	    /* No need to normalise the sum; ClosestNormalIndex
	     * compares dot products against unit-length entries,
	     * so the magnitude factor is the same for every
	     * candidate and falls out of the argmax.  Diametrically
	     * opposite endpoints produce a zero-length sum and the
	     * argmax becomes ill-defined; we just accept whatever
	     * the linear scan picks first.  That case is vanishingly
	     * rare on real Quake meshes -- it requires an edge
	     * between two vertices with antipodal surface normals,
	     * which would be a self-intersecting model. */
	    mid_normal_lut[i][j] =
		ClosestNormalIndex(r_avertexnormals[i][0] + r_avertexnormals[j][0],
				   r_avertexnormals[i][1] + r_avertexnormals[j][1],
				   r_avertexnormals[i][2] + r_avertexnormals[j][2]);
	}
    }
    mid_normal_lut_ready = true;
}

int
R_PolySubdivPasses(void)
{
    int v = (int)r_polysubdiv.value;
    if (v < 0)
	return 0;
    if (v > R_POLYSUBDIV_MAX_PASSES)
	return R_POLYSUBDIV_MAX_PASSES;
    return v;
}

/* ----------------------------------------------------------------- */
/* Internal: edge hash table for one subdivision pass.               */
/* ----------------------------------------------------------------- */

typedef struct subdiv_edge_s {
    int v0;		/* lower vertex index of edge */
    int v1;		/* higher vertex index of edge */
    int new_vert;	/* index of the inserted midpoint vertex */
    int facesfront_mask;/* bit 0: a front-facing tri uses this edge,
			 * bit 1: a back-facing tri uses this edge.
			 * mask==3 means the edge is the texture seam. */
    int next;		/* hash chain pointer, -1 = end */
} subdiv_edge_t;

static int
NextPow2(int x)
{
    int p = 1;
    while (p < x)
	p <<= 1;
    return p;
}

/*
 * Round-half-up midpoint of two unsigned 8-bit values.
 * (a + b + 1) >> 1 is the standard rounding average without overflow,
 * since the sum fits in 9 bits.
 */
static INLINE byte
ByteMid2(int a, int b)
{
    return (byte)((a + b + 1) >> 1);
}

/*
 * Apply a single 1-to-4 subdivision pass.  Inputs and outputs are
 * malloc'd buffers; on success the *_io pointers are freed and
 * replaced.  Returns false (with originals untouched) if the result
 * would exceed the runtime caps or on allocation failure.  skinwidth
 * is the model's skin width, needed to resolve the texture-seam
 * side that an interior midpoint sits on (see the stvert build
 * loop below).
 */
static qboolean
SubdivOnePass(int *numverts_io, int *numtris_io, int numposes, int skinwidth,
	      stvert_t **stverts_io, mtriangle_t **tris_io,
	      trivertx_t **poses_io)
{
    int nv = *numverts_io;
    int nt = *numtris_io;
    int bucket_count, bucket_mask;
    int *buckets = NULL;
    subdiv_edge_t *edges = NULL;
    int (*tri_edge_idx)[3] = NULL;
    int num_edges = 0;
    int new_nv, new_nt;
    stvert_t *new_stverts = NULL;
    mtriangle_t *new_tris = NULL;
    trivertx_t *new_poses = NULL;
    stvert_t *old_stverts = *stverts_io;
    mtriangle_t *old_tris = *tris_io;
    trivertx_t *old_poses = *poses_io;
    int i, j, pose;

    /* Edge hashtable: a power-of-two bucket count keyed by a cheap
     * mix of the two endpoint indices.  Worst-case chain length is
     * roughly 6 (each vert participates in ~6 edges for a regular
     * triangle mesh), so this stays close to O(1) lookups. */
    bucket_count = NextPow2(nv);
    if (bucket_count < 16)
	bucket_count = 16;
    bucket_mask = bucket_count - 1;

    buckets = (int *)malloc(bucket_count * sizeof(int));
    if (!buckets)
	goto fail;
    for (i = 0; i < bucket_count; i++)
	buckets[i] = -1;

    /* Each triangle contributes 3 edges; shared edges are deduplicated
     * but the worst-case upper bound is 3*nt. */
    edges = (subdiv_edge_t *)malloc((size_t)nt * 3 * sizeof(subdiv_edge_t));
    if (!edges)
	goto fail;

    tri_edge_idx = (int (*)[3])malloc((size_t)nt * sizeof(*tri_edge_idx));
    if (!tri_edge_idx)
	goto fail;

    /* Walk every triangle edge, deduplicating into a flat list of
     * unique undirected edges, and OR each contributing triangle's
     * facesfront bit into the edge record.  The combined mask later
     * tells us whether each edge is front-only (mask=1), back-only
     * (mask=2) or the texture seam itself (mask=3), which the
     * stvert build loop needs to keep mid-edge texture coordinates
     * on the right half of the skin. */
    for (i = 0; i < nt; i++) {
	int facebit = old_tris[i].facesfront ? 1 : 2;
	for (j = 0; j < 3; j++) {
	    int a = old_tris[i].vertindex[j];
	    int b = old_tris[i].vertindex[(j + 1) % 3];
	    int v0 = (a < b) ? a : b;
	    int v1 = (a < b) ? b : a;
	    unsigned h = ((unsigned)v0 * 73856093u) ^ ((unsigned)v1 * 19349663u);
	    int bucket = (int)(h & (unsigned)bucket_mask);
	    int e = buckets[bucket];
	    while (e != -1 && (edges[e].v0 != v0 || edges[e].v1 != v1))
		e = edges[e].next;
	    if (e == -1) {
		e = num_edges++;
		edges[e].v0 = v0;
		edges[e].v1 = v1;
		edges[e].new_vert = nv + e;
		edges[e].facesfront_mask = facebit;
		edges[e].next = buckets[bucket];
		buckets[bucket] = e;
	    } else {
		edges[e].facesfront_mask |= facebit;
	    }
	    tri_edge_idx[i][j] = e;
	}
    }

    new_nv = nv + num_edges;
    new_nt = nt * 4;

    /* Cap enforcement: the caller picks how to react (try fewer
     * passes), so just refuse here. */
    if (new_nv > MAXALIASVERTS_RUNTIME || new_nt > MAXALIASTRIS_RUNTIME)
	goto fail;

    /* stverts: copy old, then synthesize one new entry per unique
     * edge.  The midpoint's onseam flag and s coordinate are
     * picked per the edge's facesfront_mask:
     *
     *   mask=3 (front + back triangles both use this edge):
     *       The edge IS the texture seam.  Midpoint is also on the
     *       seam: onseam=1, s = average of the front-side raw s
     *       values of the endpoints.  The renderer's seam fixup
     *       will lift s by skinwidth/2 for back-facing draws,
     *       exactly as for the original onseam vertices.
     *
     *   mask=1 (front-facing triangles only):
     *       Midpoint sits in the front half of the skin.
     *       onseam=0, s = average of raw s values.  onseam
     *       endpoints' stored s is already the front-side coord,
     *       so no adjustment is needed.
     *
     *   mask=2 (back-facing triangles only):
     *       Midpoint sits in the back half.  onseam=0, but the
     *       renderer applies no fixup to non-onseam vertices, so
     *       we have to bake the back-side coord into s directly.
     *       For onseam endpoints we lift their stored front-side
     *       s by skinwidth/2 before averaging; for non-onseam
     *       endpoints the stored s is already in the back half.
     *
     * Without the mask=2 branch, back-side interior midpoints
     * (every subdivided face on the back of the model) sample
     * skinwidth/4 too far left in the texture, producing the
     * ribbon-of-wrong-color streaks visible on subdivided
     * monster corpses before this change. */
    new_stverts = (stvert_t *)malloc((size_t)new_nv * sizeof(stvert_t));
    if (!new_stverts)
	goto fail;
    memcpy(new_stverts, old_stverts, (size_t)nv * sizeof(stvert_t));
    {
	int half = skinwidth / 2;
	for (i = 0; i < num_edges; i++) {
	    const stvert_t *a = &old_stverts[edges[i].v0];
	    const stvert_t *b = &old_stverts[edges[i].v1];
	    stvert_t *out = &new_stverts[nv + i];
	    int mask = edges[i].facesfront_mask;
	    int s_a = a->s;
	    int s_b = b->s;
	    if (mask == 3) {
		/* Seam midpoint. */
		out->onseam = ALIAS_ONSEAM;
	    } else {
		out->onseam = 0;
		if (mask == 2) {
		    /* Back-side interior: bake the seam fixup into
		     * any onseam endpoint contributions. */
		    if (a->onseam)
			s_a += half;
		    if (b->onseam)
			s_b += half;
		}
	    }
	    out->s = (s_a + s_b) / 2;
	    out->t = (a->t + b->t) / 2;
	}
    }

    /* Pose verts: one flat buffer of (numposes * new_nv) entries,
     * matching SW_LoadMeshData's expected layout. */
    new_poses = (trivertx_t *)malloc((size_t)numposes * (size_t)new_nv
				     * sizeof(trivertx_t));
    if (!new_poses)
	goto fail;
    for (pose = 0; pose < numposes; pose++) {
	const trivertx_t *src = &old_poses[(size_t)pose * (size_t)nv];
	trivertx_t *dst = &new_poses[(size_t)pose * (size_t)new_nv];
	memcpy(dst, src, (size_t)nv * sizeof(trivertx_t));
	for (i = 0; i < num_edges; i++) {
	    const trivertx_t *va = &src[edges[i].v0];
	    const trivertx_t *vb = &src[edges[i].v1];
	    trivertx_t *out = &dst[nv + i];
	    /* Plain midpoint for every edge -- see header comment.
	     * Loop-style weighting using opposite vertices shrinks
	     * convex silhouettes on chunky game models, which is
	     * the opposite of what users expect from a "smoother
	     * models" option; we leave positions exactly on the
	     * original edge polylines and let the higher tri count
	     * carry the visual smoothing via Gouraud lighting. */
	    out->v[0] = ByteMid2(va->v[0], vb->v[0]);
	    out->v[1] = ByteMid2(va->v[1], vb->v[1]);
	    out->v[2] = ByteMid2(va->v[2], vb->v[2]);
	    /* Averaged-normal lookup: the midpoint represents a
	     * point on the surface between the two endpoint
	     * vertices, so its smoothed normal is the normalised
	     * average of theirs.  Looked up via the precomputed
	     * 162x162 LUT, this is one memory indirection per
	     * midpoint per pose, vs. the visible flat-shaded
	     * banding produced by inheriting a single endpoint's
	     * index. */
	    out->lightnormalindex =
		mid_normal_lut[va->lightnormalindex][vb->lightnormalindex];
	}
    }

    /* Triangles: each old tri (A,B,C) produces 4 new tris using the
     * three edge midpoint vertex indices.  Edge 0 is AB (j=0 in the
     * edge build loop), edge 1 is BC (j=1), edge 2 is CA (j=2).
     * facesfront is inherited unchanged. */
    new_tris = (mtriangle_t *)malloc((size_t)new_nt * sizeof(mtriangle_t));
    if (!new_tris)
	goto fail;
    for (i = 0; i < nt; i++) {
	int A = old_tris[i].vertindex[0];
	int B = old_tris[i].vertindex[1];
	int C = old_tris[i].vertindex[2];
	int AB = edges[tri_edge_idx[i][0]].new_vert;
	int BC = edges[tri_edge_idx[i][1]].new_vert;
	int CA = edges[tri_edge_idx[i][2]].new_vert;
	int ff = old_tris[i].facesfront;
	mtriangle_t *o = &new_tris[i * 4];
	o[0].facesfront = ff;
	o[0].vertindex[0] = A;  o[0].vertindex[1] = AB; o[0].vertindex[2] = CA;
	o[1].facesfront = ff;
	o[1].vertindex[0] = AB; o[1].vertindex[1] = B;  o[1].vertindex[2] = BC;
	o[2].facesfront = ff;
	o[2].vertindex[0] = CA; o[2].vertindex[1] = BC; o[2].vertindex[2] = C;
	o[3].facesfront = ff;
	o[3].vertindex[0] = AB; o[3].vertindex[1] = BC; o[3].vertindex[2] = CA;
    }

    free(buckets);
    free(edges);
    free(tri_edge_idx);

    /* Replace inputs (caller hands ownership of the old buffers
     * to us; they get freed here). */
    free(old_stverts);
    free(old_tris);
    free(old_poses);
    *stverts_io = new_stverts;
    *tris_io = new_tris;
    *poses_io = new_poses;
    *numverts_io = new_nv;
    *numtris_io = new_nt;
    return true;

fail:
    if (buckets)
	free(buckets);
    if (edges)
	free(edges);
    if (tri_edge_idx)
	free(tri_edge_idx);
    if (new_stverts)
	free(new_stverts);
    if (new_tris)
	free(new_tris);
    if (new_poses)
	free(new_poses);
    return false;
}

qboolean
R_SubdivideAliasMesh(int passes, int numposes, int skinwidth,
		     int numverts_in, int numtris_in,
		     const stvert_t *stverts_in,
		     const mtriangle_t *tris_in,
		     const trivertx_t **poseverts_in,
		     int *numverts_out, int *numtris_out,
		     stvert_t **stverts_out, mtriangle_t **tris_out,
		     trivertx_t **poses_out, int *passes_applied)
{
    int nv = numverts_in;
    int nt = numtris_in;
    stvert_t *stverts = NULL;
    mtriangle_t *tris = NULL;
    trivertx_t *poses = NULL;
    int i;
    int applied = 0;

    if (passes_applied)
	*passes_applied = 0;
    if (passes < 1 || numverts_in <= 0 || numtris_in <= 0 || numposes <= 0)
	return false;
    if (passes > R_POLYSUBDIV_MAX_PASSES)
	passes = R_POLYSUBDIV_MAX_PASSES;

    /* First-use lazy init of the averaged-normal LUT.  Building it
     * here rather than at module load means non-subdivided runs
     * pay nothing for the table. */
    BuildMidNormalLUT();

    /* Materialise malloc'd copies of the read-only inputs so the
     * per-pass routine can free/replace them uniformly. */
    stverts = (stvert_t *)malloc((size_t)nv * sizeof(stvert_t));
    tris = (mtriangle_t *)malloc((size_t)nt * sizeof(mtriangle_t));
    poses = (trivertx_t *)malloc((size_t)numposes * (size_t)nv
				 * sizeof(trivertx_t));
    if (!stverts || !tris || !poses)
	goto fail;

    memcpy(stverts, stverts_in, (size_t)nv * sizeof(stvert_t));
    memcpy(tris, tris_in, (size_t)nt * sizeof(mtriangle_t));
    for (i = 0; i < numposes; i++)
	memcpy(&poses[(size_t)i * (size_t)nv], poseverts_in[i],
	       (size_t)nv * sizeof(trivertx_t));

    /* Apply passes one at a time; each call may reject the pass if
     * the next size would exceed the runtime cap.  We stop early in
     * that case rather than failing the whole subdivision, so a
     * partially smoothed mesh is still better than the original. */
    for (i = 0; i < passes; i++) {
	if (!SubdivOnePass(&nv, &nt, numposes, skinwidth,
			   &stverts, &tris, &poses))
	    break;
	applied++;
    }

    if (applied == 0)
	goto fail;

    *numverts_out = nv;
    *numtris_out = nt;
    *stverts_out = stverts;
    *tris_out = tris;
    *poses_out = poses;
    if (passes_applied)
	*passes_applied = applied;
    return true;

fail:
    if (stverts)
	free(stverts);
    if (tris)
	free(tris);
    if (poses)
	free(poses);
    return false;
}
