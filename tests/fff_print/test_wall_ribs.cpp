#include <catch2/catch.hpp>

#include "libslic3r/WallRibs.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/Line.hpp"

using namespace Slic3r;

// The rib splice must never retrace: no two segments of the merged walk may run on top of
// each other in opposite directions (that is doubled extrusion, the hard no-go on pellet).
// Same-direction coincidence would be a bug too, so check both.
static void require_no_coincident_segments(const Polygon &poly, coord_t line_width)
{
    const Lines  lines   = poly.lines();
    const double overlap = 0.25 * double(line_width); // centerlines closer than this = same axis
    for (size_t i = 0; i < lines.size(); ++ i)
        for (size_t j = i + 1; j < lines.size(); ++ j) {
            const Line &l1 = lines[i];
            const Line &l2 = lines[j];
            if (l1.length() < SCALED_EPSILON || l2.length() < SCALED_EPSILON)
                continue;
            const Vec2d d1 = (l1.b - l1.a).cast<double>().normalized();
            const Vec2d d2 = (l2.b - l2.a).cast<double>().normalized();
            if (std::abs(d1.dot(d2)) < 0.99)
                continue; // not parallel
            // Parallel: reject if they overlap along their direction while sharing the axis.
            const Vec2d n(-d1.y(), d1.x());
            const double dist_axis = std::abs(n.dot((l2.a - l1.a).cast<double>()));
            if (dist_axis > overlap)
                continue;
            const double t0 = d1.dot((l2.a - l1.a).cast<double>());
            const double t1 = d1.dot((l2.b - l1.a).cast<double>());
            const double len1 = l1.length();
            const double lo = std::min(t0, t1), hi = std::max(t0, t1);
            const double shared = std::min(hi, len1) - std::max(lo, 0.);
            INFO("segments " << i << " and " << j << " share " << unscale<double>(coord_t(shared)) << " mm of axis");
            REQUIRE(shared < double(line_width)); // touching at a junction is fine, running together is not
        }
}

static Polygon square_ccw(double cx, double cy, double half)
{
    return Polygon({ Point::new_scale(cx - half, cy - half), Point::new_scale(cx + half, cy - half),
                     Point::new_scale(cx + half, cy + half), Point::new_scale(cx - half, cy + half) });
}

TEST_CASE("WallRibs: annulus (outer + one hole) becomes one closed walk", "[WallRibs]")
{
    const coord_t lw = scale_(3.2);
    Polygon outer = square_ccw(0., 0., 100.);
    Polygon hole  = square_ccw(0., 0., 60.);
    hole.reverse(); // hole winding (CW), as PerimeterGenerator emits it

    Polygons loops { outer, hole };
    Polygon  merged;
    std::vector<size_t> unmerged;
    REQUIRE(splice_wall_loops(loops, lw, merged, unmerged));
    REQUIRE(unmerged.empty());
    REQUIRE(merged.size() >= loops[0].size() + loops[1].size());

    // Total length: both perimeters, minus the two cuts (one stagger each), plus the two links
    // crossing the 40mm gap (each >= gap, <= gap + stagger by construction).
    const double gap      = scale_(40.);
    const double expected = outer.length() + hole.length() + 2. * gap;
    REQUIRE_THAT(merged.length(), Catch::Matchers::WithinRel(expected, 0.02));

    require_no_coincident_segments(merged, lw);
}

TEST_CASE("WallRibs: three holes all get spliced in (Prim over the growing walk)", "[WallRibs]")
{
    const coord_t lw = scale_(3.2);
    Polygon outer = square_ccw(0., 0., 150.);
    Polygon h1 = square_ccw(-80., -80., 25.); h1.reverse();
    Polygon h2 = square_ccw( 80., -80., 25.); h2.reverse();
    Polygon h3 = square_ccw(  0.,  80., 25.); h3.reverse();

    Polygons loops { outer, h1, h2, h3 };
    Polygon  merged;
    std::vector<size_t> unmerged;
    REQUIRE(splice_wall_loops(loops, lw, merged, unmerged));
    REQUIRE(unmerged.empty());

    // Every loop's material is in the walk (each loses only its one-stagger cut).
    double loops_len = 0.;
    for (const Polygon &l : loops)
        loops_len += l.length();
    REQUIRE(merged.length() > loops_len - 4.5 * double(lw)); // 4 cuts remove < ~4.5 line widths total
    require_no_coincident_segments(merged, lw);
}

