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

static Polygon circle_ccw(double cx, double cy, double r)
{
    Points pts;
    for (int i = 0; i < 32; ++ i) {
        const double a = 2. * PI * i / 32.;
        pts.emplace_back(Point::new_scale(cx + r * std::cos(a), cy + r * std::sin(a)));
    }
    return Polygon(pts);
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
    auto         prev = plan1.links;
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
    // Outer plate; a big EXTRA bead (e.g. a thin-wall ring the planner cannot see as a
    // candidate) sits exactly between the outer wall and the only mergeable hole: the
    // direct link would extrude straight across it, so the merge must be refused. The
    // candidates themselves need not be passed - the planner tracks walk and loops itself.
    Polygon outer    = square_ccw(0., 0., 100.);
    Polygon hole     = square_ccw(0., 0., 20.);  hole.reverse();     // centered, gap 40mm to obstacle ring
    Polygon obstacle = square_ccw(0., 0., 60.);                      // ring between them (not in loops)

    WallRibParams params;
    params.stagger         = scale_(3.2);
    params.corridor_offset = coord_t(scale_(1.6));
    Lines obstacle_lines = obstacle.lines();
    params.extra_obstacles = &obstacle_lines;
    WallRibMerge        plan;
    std::vector<size_t> unmerged;
    REQUIRE_FALSE(plan_wall_ribs({ outer, hole }, params, nullptr, plan, unmerged));
}

TEST_CASE("WallRibs: segment conflict rules - crossing, attach contact, interasse", "[WallRibs]")
{
    const coord_t lw = scale_(3.2);
    const Lines   none;
    const Point   a = Point::new_scale(0., 0.);
    const Point   b = Point::new_scale(30., 0.);

    // Crossing a foreign bead is illegal anywhere - even right next to an endpoint (the old
    // distance-based exemption let a third wall passing near the attach be crossed).
    Lines mid_cross  { Line(Point::new_scale(15., -10.), Point::new_scale(15., 10.)) };
    Lines near_cross { Line(Point::new_scale(1., -10.),  Point::new_scale(1., 10.)) };
    CHECK(rib_segment_conflicts(a, b, lw, none, none, mid_cross));
    CHECK(rib_segment_conflicts(a, b, lw, none, none, near_cross));

    // Crossing the OWN curves is the attach contact: legal within one stagger of the
    // respective endpoint, illegal farther (that is crossing the walk somewhere else).
    CHECK_FALSE(rib_segment_conflicts(a, b, lw, near_cross, none, none)); // own_a at 1mm from a
    CHECK(rib_segment_conflicts(a, b, lw, mid_cross, none, none));        // own_a at 15mm from a
    Lines near_b { Line(Point::new_scale(29., -10.), Point::new_scale(29., 10.)) };
    CHECK_FALSE(rib_segment_conflicts(a, b, lw, none, near_b, none));     // own_b at 1mm from b

    // Interasse (the pellet hard rule): a bead running PARALLEL closer than one width for an
    // extended stretch is doubled extrusion - invisible to segment intersection, caught by
    // the proximity run. At one full width the flanks touch and fuse: legal (the rib's own
    // two links sit at exactly one stagger).
    Lines riding { Line(Point::new_scale(5., 2.), Point::new_scale(25., 2.)) };   // 2mm off-axis, 20mm long
    Lines fused  { Line(Point::new_scale(5., 3.2), Point::new_scale(25., 3.2)) }; // exactly one width
    CHECK(rib_segment_conflicts(a, b, lw, none, none, riding));
    CHECK_FALSE(rib_segment_conflicts(a, b, lw, none, none, fused));
    // Riding the OWN walk far from the attach is equally illegal (a later link along an
    // earlier rib); near the attach it is the legitimate wall contact.
    CHECK(rib_segment_conflicts(a, b, lw, riding, none, none));
    Lines own_near_attach { Line(Point::new_scale(0., 2.), Point::new_scale(3., 2.)) };
    CHECK_FALSE(rib_segment_conflicts(a, b, lw, own_near_attach, none, none));

    // A quick transversal pass-by (one close sample, no shared axis) is not riding.
    Lines pass_by { Line(Point::new_scale(15., 2.), Point::new_scale(18., 12.)) };
    CHECK_FALSE(rib_segment_conflicts(a, b, lw, none, none, pass_by));
}

