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

#include "Polygon.hpp"

namespace Slic3r {

// Merge `loops` (centerline polygons of the wall loops of one island) into a single closed
// walk. `stagger` is the link separation (use the extrusion line width, scaled). The seed
// (walk start) is the loop with the largest absolute area (the outer wall). Loops whose
// perimeter is too short to host a cut (< 4 * stagger) are left out and reported in
// `unmerged` (indices into `loops`). Returns false (and leaves `merged` empty) when fewer
// than two loops are mergeable.
bool splice_wall_loops(const Polygons &loops, coord_t stagger, Polygon &merged, std::vector<size_t> &unmerged);

} // namespace Slic3r
