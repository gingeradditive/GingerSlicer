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
//    extruded across the rib beads),
//  - each rib is ANCHORED to the previous layer's rib position (self-standing column:
//    the rib prints on top of yesterday's rib, never in the air over sparse infill), and
//  - a rib that must START a column with nothing to stand on gets a FOUNDATION BUTTRESS:
//    on the layers below, one of its two walls grows an out-and-back stub of the same two
//    staggered beads (part of the wall, printed in the wall phase, zero travel), shrinking
//    by the self-support step each layer downward - a lightning-style branch leaning on the
//    wall. It needs no material under the gap at all, so it works even over true void; the
//    chain ends where it meets real support or melts back into the wall. Big-nozzle prints
//    rarely have infill dense enough to hold anything (grid 5% = beads ~70mm apart), and
//    the geometry below is NOT a guarantee of material - exactly the assumption that used
//    to collapse walls.
// The G-code generator only consumes the stored plan.

#include "Polygon.hpp"
#include "ExPolygon.hpp"

#include <functional>
#include <limits>
#include <utility>
#include <vector>

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
    // Links of ribs that start a NEW column with nothing to stand on (walk-side point, loop-
    // side point of the first link). The caller grows a FOUNDATION BUTTRESS under each: on
    // the layers below, one of the two walls gets an out-and-back stub of the same staggered
    // bead pair (part of the wall itself), shrinking by the self-support step per layer -
    // a lightning-style branch leaning on the wall, so the first rib never prints in the air.
    // After that the rib column carries itself on its own corridors.
    std::vector<std::pair<Point, Point>> founded_links;
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

// Diagnostic counters of one plan_wall_ribs call (aggregated per layer by the caller).
// Every candidate loop that does NOT get spliced is counted in exactly one drop_* bucket.
struct WallRibStats
{
    size_t loops_in          { 0 };  // mergeable candidates offered (incl. seed)
    size_t spliced           { 0 };  // loops actually welded into the walk
    size_t anchor_reused     { 0 };  // splices that reused the previous layer's column position
    size_t founded           { 0 };  // splices that needed a foundation pad on the layer below
    size_t drop_too_short    { 0 };  // loop perimeter too short to host a cut
    size_t drop_prim_far     { 0 };  // farther than max_link_length from the walk (Prim cap)
    size_t drop_obstacle     { 0 };  // every candidate link crosses another wall
    size_t drop_unsupported  { 0 };  // every candidate link is over true void (not even sparse below)

    void add(const WallRibStats &o) {
        loops_in += o.loops_in; spliced += o.spliced; anchor_reused += o.anchor_reused;
        founded += o.founded;
        drop_too_short += o.drop_too_short; drop_prim_far += o.drop_prim_far;
        drop_obstacle += o.drop_obstacle; drop_unsupported += o.drop_unsupported;
    }
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
    // would require more, the anchor is IGNORED and the rib relocates to the best legal spot
    // (starting a fresh, founded column there) - the old column simply ends, but the loop
    // stays connected.
    coord_t max_drift { std::numeric_limits<coord_t>::max() };
    // All wall loops of the island (including the non-mergeable ones): a rib link must not
    // cross any of them, or it would extrude straight over a hole / another wall.
    const Polygons *obstacles { nullptr };
    // Support of the previous layer; null = no support constraint (geometry-only use / tests).
    const WallRibSupport *support { nullptr };
    // Asked when a candidate link rests on nothing: may a foundation buttress be grown below
    // the link (walk-side point, loop-side point)? The caller dry-runs the buttress descent
    // (wall continuity, obstacles) and answers; accepted links are reported in founded_links
    // and the caller materializes the buttresses afterwards. Empty = no foundations possible.
    std::function<bool(const Point &, const Point &)> can_found;
    // Optional diagnostic counters (filled, never read, by plan_wall_ribs).
    WallRibStats *stats { nullptr };
};

// Plan the merge of `loops` (centerline polygons of one island's wall loops, same role/flow).
// `prev_anchors` (optional) biases each rib to the previous layer's position when the local
// geometry still allows it (self-standing columns). Placement cascade per loop: column anchor,
// then the closest approach, then a scan around the loop - first at SUPPORTED positions (rib
// stands on yesterday's rib/wall/solid); when none exists, the first position whose foundation
// buttress `can_found` grants is taken and reported in `founded_links` for the caller to grow.
// Only loops that are too short to host a cut, farther than max_link_length, whose every link
// would cross another wall, or for which no buttress is possible either, are reported in
// `unmerged` (indices into `loops`) and left out. Returns false when fewer than two loops end
// up merged.
bool plan_wall_ribs(const Polygons &loops, const WallRibParams &params,
                    const Points *prev_anchors, WallRibMerge &out, std::vector<size_t> &unmerged);

// Geometry-only convenience wrapper (unit tests): merge and return just the walk.
bool splice_wall_loops(const Polygons &loops, coord_t stagger, Polygon &merged, std::vector<size_t> &unmerged);

// Splice an out-and-back STUB into the closed `walk`: base at the walk position nearest
// `base_hint`, tip at `tip`, the two beads staggered by the cut of one `stagger` exactly like
// a rib's links (touching flanks, never a doubled centerline). This is one layer of a rib's
// foundation buttress - part of the wall, printed in the wall phase. The carved footprint is
// returned in `quad_out` (subtract it from the layer's fill surfaces). Fails on walks too
// short to host the cut.
bool splice_wall_stub(Polygon &walk, const Point &base_hint, const Point &tip, coord_t stagger, Polygon &quad_out);

// The planner's link support test, exported for the buttress descent: does the segment a-b
// rest on the given previous-layer material (with the same sub-bead gap tolerance)?
bool wall_rib_link_supported(const Point &a, const Point &b, coord_t stagger, const WallRibSupport &support);

} // namespace Slic3r
