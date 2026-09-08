#include "../ClipperUtils.hpp"
#include "../ExPolygon.hpp"
#include "../Surface.hpp"
#include "../VariableWidth.hpp"
#include "Arachne/WallToolPaths.hpp"

#include "FillConcentric.hpp"
#include <libslic3r/ShortestPath.hpp>
#include <unordered_map>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace Slic3r {

// Ginger continuous_path: the ring-merging splice (FillBase.cpp). Same construction as Cura's
// PolygonConnector - two links one bead apart, both rings cut and rejoined into one - plus the
// physical validity rules (containment in the island, no retracing an existing bead).
void single_path_splice_loops(Polylines &loops, double max_link_distance, double stagger, const Polygons *island, bool barrier_only);

// Fuse the concentric rings of ONE patch into a single loop. The rings come from Arachne as
// ThickPolylines (variable width); the splice works on plain Polylines, so the widths are carried
// over to the fused path by nearest source point - printing a merged top at a constant width would
// leave voids where the patch tapers and over-extrude where it widens.
static void splice_concentric_rings(ThickPolylines &thick, size_t first, const ExPolygon &expolygon,
                                    double spacing, coord_t min_spacing)
{
    if (thick.size() < first + 2)
        return;
    // Width samples from every source ring: one per segment endpoint.
    struct WSample { Point p; coordf_t w; };
    std::vector<WSample> samples;
    Polylines            rings;
    for (size_t i = first; i < thick.size(); ++ i) {
        const ThickPolyline &tp = thick[i];
        if (tp.points.size() < 2 || tp.width.size() + 2 < tp.points.size() * 2)
            return; // malformed: leave the patch untouched
        for (size_t k = 0; k + 1 < tp.points.size(); ++ k) {
            samples.push_back({ tp.points[k],     tp.width[2 * k] });
            samples.push_back({ tp.points[k + 1], tp.width[2 * k + 1] });
        }
        rings.emplace_back(tp.points);
    }
    if (samples.empty())
        return;
    const size_t rings_before = rings.size();
    size_t closed_in = 0;
    for (const Polyline &pl : rings)
        if (pl.points.size() > 3 && pl.points.front() == pl.points.back())
            ++ closed_in;
    // Link ceiling, in ring pitches. Arachne rings have a VARIABLE pitch (variable width), so where
    // the patch tapers two neighbouring rings sit further apart than one pitch and the merge is
    // refused. Tunable for the A/B; the splice itself caps a straight ring-ring link at 3 staggers,
    // so values above 3 do nothing. Cura hits the same wall: with connect_skin_polygons it still
    // leaves 3.04 pieces per patch (from 4.70), i.e. it fuses about a third.
    static const double link_mult = [] {
        const char *e = ::getenv("GINGER_TOPSPLICE_LINK");
        const double v = e == nullptr ? 1.5 : ::atof(e);
        return v > 0. ? v : 1.5;
    }();
    // The island is the same outward-offset outline Arachne walled, so the outermost ring
    // centerline lies inside it and a link along it passes the containment test.
    Polygons island = offset(expolygon, float(min_spacing) / 2.f);
    single_path_splice_loops(rings, scale_(link_mult * spacing), scale_(spacing), &island, false);
    if (::getenv("GINGER_TOPSPLICE_DEBUG") != nullptr)
        std::fprintf(stderr, "[TOPSPLICE] anelli=%zu (chiusi=%zu) -> %zu  link_max=%.2f interassi\n",
                     rings_before, closed_in, rings.size(), link_mult);
    if (rings.size() >= rings_before)
        return; // nothing merged: keep the Arachne widths exactly as they were
    // Nearest-sample lookup on a uniform grid (cell = one ring pitch). A linear scan here would be
    // O(samples * output points) per patch - the same quadratic shape that already cost 52s on the
    // stool once; with 20 rings of 200 points that is 16M distance tests per patch per layer.
    const coord_t cell = std::max<coord_t>(min_spacing, coord_t(SCALED_EPSILON) * 16);
    std::unordered_map<int64_t, std::vector<uint32_t>> grid;
    auto key_of = [cell](const Point &p) -> int64_t {
        const int64_t cx = int64_t(std::floor(double(p.x()) / double(cell)));
        const int64_t cy = int64_t(std::floor(double(p.y()) / double(cell)));
        return (cx << 32) ^ (cy & 0xffffffffLL);
    };
    for (uint32_t i = 0; i < uint32_t(samples.size()); ++ i)
        grid[key_of(samples[i].p)].push_back(i);
    auto width_at = [&samples, &grid, cell](const Point &p) -> coordf_t {
        const int64_t cx = int64_t(std::floor(double(p.x()) / double(cell)));
        const int64_t cy = int64_t(std::floor(double(p.y()) / double(cell)));
        double   best = std::numeric_limits<double>::max();
        coordf_t w    = samples.front().w;
        // Widening rings of cells: a source point sits in the same cell (distance 0) and a link
        // endpoint within one ring pitch, so ring 1 nearly always answers; the loop is a guard.
        for (int ring = 1; ring <= 4; ++ ring) {
            for (int64_t dx = -ring; dx <= ring; ++ dx)
                for (int64_t dy = -ring; dy <= ring; ++ dy) {
                    if (ring > 1 && std::llabs(dx) != ring && std::llabs(dy) != ring)
                        continue; // interior already visited
                    auto it = grid.find(((cx + dx) << 32) ^ ((cy + dy) & 0xffffffffLL));
                    if (it == grid.end())
                        continue;
                    for (uint32_t idx : it->second) {
                        const double d = (samples[idx].p - p).cast<double>().squaredNorm();
                        if (d < best) { best = d; w = samples[idx].w; }
                    }
                }
            if (best < std::numeric_limits<double>::max())
                break; // a hit in this ring of cells cannot be beaten by more than one cell
        }
        return w;
    };
    thick.erase(thick.begin() + first, thick.end());
    for (Polyline &pl : rings) {
        if (pl.points.size() < 2)
            continue;
        ThickPolyline tp;
        tp.points = std::move(pl.points);
        tp.width.reserve((tp.points.size() - 1) * 2);
        for (size_t k = 0; k + 1 < tp.points.size(); ++ k) {
            tp.width.emplace_back(width_at(tp.points[k]));
            tp.width.emplace_back(width_at(tp.points[k + 1]));
        }
        thick.emplace_back(std::move(tp));
    }
}

