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
