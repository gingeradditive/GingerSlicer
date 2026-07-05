#pragma once
// Ginger single-path wall rib connectors (single_path_wall_ribs).
//
// Merges the wall loops of one island (outer wall + hole walls, given as their centerline
// polygons) into ONE closed extrusion walk by inserting a "rib" at the closest approach of
// each loop to the growing merged walk (Prim-style, which is a minimum spanning tree over
// the loops). A rib is TWO link segments staggered by one extrusion width, so the beads
// touch side by side (fused flanks = a thin internal rib) but never share a centerline
// (no extrusion on top of itself). Each spliced loop gets a cut of one stagger length
// where the walk detours through the other loop, exactly like the manual "micro cut" a
// user would make in CAD to weld a hole wall to the outer wall.
//
// The plan is computed ONCE per layer in the slicing pipeline (PrintObject::prepare_infill,
// sequential bottom-up) so that:
//  - the rib CORRIDORS can be subtracted from the fill surfaces (no infill/bottom/top
//    extruded across the rib beads), and
//  - each rib is ANCHORED to the previous layer's rib position (self-standing column:
//    the rib prints on top of yesterday's rib, never in the air over sparse infill).
// The G-code generator only consumes the stored plan.

#include "Polygon.hpp"
#include "ExPolygon.hpp"

#include <limits>

namespace Slic3r {

// One planned merge of an island's wall loops into a single closed walk.
struct WallRibMerge
{
    // first_point() of every consumed source loop - the loop identity at G-code time (the
    // island regions reference the very same ExtrusionLoop objects, so exact comparison works).
    Points   loop_keys;
    // The resulting single closed walk (source loops + rib links, cuts applied).
    Polygon  merged;
    // One expanded quad per rib, to subtract from the layer's fill surfaces.
    Polygons corridors;
    // One anchor per rib (midpoint of the first link); the next layer reuses these positions
    // so the rib columns stack.
    Points   anchors;
};

// What the previous layer offers to stand on. A rib link is only accepted where every sampled
// point along it rests on one of these (a rib must NEVER be extruded in the air - "void" =
// inside the part's outline but with nothing printed below, e.g. over sparse infill).
struct WallRibSupport
{
    // First object layer prints on the bed: everything is supported.
    bool              first_layer    { false };
    // The previous layer's rib corridors (a rib standing on yesterday's rib = the column).
    const Polygons   *prev_corridors { nullptr };
    // The previous layer's wall loop centerlines (a rib may land on a wall bead).
    const Polygons   *prev_walls     { nullptr };
    // The previous layer's SOLID fill surfaces (bottom/top/internal solid are continuous
    // material; sparse infill is NOT support).
    const ExPolygons *prev_solid     { nullptr };
};

// Extra constraints on rib placement.
struct WallRibParams
{
    // Link separation = the extrusion line width (scaled).
    coord_t stagger;
    // How much the rib quad is expanded into the corridor polygon (bead half width, scaled).
    coord_t corridor_offset;
    // Loops farther apart than this stay unmerged: a very long rib would cross half the part
    // (and everything below it) while the travel it replaces is already short. Scaled.
    coord_t max_link_length { std::numeric_limits<coord_t>::max() };
    // Max per-layer drift of a rib column (scaled). When following the previous layer's anchor
    // would require more, the rib is dropped for this layer instead of staircasing sideways.
    coord_t max_drift { std::numeric_limits<coord_t>::max() };
    // All wall loops of the island (including the non-mergeable ones): a rib link must not
    // cross any of them, or it would extrude straight over a hole / another wall.
    const Polygons *obstacles { nullptr };
    // Support of the previous layer; null = no support constraint (geometry-only use / tests).
    const WallRibSupport *support { nullptr };
};

// Plan the merge of `loops` (centerline polygons of one island's wall loops, same role/flow).
// `prev_anchors` (optional) biases each rib to the previous layer's position when the local
// geometry still allows it (self-standing columns). Loops that cannot be merged (too short to
// host a cut, farther than max_link_length, link would cross an obstacle, drift over budget)
// are reported in `unmerged` (indices into `loops`) and left out. Returns false when fewer
// than two loops end up merged.
bool plan_wall_ribs(const Polygons &loops, const WallRibParams &params,
                    const Points *prev_anchors, WallRibMerge &out, std::vector<size_t> &unmerged);

// Geometry-only convenience wrapper (unit tests): merge and return just the walk.
bool splice_wall_loops(const Polygons &loops, coord_t stagger, Polygon &merged, std::vector<size_t> &unmerged);

} // namespace Slic3r