TEST_CASE("WallRibs: drift beyond the budget relocates the rib instead of staircasing or giving up", "[WallRibs]")
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

    // Next layer the geometry rotates hard: following the old anchor would need a jump far
    // beyond the drift budget. The old column must NOT be clamp-followed (no staircase), but
    // the loop MUST still be connected - the rib relocates and a fresh column starts there.
    Polygon outer2 = outer, hole2 = hole;
    outer2.rotate(0.35); // ~20 deg about origin: the gap positions move by tens of mm
    hole2.rotate(0.35);
    auto prev = plan1.links;
    WallRibMerge plan2;
    REQUIRE(plan_wall_ribs({ outer2, hole2 }, params, &prev, plan2, unmerged));
    REQUIRE(unmerged.empty());
    REQUIRE(plan2.anchors.size() == 1);
    const double d = (plan2.anchors.front() - plan1.anchors.front()).cast<double>().norm();
    INFO("anchor moved " << unscale<double>(coord_t(d)) << " mm");
    // A true relocation, not a budget-sized step pretending the column continued.
    REQUIRE(d > double(params.max_drift));
    require_no_coincident_segments(plan2.merged, lw);
}

TEST_CASE("WallRibs: a neighbouring column's anchor cannot lock a loop out of merging", "[WallRibs]")
{
    // prev_anchors holds ONLY an anchor lying on the outer wall far from the hole's gap
    // (e.g. the surviving column of some other feature while this hole's own column is gone).
    // That anchor is not in this pair's gap, so it must be ignored - the hole merges at its
    // optimal position. The old guards matched it, saw a drift over budget and dropped the
    // hole outright, every layer, forever.
    const coord_t lw = scale_(3.2);
    Polygon outer = square_ccw(0., 0., 100.);
    Polygon hole  = square_ccw(0., 0., 20.);
    hole.reverse();

    WallRibParams params;
    params.stagger         = lw;
    params.corridor_offset = coord_t(scale_(1.6));
    params.max_drift       = coord_t(scale_(1.5));
    // A foreign column's LINK, both endpoints on the outer wall, nowhere near this pair's gap.
    std::vector<std::pair<Point, Point>> prev { { Point::new_scale(100., 50.), Point::new_scale(100., 46.8) } };
    WallRibMerge        plan;
    std::vector<size_t> unmerged;
    REQUIRE(plan_wall_ribs({ outer, hole }, params, &prev, plan, unmerged));
    REQUIRE(unmerged.empty());
    REQUIRE(plan.loop_keys.size() == 2);
    require_no_coincident_segments(plan.merged, lw);
}