void FillConcentric::_fill_surface_single(
    const FillParams                &params, 
    unsigned int                     thickness_layers,
    const std::pair<float, Point>   &direction, 
    ExPolygon                        expolygon,
    Polylines                       &polylines_out)
{
    // no rotation is supported for this infill pattern
    BoundingBox bounding_box = expolygon.contour.bounding_box();
    
    coord_t min_spacing = scale_(this->spacing) * params.multiline;
    coord_t distance = coord_t(min_spacing / params.density);
    
    if (params.density > 0.9999f && !params.dont_adjust) {
        distance = this->_adjust_solid_spacing(bounding_box.size()(0), distance);
        this->spacing = unscale<double>(distance);
    }

    // Contract surface polygon by half line width to avoid excesive overlap with perimeter
    ExPolygons contracted = offset_ex(expolygon, -float(scale_(0.5 * (params.multiline - 1) * this->spacing )));

    Polygons loops = to_polygons(contracted);

    ExPolygons last { std::move(contracted) };
    while (! last.empty()) {
        last = offset2_ex(last, -(distance + min_spacing/2), +min_spacing/2);
        append(loops, to_polygons(last));
    }

    // generate paths from the outermost to the innermost, to avoid
    // adhesion problems of the first central tiny loops
    loops = union_pt_chained_outside_in(loops);
    
    // split paths using a nearest neighbor search
    size_t iPathFirst = polylines_out.size();
    Point last_pos(0, 0);
    for (const Polygon &loop : loops) {
        polylines_out.emplace_back(loop.split_at_index(last_pos.nearest_point_index(loop.points)));
        last_pos = polylines_out.back().last_point();
    }

    // Apply multiline offset if needed
    multiline_fill(polylines_out, params, spacing);

    // clip the paths to prevent the extruder from getting exactly on the first point of the loop
    // Keep valid paths only.
    size_t j = iPathFirst;
    for (size_t i = iPathFirst; i < polylines_out.size(); ++ i) {
        // Ginger continuous_path: keep the ring CLOSED (split_at_index duplicates the split point
        // at both ends). A closed ring becomes an ExtrusionLoop downstream, which the G-code router
        // may enter anywhere - so its exit coincides with its entry and the return travel that a
        // clipped, open ring pays (the distance between its two ends) disappears. The seam gap is
        // applied at emission time by extrude_loop, so not clipping here never re-extrudes a seam.
        if (! params.connect_polygons)
            polylines_out[i].clip_end(this->loop_clipping);
        if (polylines_out[i].is_valid()) {
            if (j < i)
                polylines_out[j] = std::move(polylines_out[i]);
            ++ j;
        }
    }
    if (j < polylines_out.size())
        polylines_out.erase(polylines_out.begin() + j, polylines_out.end());
    //TODO: return ExtrusionLoop objects to get better chained paths,
    // otherwise the outermost loop starts at the closest point to (0, 0).
    // We want the loops to be split inside the G-code generator to get optimum path planning.
}