TEST_CASE("WallRibs: two side-by-side loops (same winding) do not cross links", "[WallRibs]")
{
    // Same-winding loops run anti-parallel where they face each other; the splice must handle
    // the tangent check and come out clean.
    const coord_t lw = scale_(3.2);
    Polygons loops { square_ccw(-60., 0., 40.), square_ccw(60., 0., 40.) };
    Polygon  merged;
    std::vector<size_t> unmerged;
    REQUIRE(splice_wall_loops(loops, lw, merged, unmerged));
    REQUIRE(unmerged.empty());
    require_no_coincident_segments(merged, lw);
}

TEST_CASE("WallRibs: a loop too small to host a cut is reported unmerged", "[WallRibs]")
{
    const coord_t lw = scale_(3.2);
    Polygon tiny = square_ccw(0., 0., 2.); // 16mm perimeter < 4 * 3.2mm... borderline: use 1.5
    tiny = square_ccw(0., 0., 1.5);        // 12mm perimeter < 12.8mm -> not mergeable
    tiny.reverse();
    Polygons loops { square_ccw(0., 0., 100.), tiny, square_ccw(50., 50., 20.) };
    loops[2].reverse();
    Polygon  merged;
    std::vector<size_t> unmerged;
    REQUIRE(splice_wall_loops(loops, lw, merged, unmerged));
    REQUIRE(unmerged == std::vector<size_t>{ 1 });
    require_no_coincident_segments(merged, lw);
}

TEST_CASE("WallRibs: fewer than two mergeable loops refuses", "[WallRibs]")
{
    Polygon  merged;
    std::vector<size_t> unmerged;
    REQUIRE_FALSE(splice_wall_loops({ square_ccw(0., 0., 50.) }, scale_(3.2), merged, unmerged));
    REQUIRE(merged.empty());
}

TEST_CASE("WallRibs: plan produces corridors covering the links and stable anchors across layers", "[WallRibs]")
{
    const coord_t lw = scale_(3.2);
    Polygon outer = square_ccw(0., 0., 100.);
    Polygon hole  = square_ccw(0., 0., 60.);
    hole.reverse();

    WallRibMerge        plan1;
    std::vector<size_t> unmerged;
    WallRibParams params;
    params.stagger         = lw;
    params.corridor_offset = coord_t(scale_(0.5 * 3.2));
    REQUIRE(plan_wall_ribs({ outer, hole }, params, nullptr, plan1, unmerged));
    REQUIRE(plan1.anchors.size() == 1);
    REQUIRE(! plan1.corridors.empty());
    // The anchor (link midpoint) sits inside the corridor - that is the area carved out of the
    // fill surfaces so nothing extrudes across the rib.
    bool anchor_covered = false;
    for (const Polygon &c : plan1.corridors)
        if (c.contains(plan1.anchors.front()))
            anchor_covered = true;
    REQUIRE(anchor_covered);
    REQUIRE(plan1.loop_keys.size() == 2);

    // Next layer: same part, geometry jittered by half a bead. With the previous anchors the rib
    // must stay where it was (self-standing column), not jump to a new closest approach.
    auto jitter = [](Polygon p, double dx, double dy) {
        p.translate(Point::new_scale(dx, dy));
        return p;
    };
    WallRibMerge plan2;
    Points       prev = plan1.anchors;
    REQUIRE(plan_wall_ribs({ jitter(outer, 1.5, -1.0), jitter(hole, 1.5, -1.0) }, params, &prev, plan2, unmerged));
    REQUIRE(plan2.anchors.size() == 1);
    const double drift = (plan2.anchors.front() - plan1.anchors.front()).cast<double>().norm();
    INFO("rib drift between layers: " << unscale<double>(coord_t(drift)) << " mm");
    REQUIRE(drift < 2. * double(lw)); // follows the geometry, does not teleport
    require_no_coincident_segments(plan2.merged, lw);
}

TEST_CASE("WallRibs: a loop farther than max_link_length stays unmerged", "[WallRibs]")
{
    // Outer plate with one near hole (gap ~15mm) and one far hole (gap ~60mm from anything).
    Polygon outer = square_ccw(0., 0., 100.);
    Polygon near_hole = square_ccw(-70., 0., 15.);  near_hole.reverse(); // gap to outer ~15mm
    Polygon far_hole  = square_ccw(25., 0., 10.);   far_hole.reverse();  // >=45mm from both

    WallRibParams params;
    params.stagger         = scale_(3.2);
    params.corridor_offset = coord_t(scale_(1.6));
    params.max_link_length = coord_t(scale_(20.));
    WallRibMerge        plan;
    std::vector<size_t> unmerged;
    REQUIRE(plan_wall_ribs({ outer, near_hole, far_hole }, params, nullptr, plan, unmerged));
    REQUIRE(plan.loop_keys.size() == 2);              // outer + near hole only
    REQUIRE(unmerged == std::vector<size_t>{ 2 });    // the far hole is left to the seam chain
    REQUIRE(plan.anchors.size() == 1);
}