TEST_CASE("WallRibs: free re-founding - a dead column must not capture the rib when real material is everywhere", "[WallRibs]")
{
    // Miniature of the stool hub: at layer N-1 a feature (the engraved text) provided loops in
    // the middle and rib columns ended on them; at layer N the feature is gone all at once.
    // The near-dead-column preference exists to buy SELF-support (standing on yesterday's rib),
    // so when the region below is plain solid - the rib can stand anywhere - the corpse must
    // not drag the new rib into a long chord: the loop re-founds at its closest approach.
    const coord_t lw = scale_(3.2);
    Polygon outer = square_ccw(0., 0., 50.);
    // Two vertical bars facing each other across a 20 mm mouth at the origin (the hub in
    // miniature); each bar's tip toward the outer wall is only 5 mm away - the H the operator
    // expects. For a cursor on a facing tip the OTHER bar is genuinely the nearest merged
    // geometry, so the 20 mm tip-to-tip chord is a real scan candidate (like the hub chord).
    Polygon bar_top({ Point::new_scale(-5., 10.), Point::new_scale(5., 10.),
                      Point::new_scale(5., 45.),  Point::new_scale(-5., 45.) });
    Polygon bar_bottom({ Point::new_scale(-5., -45.), Point::new_scale(5., -45.),
                         Point::new_scale(5., -10.),  Point::new_scale(-5., -10.) });
    bar_top.reverse(); bar_bottom.reverse();

    WallRibParams params;
    params.stagger         = lw;
    params.corridor_offset = coord_t(scale_(1.6));
    params.max_drift       = coord_t(scale_(1.5)); // real-world budget: without it the corpse
                                                   // reprojects as a live column from anywhere
    // The corpse: yesterday's link onto the now-gone middle feature, midpoint at the origin -
    // right where the tip-to-tip chord passes.
    std::vector<std::pair<Point, Point>> prev { { Point::new_scale(-2., 0.), Point::new_scale(2., 0.) } };
    // Real material everywhere below: solid fill covering the whole part.
    ExPolygons solid { ExPolygon(square_ccw(0., 0., 50.)) };
    Polygons   none;
    WallRibSupport support;
    support.prev_corridors = &none;
    support.prev_walls     = &none;
    support.prev_solid     = &solid;
    params.support         = &support;

    WallRibMerge        plan;
    std::vector<size_t> unmerged;
    REQUIRE(plan_wall_ribs({ outer, bar_bottom, bar_top }, params, &prev, plan, unmerged));
    REQUIRE(unmerged.empty());
    REQUIRE(plan.links.size() == 2);
    // Both ribs must be the 5 mm hops to the outer wall; the 20 mm chord through the corpse
    // (near-dead order) must lose to them, because everything stands on solid anyway.
    for (const auto &link : plan.links) {
        const double len = (link.second - link.first).cast<double>().norm();
        UNSCOPED_INFO("link (" << unscale<double>(link.first.x()) << "," << unscale<double>(link.first.y())
                      << ") -> (" << unscale<double>(link.second.x()) << "," << unscale<double>(link.second.y())
                      << ") length " << unscale<double>(coord_t(len)) << " mm");
        CHECK(len < scale_(10.));
    }
    require_no_coincident_segments(plan.merged, lw);
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
    // even though other positions are geometrically equivalent. Standing on real solid, the rib
    // must not ask for a foundation.
    solid.emplace_back(ExPolygon(square_ccw(80., 0., 22.)));
    REQUIRE(plan_wall_ribs({ outer, hole }, params, nullptr, plan, unmerged));
    REQUIRE(plan.anchors.size() == 1);
    const Point anchor = plan.anchors.front();
    INFO("anchor at " << unscale<double>(anchor.x()) << "," << unscale<double>(anchor.y()));
    REQUIRE(unscale<double>(anchor.x()) > 55.);              // in the right-side gap
    REQUIRE(std::abs(unscale<double>(anchor.y())) < 25.);    // on the pad, not far away
    REQUIRE(plan.founded_links.empty());
    require_no_coincident_segments(plan.merged, lw);

    // First layer: everything sits on the bed, no constraint.
    solid.clear();
    support.first_layer = true;
    REQUIRE(plan_wall_ribs({ outer, hole }, params, nullptr, plan, unmerged));
}

TEST_CASE("WallRibs: a rib on nothing is placed only when a foundation buttress is possible", "[WallRibs]")
{
    const coord_t lw = scale_(3.2);
    Polygon outer = square_ccw(0., 0., 100.);
    Polygon hole  = square_ccw(0., 0., 60.);
    hole.reverse();

    WallRibParams params;
    params.stagger         = lw;
    params.corridor_offset = coord_t(scale_(1.6));
    WallRibSupport support; // not first layer, nothing below at all
    Polygons   no_corridors;
    Polygons   no_walls;
    ExPolygons no_solid;
    support.prev_corridors = &no_corridors;
    support.prev_walls     = &no_walls;
    support.prev_solid     = &no_solid;
    params.support         = &support;

    WallRibMerge        plan;
    std::vector<size_t> unmerged;
    // No way to found anything (no can_found callback): refuse rather than print in the air.
    REQUIRE_FALSE(plan_wall_ribs({ outer, hole }, params, nullptr, plan, unmerged));

    // The caller can grow a foundation buttress below (wall stubs shrinking layer by layer,
    // lightning-style): the rib is placed and its link is reported for founding - never in
    // the air, never dropped.
    params.can_found = [](const Point &, const Point &) { return true; };
    REQUIRE(plan_wall_ribs({ outer, hole }, params, nullptr, plan, unmerged));
    REQUIRE(plan.loop_keys.size() == 2);
    REQUIRE(plan.founded_links.size() == 1);
    require_no_coincident_segments(plan.merged, lw);
}

