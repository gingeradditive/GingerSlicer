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
//      2-spacing opening, which is exactly the coverage limit - the second root is dropped.
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
    // R3: branches shorter than this are not worth a detour. 2.5 line widths.
    coord_t prune_length { 0 };
    // R2: a branch end closer than this to the wall centerline is a root, and gets extended to it.
    coord_t root_reach   { 0 };
    // R9: minimum distance between two roots on the wall.
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
    // Census, for GINGER_FUSION_DEBUG.
    size_t     gorges        { 0 };
    size_t     pruned        { 0 };
    size_t     dropped_roots { 0 };
};

// `wall_loop` is the centerline of the outer wall, `branches` the Lightning tree polylines of the
// same layer (unclipped: their roots may sit inside the loop, they are extended out to it here).
WallFusionResult fuse_wall_with_branches(const Polygon          &wall_loop,
                                         const Polylines        &branches,
                                         const WallFusionParams &params);

} // namespace Slic3r

#endif // slic3r_WallFusion_hpp_