void FillConcentric::_fill_surface_single(const FillParams& params,
    unsigned int                   thickness_layers,
    const std::pair<float, Point>& direction,
    ExPolygon                      expolygon,
    ThickPolylines& thick_polylines_out)
{
    assert(params.use_arachne);
    assert(this->print_config != nullptr && this->print_object_config != nullptr);

    // no rotation is supported for this infill pattern
    Point   bbox_size = expolygon.contour.bounding_box().size();
    coord_t min_spacing = scaled<coord_t>(this->spacing);

    if (params.density > 0.9999f && !params.dont_adjust) {
        coord_t                loops_count = std::max(bbox_size.x(), bbox_size.y()) / min_spacing + 1;
        Polygons               polygons = offset(expolygon, float(min_spacing) / 2.f);

        double min_nozzle_diameter = *std::min_element(print_config->nozzle_diameter.values.begin(), print_config->nozzle_diameter.values.end());
        Arachne::WallToolPathsParams input_params;
        input_params.min_bead_width = 0.85 * min_nozzle_diameter;
        input_params.min_feature_size = 0.25 * min_nozzle_diameter;
        input_params.wall_transition_length = 1.0 * min_nozzle_diameter;
        input_params.wall_transition_angle = 10;
        input_params.wall_transition_filter_deviation = 0.25 * min_nozzle_diameter;
        input_params.wall_distribution_count = 1;

        Arachne::WallToolPaths wallToolPaths(polygons, min_spacing, min_spacing, loops_count, 0, params.layer_height, input_params);

        std::vector<Arachne::VariableWidthLines>    loops = wallToolPaths.getToolPaths();
        std::vector<const Arachne::ExtrusionLine*> all_extrusions;
        for (Arachne::VariableWidthLines& loop : loops) {
            if (loop.empty())
                continue;
            for (const Arachne::ExtrusionLine& wall : loop)
                all_extrusions.emplace_back(&wall);
        }

        // Split paths using a nearest neighbor search.
        size_t firts_poly_idx = thick_polylines_out.size();
        Point  last_pos(0, 0);
        for (const Arachne::ExtrusionLine* extrusion : all_extrusions) {
            if (extrusion->empty())
                continue;

            ThickPolyline thick_polyline = Arachne::to_thick_polyline(*extrusion);
            if (extrusion->is_closed)
                thick_polyline.start_at_index(last_pos.nearest_point_index(thick_polyline.points));
            thick_polylines_out.emplace_back(std::move(thick_polyline));
            last_pos = thick_polylines_out.back().last_point();
        }

        // clip the paths to prevent the extruder from getting exactly on the first point of the loop
        // Keep valid paths only.
        size_t j = firts_poly_idx;
        for (size_t i = firts_poly_idx; i < thick_polylines_out.size(); ++i) {
            // Ginger continuous_path: see the polyline branch above - a closed thick polyline makes
            // variable_width() emit an ExtrusionLoop instead of loose ExtrusionPaths.
            if (! params.connect_polygons)
                thick_polylines_out[i].clip_end(this->loop_clipping);
            if (thick_polylines_out[i].is_valid()) {
                if (j < i)
                    thick_polylines_out[j] = std::move(thick_polylines_out[i]);
                ++j;
            }
        }
        if (j < thick_polylines_out.size())
            thick_polylines_out.erase(thick_polylines_out.begin() + int(j), thick_polylines_out.end());

        // Ginger continuous_path: N closed rings are N loop units for the router, i.e. N-1 ring-to-ring
        // hops. Cura pairs concentric with connect_skin_polygons for exactly this reason - the pattern
        // alone is not the point, fusing its rings into one path is. Measured on plate 3 before this:
        // the closed rings cut Top->Sparse by 12.7 m but Top->Top grew by 12.3 m over 5021 hops.
        if (params.connect_polygons)
            splice_concentric_rings(thick_polylines_out, firts_poly_idx, expolygon, this->spacing, min_spacing);

        reorder_by_shortest_traverse(thick_polylines_out);
    }
    else {
        Polylines polylines;
        this->_fill_surface_single(params, thickness_layers, direction, expolygon, polylines);
        append(thick_polylines_out, to_thick_polylines(std::move(polylines), min_spacing));
    }
}

} // namespace Slic3r