TEST_CASE("WallRibs: a column standing on its own corridor continues without a foundation", "[WallRibs]")
{
    const coord_t lw = scale_(3.2);
    Polygon outer = square_ccw(0., 0., 100.);
    Polygon hole  = square_ccw(0., 0., 60.);
    hole.reverse();

    WallRibParams params;
    params.stagger         = lw;
    params.corridor_offset = coord_t(scale_(1.6));
    params.max_drift       = coord_t(scale_(1.5));
    WallRibMerge        plan1;
    std::vector<size_t> unmerged;
    REQUIRE(plan_wall_ribs({ outer, hole }, params, nullptr, plan1, unmerged)); // layer N: unconstrained
    REQUIRE(plan1.anchors.size() == 1);
    REQUIRE(! plan1.corridors.empty());

    // Layer N+1: the ONLY material below is yesterday's rib corridor. The column must continue
    // exactly there (anchor reuse, drift within budget) and needs no foundation - this is the
    // self-standing rib column that carries itself through hundreds of sparse layers.
    WallRibSupport support;
    Polygons   no_walls;
    ExPolygons no_solid;
    support.prev_corridors = &plan1.corridors;
    support.prev_walls     = &no_walls;
    support.prev_solid     = &no_solid;
    params.support         = &support;
    WallRibMerge plan2;
    REQUIRE(plan_wall_ribs({ outer, hole }, params, &plan1.links, plan2, unmerged));
    REQUIRE(plan2.anchors.size() == 1);
    const double drift = (plan2.anchors.front() - plan1.anchors.front()).cast<double>().norm();
    INFO("column drift: " << unscale<double>(coord_t(drift)) << " mm");
    REQUIRE(drift <= double(params.max_drift) + SCALED_EPSILON);
    REQUIRE(plan2.founded_links.empty());
}

TEST_CASE("WallRibs: on smooth walls the rib straddles its connection axis symmetrically", "[WallRibs]")
{
    // A round boss ring, like the real screw bosses: the two link beads must sit at +/- half
    // a bead around the connection axis. The old one-sided cut put one bead ON the axis and
    // the other a full bead off it - the rib attached visibly eccentric to the hole.
    const coord_t lw = scale_(3.2);
    Polygon outer = circle_ccw(0., 0., 80.);
    Polygon hole  = circle_ccw(0., 0., 50.);
    hole.reverse();

    WallRibParams params;
    params.stagger         = lw;
    params.corridor_offset = coord_t(scale_(1.6));
    WallRibMerge        plan;
    std::vector<size_t> unmerged;
    REQUIRE(plan_wall_ribs({ outer, hole }, params, nullptr, plan, unmerged));
    REQUIRE(plan.anchors.size() == 1);

    Points mids;
    for (const Line &l : plan.merged.lines()) {
        if (l.length() < scale_(25.))
            continue; // the links cross the 30mm ring gap; wall fragments lie on one loop only
        const bool a_outer = (outer.point_projection(l.a) - l.a).cast<double>().norm() < scale_(0.1);
        const bool b_outer = (outer.point_projection(l.b) - l.b).cast<double>().norm() < scale_(0.1);
        const bool a_hole  = (hole.point_projection(l.a) - l.a).cast<double>().norm() < scale_(0.1);
        const bool b_hole  = (hole.point_projection(l.b) - l.b).cast<double>().norm() < scale_(0.1);
        if ((a_outer && b_hole) || (a_hole && b_outer))
            mids.emplace_back((l.a + l.b) / 2);
    }
    REQUIRE(mids.size() == 2);
    const Point  mean = (mids[0] + mids[1]) / 2;
    const double ecc  = (mean - plan.anchors.front()).cast<double>().norm();
    INFO("rib eccentricity vs connection axis: " << unscale<double>(coord_t(ecc)) << " mm");
    REQUIRE(ecc < 0.25 * double(lw)); // was ~half a bead with the one-sided cut
    require_no_coincident_segments(plan.merged, lw);
}

