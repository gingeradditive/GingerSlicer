#ifndef slic3r_WallFusion_hpp_
#define slic3r_WallFusion_hpp_

// Ginger single_path_infill_as_wall: the outer wall takes over the Lightning branches.
//
// Instead of anchoring a branch against the wall - a T junction, the "anchor" that shows through
// transparent material - the wall loop itself detours inward around every branch, goes around it
// and comes back. The construction is a boolean, not a new router:
//
//     loop = boundary( P \ (branches (+) spacing/2) )        P = region inside the wall centerline
//
// The bead keeps its usual position (half a width from the surface) and the skin stays closed: the
// two branch flanks sit one spacing apart and are one width each, so together they cover the mouth
// they opened. Everything downstream keeps treating the result as what it is - a wall loop - so it
// inherits the rib planner, the seam, the wall speed/flow and the role-preserving re-emission.
//
// Rules enforced here (numbered as in the design review, docs/ginger/DFM.md):
//   R2 roots are extended out to the wall centerline, or the gorge never opens;
//   R3 branches shorter than `prune_length` are dropped (a gorge that short is a dent, not a detour);
//   R5 the cleanup opening is applied to the INTERIOR only - run over the whole region it eats
//      stretches of wall (measured: 1.0% of the skin band uncovered vs 0.2%);
//   R9 two roots closer than `root_min_gap` on the same stretch of wall merge their mouths into one
//      2-spacing opening, which is exactly the coverage limit - the second one is demoted to a
//      plain branch (kept, but not extended to the wall: it becomes an inner ring, not a mouth).
// R4 is not a rule: R3 implies it (a tip close to the wall implies a short branch), so the caller
// simply takes every curve the boolean returns instead of assuming there is one.

#include "Polygon.hpp"
#include "Polyline.hpp"
#include "ExPolygon.hpp"

namespace Slic3r {

struct WallFusionParams
{
    // Distance between the two flanks of a gorge = the wall's own spacing (flank contact = fusion).
    coord_t spacing      { 0 };
    // R3: branches shorter than this are not worth a detour. ZERO by default, and that is the
    // point: whatever the fusion refuses is material the layer loses, because a fused island gets
    // no sparse infill at all. The tree cannot be asked to be tidier either - the generator's own
    // prune length is the 45 degree overhang budget (one layer height per layer), not a cosmetic
    // knob, so a twig it kept is a twig something above stands on. A dent in the wall beats a
    // missing support. Left as a parameter for GINGER_FUSION_PRUNE_W experiments only.
    coord_t prune_length { 0 };
    // R2: a branch end closer than this to the wall centerline is a root, and gets extended to it.
    coord_t root_reach   { 0 };
    // R9: minimum distance between two roots on the wall. A root that lands too close to one
    // already taken is DEMOTED, not dropped: it keeps its material (its tube becomes an inner ring
    // the rib planner welds) but it does not open a second mouth next to the first.
    coord_t root_min_gap { 0 };
};

struct WallFusionResult
{
    // Rings to print as wall: the outer contour plus any hole/extra ring the boolean produced.
    // More than one closed curve is legal (crossing branches split the region); the caller passes
    // them all on and lets the rib planner weld them.
    Polygons   loops;
    // Where the leftover sparse infill may still go: the fused region eroded by half a spacing.
    ExPolygons interior;
    // Footprint to carve out of the fill surfaces: the gorges only. Carving with `interior` instead
    // would pull the fill back from the WHOLE island edge (the fill boundary carries
    // infill_wall_overlap, which `interior` does not), and the tree roots live exactly there - the
    // pruned branches would lose their anchor and vanish from the layer. Like the rib corridors,
    // it is a quarter of a spacing tighter than the beads' true footprint, so what is still printed
    // fuses into the gorge flanks instead of leaving a gap.
    Polygons   carve;
    // Census, for GINGER_FUSION_DEBUG.
    size_t     gorges        { 0 };
    // Roots the tree had on this wall BEFORE pruning: the anchors the fusion set out to remove.
    // gorges < roots_before_prune means some anchors are still printed as ordinary infill.
    size_t     roots_before_prune { 0 };
    size_t     pruned        { 0 };
    size_t     dropped_roots { 0 };
    // Branches the fusion did NOT take into the wall (pruned by an explicit prune_length). Zero
    // means the island's whole tree became wall, which is what lets the caller drop its sparse
    // fill entirely: nothing is left to print there.
    size_t     left_to_infill { 0 };
};

// `island` is the material region bounded by the island's wall centerlines - the outer loop as the
// contour, one hole per inner loop - and `branches` the Lightning tree polylines of the same layer
// (unclipped: their roots may sit inside a ring, they are extended out to it here). Every ring is
// wall, so a branch rooted on a hole opens its gorge there; the returned `loops` are the rings of
// the reshaped region and REPLACE the island's whole loop set - two of them may have merged into
// one (a branch bridging a hole to the outer wall) or one may have split in two.
WallFusionResult fuse_wall_with_branches(const ExPolygon        &island,
                                         const Polylines        &branches,
                                         const WallFusionParams &params);

// Convenience overload for a solid island (no holes).
inline WallFusionResult fuse_wall_with_branches(const Polygon          &wall_loop,
                                                const Polylines        &branches,
                                                const WallFusionParams &params)
{
    return fuse_wall_with_branches(ExPolygon(wall_loop), branches, params);
}

} // namespace Slic3r

#endif // slic3r_WallFusion_hpp_