TEST_CASE("WallRibs: a link that would cross another wall is rejected", "[WallRibs]")
{
    // Outer plate; a big EXCLUDED loop (obstacle, e.g. a multi-path overhang wall) sits exactly
    // between the outer wall and the only mergeable hole: the direct link would extrude straight
    // across it, so the merge must be refused.
    Polygon outer    = square_ccw(0., 0., 100.);
    Polygon hole     = square_ccw(0., 0., 20.);  hole.reverse();     // centered, gap 40mm to obstacle ring
    Polygon obstacle = square_ccw(0., 0., 60.);                      // ring between them (not in loops)

    WallRibParams params;
    params.stagger         = scale_(3.2);
    params.corridor_offset = coord_t(scale_(1.6));
    Polygons obstacles { outer, hole, obstacle };
    params.obstacles = &obstacles;
    WallRibMerge        plan;
    std::vector<size_t> unmerged;
    REQUIRE_FALSE(plan_wall_ribs({ outer, hole }, params, nullptr, plan, unmerged));
}

TEST_CASE("WallRibs: drift beyond the budget ends the column instead of staircasing", "[WallRibs]")
{
    const coord_t lw = scale_(3.2);
    Polygon outer = square_ccw(0., 0., 100.);
    Polygon hole  = square_ccw(0., 0., 60.);
    hole.reverse();

    WallRibParams params;
    params.stagger         = lw;
    params.corridor_offset = coord_t(scale_(1.6));
    params.max_drift       = coord_t(scale_(1.5)); // one layer height
    WallRibMerge        plan1;
    std::vector<size_t> unmerged;
    REQUIRE(plan_wall_ribs({ outer, hole }, params, nullptr, plan1, unmerged));

    // Next layer the geometry rotates hard: following the old anchor would need a ~20mm jump.
    Polygon outer2 = outer, hole2 = hole;
    outer2.rotate(0.35); // ~20 deg about origin: the gap positions move by tens of mm
    hole2.rotate(0.35);
    Points prev = plan1.anchors;
    WallRibMerge plan2;
    const bool ok = plan_wall_ribs({ outer2, hole2 }, params, &prev, plan2, unmerged);
    if (ok) {
        // If a rib was still placed, it must NOT be a clamped-anchor staircase: either it reused
        // a genuinely close anchor or it started fresh - never a mid-air step near the old spot.
        for (const Point &a : plan2.anchors) {
            const double d = (a - plan1.anchors.front()).cast<double>().norm();
            INFO("anchor moved " << unscale<double>(coord_t(d)) << " mm");
            REQUIRE((d <= double(params.max_drift) + SCALED_EPSILON || d > 10. * double(lw)));
        }
    }
}

TEST_CASE("WallRibs: support test - rib only where the previous layer has material", "[WallRibs]")
{
    const coord_t lw = scale_(3.2);
    Polygon outer = square_ccw(0., 0., 100.);
    Polygon hole  = square_ccw(0., 0., 60.);
    hole.reverse();

    WallRibParams params;
    params.stagger         = lw;
    params.corridor_offset = coord_t(scale_(1.6));
    WallRibSupport support; // not first layer, nothing below
    Polygons   no_corridors;
    Polygons   no_walls;
    ExPolygons solid;
    support.prev_corridors = &no_corridors;
    support.prev_walls     = &no_walls;
    support.prev_solid     = &solid;
    params.support         = &support;

    WallRibMerge        plan;
    std::vector<size_t> unmerged;
    // Nothing below anywhere: the merge must be refused (a rib would hang in the air).
    REQUIRE_FALSE(plan_wall_ribs({ outer, hole }, params, nullptr, plan, unmerged));

    // A solid pad below ONLY on the right side gap: the scan must find it and put the rib there,
    // even though other positions are geometrically equivalent.
    solid.emplace_back(ExPolygon(square_ccw(80., 0., 22.)));
    REQUIRE(plan_wall_ribs({ outer, hole }, params, nullptr, plan, unmerged));
    REQUIRE(plan.anchors.size() == 1);
    const Point anchor = plan.anchors.front();
    INFO("anchor at " << unscale<double>(anchor.x()) << "," << unscale<double>(anchor.y()));
    REQUIRE(unscale<double>(anchor.x()) > 55.);              // in the right-side gap
    REQUIRE(std::abs(unscale<double>(anchor.y())) < 25.);    // on the pad, not far away
    require_no_coincident_segments(plan.merged, lw);

    // First layer: everything sits on the bed, no constraint.
    solid.clear();
    support.first_layer = true;
    REQUIRE(plan_wall_ribs({ outer, hole }, params, nullptr, plan, unmerged));
}