TEST_CASE("WallRibs: a column on stationary geometry does not creep sideways", "[WallRibs]")
{
    const coord_t lw = scale_(3.2);
    Polygon outer = square_ccw(0., 0., 100.);
    Polygon hole  = square_ccw(0., 0., 60.);
    hole.reverse();

    WallRibParams params;
    params.stagger         = lw;
    params.corridor_offset = coord_t(scale_(1.6));
    params.max_drift       = coord_t(scale_(1.5));
    std::vector<size_t> unmerged;
    WallRibMerge plan;
    REQUIRE(plan_wall_ribs({ outer, hole }, params, nullptr, plan, unmerged));
    const Point a0 = plan.anchors.front();
    // Ten identical layers: the anchor must stay put. The old joint-based anchor fed the cut
    // offset back into the next layer's projection and migrated half a bead sideways EVERY
    // layer - a 45-degree dashed staircase across dead-vertical walls.
    auto  prev        = plan.links;
    Point last_anchor = plan.anchors.front();
    for (int i = 0; i < 10; ++ i) {
        WallRibMerge next;
        REQUIRE(plan_wall_ribs({ outer, hole }, params, &prev, next, unmerged));
        REQUIRE(next.anchors.size() == 1);
        prev        = next.links;
        last_anchor = next.anchors.front();
    }
    const double creep = (last_anchor - a0).cast<double>().norm();
    INFO("total creep over 10 layers: " << unscale<double>(coord_t(creep)) << " mm");
    REQUIRE(creep < scale_(0.5)); // was ~16mm with the joint-based anchor
}

TEST_CASE("WallRibs: column reuse respects the per-layer drift budget", "[WallRibs]")
{
    // The reuse tolerance is the caller's max_drift (half a bead capped at ONE LAYER HEIGHT).
    // A half-bead floor inside the planner used to override the layer-height cap: with thin
    // layers a column could lean far past 45 degrees and still be "continued". Simulate the
    // walls having moved off yesterday's link by more than the budget (but less than half a
    // bead): the column must NOT be reused - the rib re-founds instead of leaning after it.
    const coord_t lw = scale_(3.2);
    Polygon outer = square_ccw(0., 0., 100.);
    Polygon hole  = square_ccw(0., 0., 60.);
    hole.reverse();

    WallRibParams params;
    params.stagger         = lw;
    params.corridor_offset = coord_t(scale_(1.6));
    params.max_drift       = coord_t(scale_(1.0)); // thin layer: budget below half a bead (1.6)
    WallRibMerge        plan1;
    std::vector<size_t> unmerged;
    REQUIRE(plan_wall_ribs({ outer, hole }, params, nullptr, plan1, unmerged));
    REQUIRE(plan1.links.size() == 1);
    // Shift yesterday's link endpoints ALONG the link axis (= off both walls): both endpoints
    // reproject onto today's unchanged walls at exactly `delta`.
    auto shifted = [&plan1](double delta_mm) {
        const auto &link = plan1.links.front();
        const Point d = ((link.second - link.first).cast<double>().normalized() * scale_(delta_mm)).cast<coord_t>();
        return std::vector<std::pair<Point, Point>> { { link.first + d, link.second + d } };
    };

    WallRibStats stats;
    params.stats = &stats;
    WallRibMerge plan2;
    auto prev_far = shifted(1.3); // beyond the 1.0mm budget, below the old half-bead floor
    REQUIRE(plan_wall_ribs({ outer, hole }, params, &prev_far, plan2, unmerged));
    REQUIRE(unmerged.empty());
    REQUIRE(stats.anchor_reused == 0); // the old floor "continued" this column

    stats = WallRibStats{};
    WallRibMerge plan3;
    auto prev_near = shifted(0.7); // within the budget: the column continues
    REQUIRE(plan_wall_ribs({ outer, hole }, params, &prev_near, plan3, unmerged));
    REQUIRE(unmerged.empty());
    REQUIRE(stats.anchor_reused == 1);
}

TEST_CASE("WallRibs: a foundation stub is part of the wall and never retraces", "[WallRibs]")
{
    const coord_t lw = scale_(3.2);
    Polygon walk = square_ccw(0., 0., 50.);
    const double before = walk.length();
    Polygon quad;
    // Stub riding the right edge, tip 30mm out: the walk gains the out-and-back bead pair.
    REQUIRE(splice_wall_stub(walk, Point::new_scale(50., 0.), Point::new_scale(80., 0.), lw, quad));
    REQUIRE(walk.length() > before + 2. * scale_(25.));
    REQUIRE(std::abs(quad.area()) > 0.);
    require_no_coincident_segments(walk, lw);

    // A stub shorter than one bead has melted back into the wall: nothing to print.
    Polygon walk2 = square_ccw(0., 0., 50.);
    REQUIRE_FALSE(splice_wall_stub(walk2, Point::new_scale(50., 0.), Point::new_scale(52., 0.), lw, quad));
}
