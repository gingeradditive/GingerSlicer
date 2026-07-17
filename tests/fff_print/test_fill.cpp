#include <catch2/catch.hpp>

#include <numeric>
#include <set>
#include <tuple>
#include <sstream>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Fill/Fill.hpp"
#include "libslic3r/Fill/FillBase.hpp"
#include "libslic3r/Flow.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/SVG.hpp"
#include "libslic3r/libslic3r.h"

#include "test_data.hpp"

using namespace Slic3r;

bool test_if_solid_surface_filled(const ExPolygon& expolygon, double flow_spacing, double angle = 0, double density = 1.0);

#if 0
TEST_CASE("Fill: adjusted solid distance") {
    int surface_width = 250;
    int distance = Slic3r::Flow::solid_spacing(surface_width, 47);
    REQUIRE(distance == Approx(50));
    REQUIRE(surface_width % distance == 0);
}
#endif

TEST_CASE("Fill: Pattern Path Length", "[Fill]") {
    std::unique_ptr<Slic3r::Fill> filler(Slic3r::Fill::new_from_type("rectilinear"));
    filler->angle = float(-(PI)/2.0);
	FillParams fill_params;
	filler->spacing = 5;
	fill_params.dont_adjust = true;
	//fill_params.endpoints_overlap = false;
	fill_params.density = float(filler->spacing / 50.0);

    auto test = [&filler, &fill_params] (const ExPolygon& poly) -> Slic3r::Polylines {
        Slic3r::Surface surface(stTop, poly);
        return filler->fill_surface(&surface, fill_params);
    };

    SECTION("Square") {
        Slic3r::Points test_set;
        test_set.reserve(4);
        std::vector<Vec2d> points {Vec2d(0,0), Vec2d(100,0), Vec2d(100,100), Vec2d(0,100)};
        for (size_t i = 0; i < 4; ++i) {
            std::transform(points.cbegin()+i, points.cend(),   std::back_inserter(test_set), [] (const Vec2d& a) -> Point { return Point::new_scale(a.x(), a.y()); } ); 
            std::transform(points.cbegin(), points.cbegin()+i, std::back_inserter(test_set), [] (const Vec2d& a) -> Point { return Point::new_scale(a.x(), a.y()); } );
            Slic3r::Polylines paths = test(Slic3r::ExPolygon(test_set));
            REQUIRE(paths.size() == 1); // one continuous path

            // TODO: determine what the "Expected length" should be for rectilinear fill of a 100x100 polygon. 
            // This check only checks that it's above scale(3*100 + 2*50) + scaled_epsilon.
            // ok abs($paths->[0]->length - scale(3*100 + 2*50)) - scaled_epsilon, 'path has expected length';
            REQUIRE(std::abs(paths[0].length() - static_cast<double>(scale_(3*100 + 2*50))) - SCALED_EPSILON > 0); // path has expected length

            test_set.clear();
        }
    }
    SECTION("Diamond with endpoints on grid") {
        std::vector<Vec2d> points {Vec2d(0,0), Vec2d(100,0), Vec2d(150,50), Vec2d(100,100), Vec2d(0,100), Vec2d(-50,50)};
        Slic3r::Points test_set;
        test_set.reserve(6);
        std::transform(points.cbegin(), points.cend(),   std::back_inserter(test_set), [] (const Vec2d& a) -> Point { return Point::new_scale(a.x(), a.y()); } );
        Slic3r::Polylines paths = test(Slic3r::ExPolygon(test_set));
        REQUIRE(paths.size() == 1); // one continuous path
    }

    SECTION("Square with hole") {
        std::vector<Vec2d> square {Vec2d(0,0), Vec2d(100,0), Vec2d(100,100), Vec2d(0,100)};
        std::vector<Vec2d> hole {Vec2d(25,25), Vec2d(75,25), Vec2d(75,75), Vec2d(25,75) };
        std::reverse(hole.begin(), hole.end());

        Slic3r::Points test_hole;
        Slic3r::Points test_square;

        std::transform(square.cbegin(), square.cend(), std::back_inserter(test_square), [] (const Vec2d& a) -> Point { return Point::new_scale(a.x(), a.y()); } );
        std::transform(hole.cbegin(), hole.cend(), std::back_inserter(test_hole), [] (const Vec2d& a) -> Point { return Point::new_scale(a.x(), a.y()); } );

        for (double angle : {-(PI/2.0), -(PI/4.0), -(PI), PI/2.0, PI}) {
            for (double spacing : {25.0, 5.0, 7.5, 8.5}) {
				fill_params.density = float(filler->spacing / spacing);
                filler->angle = float(angle);
                ExPolygon e(test_square, test_hole);
                Slic3r::Polylines paths = test(e);
#if 0
				{
					BoundingBox bbox = get_extents(e);
					SVG svg("c:\\data\\temp\\square_with_holes.svg", bbox);
					svg.draw(e);
					svg.draw(paths);
					svg.Close();
				}
#endif
                REQUIRE(paths.size() >= 1);
                REQUIRE(paths.size() <= 3);
                // paths don't cross hole
                REQUIRE(diff_pl(paths, offset(e, float(SCALED_EPSILON*10))).size() == 0);
            }
        }
    }
    SECTION("Regression: Missing infill segments in some rare circumstances") {
        filler->angle = float(PI/4.0);
		fill_params.dont_adjust = false;
        filler->spacing = 0.654498;
        //filler->endpoints_overlap = unscale(359974);
		fill_params.density = 1;
        filler->layer_id = 66;
        filler->z = 20.15;

        Slic3r::Points points {Point(25771516,14142125),Point(14142138,25771515),Point(2512749,14142131),Point(14142125,2512749)};
        Slic3r::Polylines paths = test(Slic3r::ExPolygon(points));
        REQUIRE(paths.size() == 1); // one continuous path

        // TODO: determine what the "Expected length" should be for rectilinear fill of a 100x100 polygon. 
        // This check only checks that it's above scale(3*100 + 2*50) + scaled_epsilon.
        // ok abs($paths->[0]->length - scale(3*100 + 2*50)) - scaled_epsilon, 'path has expected length';
        REQUIRE(std::abs(paths[0].length() - static_cast<double>(scale_(3*100 + 2*50))) - SCALED_EPSILON > 0); // path has expected length
    }

    SECTION("Rotated Square") {
        Slic3r::Points square { Point::new_scale(0,0), Point::new_scale(50,0), Point::new_scale(50,50), Point::new_scale(0,50)};
        Slic3r::ExPolygon expolygon(square);
        std::unique_ptr<Slic3r::Fill> filler(Slic3r::Fill::new_from_type("rectilinear"));
		filler->bounding_box = get_extents(expolygon.contour);
        filler->angle = 0;
        
        Surface surface(stTop, expolygon);
        auto flow = Slic3r::Flow(0.69f, 0.4f, 0.50f);

		FillParams fill_params;
		fill_params.density = 1.0;
		filler->spacing = flow.spacing();

        for (auto angle : { 0.0, 45.0}) {
            surface.expolygon.rotate(angle, Point(0,0));
            Polylines paths = filler->fill_surface(&surface, fill_params);
            REQUIRE(paths.size() == 1);
        }
    }

    #if 0   // Disabled temporarily due to precission issues on the Mac VM
    SECTION("Solid surface fill") {
        Slic3r::Points points {
            Point::new_scale(6883102, 9598327.01296997),
            Point::new_scale(6883102, 20327272.01297),
            Point::new_scale(3116896, 20327272.01297),
            Point::new_scale(3116896, 9598327.01296997) 
        };
        Slic3r::ExPolygon expolygon(points);
         
        REQUIRE(test_if_solid_surface_filled(expolygon, 0.55) == true);
        for (size_t i = 0; i <= 20; ++i)
        {
            expolygon.scale(1.05);
            REQUIRE(test_if_solid_surface_filled(expolygon, 0.55) == true);
        }
    }
    #endif

    SECTION("Solid surface fill") {
        Slic3r::Points points {
                Slic3r::Point(59515297,5422499),Slic3r::Point(59531249,5578697),Slic3r::Point(59695801,6123186),
                Slic3r::Point(59965713,6630228),Slic3r::Point(60328214,7070685),Slic3r::Point(60773285,7434379),
                Slic3r::Point(61274561,7702115),Slic3r::Point(61819378,7866770),Slic3r::Point(62390306,7924789),
                Slic3r::Point(62958700,7866744),Slic3r::Point(63503012,7702244),Slic3r::Point(64007365,7434357),
                Slic3r::Point(64449960,7070398),Slic3r::Point(64809327,6634999),Slic3r::Point(65082143,6123325),
                Slic3r::Point(65245005,5584454),Slic3r::Point(65266967,5422499),Slic3r::Point(66267307,5422499),
                Slic3r::Point(66269190,8310081),Slic3r::Point(66275379,17810072),Slic3r::Point(66277259,20697500),
                Slic3r::Point(65267237,20697500),Slic3r::Point(65245004,20533538),Slic3r::Point(65082082,19994444),
                Slic3r::Point(64811462,19488579),Slic3r::Point(64450624,19048208),Slic3r::Point(64012101,18686514),
                Slic3r::Point(63503122,18415781),Slic3r::Point(62959151,18251378),Slic3r::Point(62453416,18198442),
                Slic3r::Point(62390147,18197355),Slic3r::Point(62200087,18200576),Slic3r::Point(61813519,18252990),
                Slic3r::Point(61274433,18415918),Slic3r::Point(60768598,18686517),Slic3r::Point(60327567,19047892),
                Slic3r::Point(59963609,19493297),Slic3r::Point(59695865,19994587),Slic3r::Point(59531222,20539379),
                Slic3r::Point(59515153,20697500),Slic3r::Point(58502480,20697500),Slic3r::Point(58502480,5422499)
        };
        Slic3r::ExPolygon expolygon(points);
         
        REQUIRE(test_if_solid_surface_filled(expolygon, 0.55) == true);
        REQUIRE(test_if_solid_surface_filled(expolygon, 0.55, PI/2.0) == true);
    }
    SECTION("Solid surface fill") {
        Slic3r::Points points {
            Point::new_scale(0,0),Point::new_scale(98,0),Point::new_scale(98,10), Point::new_scale(0,10)
        };
        Slic3r::ExPolygon expolygon(points);

        REQUIRE(test_if_solid_surface_filled(expolygon, 0.5, 45.0, 0.99) == true);
    }
}

// Ginger: single-path infill (single_path_mode, config key formerly connect_infill_polygons).
// A continuous (simply connected) sparse infill region must come out as ONE polyline:
// zero travel moves inside the region.
TEST_CASE("Fill: single_path_mode single path", "[Fill]") {
    auto make_filler = [](size_t layer_id) {
        std::unique_ptr<Slic3r::Fill> filler(Slic3r::Fill::new_from_type("rectilinear"));
        filler->angle    = 0.f;
        filler->spacing  = 5.0; // large pellet-style bead
        filler->layer_id = layer_id;
        filler->z        = 0.9 * (layer_id + 1);
        return filler;
    };

    FillParams fill_params;
    fill_params.density          = 0.2f;
    fill_params.multiline        = 2;
    fill_params.connect_polygons = true;
    fill_params.dont_adjust      = true;

    // Pellet printers must never extrude twice over the same line: verify that no segment is
    // traversed twice, within a path or across paths.
    auto require_no_retraced_segments = [](const Slic3r::Polylines &paths) {
        std::set<std::tuple<coord_t, coord_t, coord_t, coord_t>> seen;
        size_t duplicates = 0;
        for (const Polyline &pl : paths)
            for (size_t i = 1; i < pl.size(); ++ i) {
                Point a = pl.points[i - 1], b = pl.points[i];
                if (a == b)
                    continue;
                if (b.x() < a.x() || (b.x() == a.x() && b.y() < a.y()))
                    std::swap(a, b);
                if (! seen.insert(std::make_tuple(a.x(), a.y(), b.x(), b.y())).second)
                    ++ duplicates;
            }
        CAPTURE(duplicates);
        REQUIRE(duplicates == 0);
    };

    // Catch the near-coincident case the exact-duplicate detector above misses: two long, nearly
    // parallel segments closer than half the extrusion width (e.g. a boundary arc traced right under a
    // row that is almost tangent to the wall - an out-and-back spur of doubled extrusion).
    auto require_no_overlapping_parallel_segments = [](const Slic3r::Polylines &paths, double width_mm) {
        struct Seg { Vec2d a, b; };
        std::vector<Seg> segs;
        for (const Polyline &pl : paths)
            for (size_t i = 1; i < pl.size(); ++ i)
                if (pl.points[i - 1] != pl.points[i])
                    segs.push_back({ pl.points[i - 1].cast<double>(), pl.points[i].cast<double>() });
        const double dist_max    = 0.45 * scale_(width_mm);
        const double min_overlap = 3.0  * scale_(width_mm);
        size_t violations = 0;
        for (size_t i = 0; i < segs.size() && violations == 0; ++ i)
            for (size_t j = i + 1; j < segs.size(); ++ j) {
                Vec2d  di = segs[i].b - segs[i].a, dj = segs[j].b - segs[j].a;
                double li = di.norm(), lj = dj.norm();
                if (li < SCALED_EPSILON || lj < SCALED_EPSILON)
                    continue;
                if (std::abs(di.dot(dj)) < 0.95 * li * lj)
                    continue; // not nearly parallel
                // Longitudinal overlap of j on i's axis.
                Vec2d  u  = di / li;
                double t0 = (segs[j].a - segs[i].a).dot(u), t1 = (segs[j].b - segs[i].a).dot(u);
                double lo = std::max(0., std::min(t0, t1)), hi = std::min(li, std::max(t0, t1));
                if (hi - lo <= min_overlap)
                    continue;
                // Perpendicular offset over the overlapping stretch, sampled; nearly parallel lines ->
                // the offset varies linearly, sampling is exact enough.
                Vec2d  n = Vec2d(-u.y(), u.x());
                double close_len = 0.;
                const int K = 16;
                for (int k = 0; k <= K; ++ k) {
                    double t  = lo + (hi - lo) * k / K;
                    double s  = std::abs(t1 - t0) < SCALED_EPSILON ? 0. : (t - t0) / (t1 - t0);
                    Vec2d  pj = segs[j].a + dj * s;
                    if (std::abs((pj - (segs[i].a + u * t)).dot(n)) < dist_max)
                        close_len += (hi - lo) / (K + 1);
                }
                if (close_len > min_overlap) {
                    ++ violations;
                    break;
                }
            }
        CAPTURE(violations);
        REQUIRE(violations == 0);
    };

    SECTION("Square 200x200, multiline 2") {
        Slic3r::Points square { Point::new_scale(0,0), Point::new_scale(200,0), Point::new_scale(200,200), Point::new_scale(0,200) };
        Slic3r::ExPolygon expolygon(square);
        for (size_t layer_id : { 0, 1, 2, 3 }) {
            DYNAMIC_SECTION("layer " << layer_id) {
                auto filler = make_filler(layer_id);
                filler->bounding_box = get_extents(expolygon.contour);
                Slic3r::Surface surface(stInternal, expolygon);
                Slic3r::Polylines paths = filler->fill_surface(&surface, fill_params);
                CAPTURE(paths.size());
                REQUIRE(paths.size() == 1); // single continuous path, no travels
                require_no_retraced_segments(paths);
                require_no_overlapping_parallel_segments(paths, filler->spacing);
                REQUIRE(paths.front().size() > 2);
                // Connect-before-multiply: the spliced outline is one CLOSED loop, emitted as an
                // ExtrusionLoop so the G-code starts it right where the wall ended (no travel at all).
                REQUIRE(paths.front().is_closed());
            }
        }
    }

    SECTION("Square 200x200, grid pattern (two crossing sweeps), multiline 1") {
        std::unique_ptr<Slic3r::Fill> filler(Slic3r::Fill::new_from_type("grid"));
        filler->angle    = 0.f;
        filler->spacing  = 5.0;
        filler->layer_id = 0;
        filler->z        = 0.9;
        FillParams grid_params = fill_params;
        grid_params.multiline  = 1;
        grid_params.density    = 0.2f;
        Slic3r::Points square { Point::new_scale(0,0), Point::new_scale(200,0), Point::new_scale(200,200), Point::new_scale(0,200) };
        Slic3r::ExPolygon expolygon(square);
        filler->bounding_box = get_extents(expolygon.contour);
        Slic3r::Surface surface(stInternal, expolygon);
        Slic3r::Polylines paths = filler->fill_surface(&surface, grid_params);
        CAPTURE(paths.size());
        REQUIRE(paths.size() == 1); // single continuous path, no travels
        require_no_retraced_segments(paths);
        require_no_overlapping_parallel_segments(paths, filler->spacing);
    }

    SECTION("Square 200x200, grid trapezoidal (multiline 2)") {
        std::unique_ptr<Slic3r::Fill> filler(Slic3r::Fill::new_from_type("grid"));
        filler->angle    = 0.f;
        filler->spacing  = 5.0;
        filler->layer_id = 0;
        filler->z        = 0.9;
        FillParams grid_params = fill_params; // multiline = 2 -> fill_surface_trapezoidal
        Slic3r::Points square { Point::new_scale(0,0), Point::new_scale(200,0), Point::new_scale(200,200), Point::new_scale(0,200) };
        Slic3r::ExPolygon expolygon(square);
        filler->bounding_box = get_extents(expolygon.contour);
        Slic3r::Surface surface(stInternal, expolygon);
        Slic3r::Polylines paths = filler->fill_surface(&surface, grid_params);
        CAPTURE(paths.size());
        REQUIRE(paths.size() == 1); // single continuous path, no travels
        require_no_retraced_segments(paths);
        require_no_overlapping_parallel_segments(paths, filler->spacing);
    }

    SECTION("Narrow slanted strip, grid trapezoidal (multiline 2)") {
        // Reproduces a real part: long thin strip at ~30deg, much narrower than the trapezoid wave
        // amplitude, so each row is chopped into many fragments by both long edges.
        std::unique_ptr<Slic3r::Fill> filler(Slic3r::Fill::new_from_type("grid"));
        filler->angle    = 0.f;
        filler->spacing  = 1.2;
        filler->layer_id = 1; // odd layer: transposed pattern, like the failing layers on the real part
        filler->z        = 0.9;
        FillParams grid_params = fill_params;
        grid_params.density = 0.1f;
        Slic3r::Polygon strip;
        const double c = std::cos(0.5), s = std::sin(0.5);
        for (const Vec2d &p : { Vec2d(0,0), Vec2d(290,0), Vec2d(290,42), Vec2d(0,42) })
            strip.points.emplace_back(Point::new_scale(p.x()*c - p.y()*s, p.x()*s + p.y()*c));
        Slic3r::ExPolygon expolygon(strip);
        filler->bounding_box = get_extents(expolygon.contour);
        Slic3r::Surface surface(stInternal, expolygon);
        Slic3r::Polylines paths = filler->fill_surface(&surface, grid_params);
        CAPTURE(paths.size());
        // Extreme fragment soup (each trapezoid row chopped into many pieces by both long edges):
        // connect-before-multiply dilates and unions whatever trails the connector produced, and the
        // spliced outline comes back as one single closed loop even here.
        REQUIRE(paths.size() == 1);
        REQUIRE(paths.front().is_closed());
        require_no_retraced_segments(paths);
        require_no_overlapping_parallel_segments(paths, filler->spacing);
    }

    SECTION("L-shape, multiline 2") {
        Slic3r::Points lshape { Point::new_scale(0,0), Point::new_scale(200,0), Point::new_scale(200,100),
                                Point::new_scale(100,100), Point::new_scale(100,200), Point::new_scale(0,200) };
        Slic3r::ExPolygon expolygon(lshape);
        auto filler = make_filler(0);
        filler->bounding_box = get_extents(expolygon.contour);
        Slic3r::Surface surface(stInternal, expolygon);
        Slic3r::Polylines paths = filler->fill_surface(&surface, fill_params);
        CAPTURE(paths.size());
        REQUIRE(paths.size() == 1); // single continuous path, no travels
        require_no_retraced_segments(paths);
        require_no_overlapping_parallel_segments(paths, filler->spacing);
    }

    // Ginger: trail closure (free-run closure + defect sliding + mouth stitching in the Euler
    // connector). A region with a hole chops every scanline onto two contours; the Euler trail used
    // to come out OPEN with its two ends at a fixed, arrival-independent spot - on the real part
    // that cost a 60-290mm travel INTO the region on every layer, regardless of where the toolhead
    // came from. The connector must now emit closed loops (seam freely placeable at G-code export);
    // whatever legitimately stays open must have a mouth wider than the stitch limit (2.5 line
    // spacings), or the stitch pass would have closed it.
    SECTION("Ring 200x200 with 100x100 hole, multiline 1 (trail closure)") {
        Slic3r::Points square { Point::new_scale(0,0), Point::new_scale(200,0), Point::new_scale(200,200), Point::new_scale(0,200) };
        Slic3r::Points hole   { Point::new_scale(50,50), Point::new_scale(50,150), Point::new_scale(150,150), Point::new_scale(150,50) };
        Slic3r::ExPolygon expolygon(square);
        expolygon.holes.emplace_back(Slic3r::Polygon(hole)); // CW ring -> hole
        FillParams params1 = fill_params;
        params1.multiline  = 1;
        for (size_t layer_id : { 0, 1 }) {
            DYNAMIC_SECTION("ring layer " << layer_id) {
                auto filler = make_filler(layer_id);
                filler->bounding_box = get_extents(expolygon.contour);
                Slic3r::Surface surface(stInternal, expolygon);
                Slic3r::Polylines paths = filler->fill_surface(&surface, params1);
                CAPTURE(paths.size());
                REQUIRE(! paths.empty());
                require_no_retraced_segments(paths);
                require_no_overlapping_parallel_segments(paths, filler->spacing);
                size_t n_open = 0;
                for (const Polyline &pl : paths)
                    if (! pl.is_closed())
                        ++ n_open;
                CAPTURE(n_open);
                REQUIRE(n_open == 0); // the ring is fully closable: closed loops only, free seam
            }
        }
    }

    SECTION("Offset hole + notch, multiline 1 (open trails only with a wide mouth)") {
        // Deliberately awkward region (hole off-center, concave notch): closure is not guaranteed
        // here, but the invariant is - an emitted OPEN path means the stitch pass judged its mouth
        // wider than the limit, so no path may end with a small, silently-unstitched mouth.
        Slic3r::Points outline { Point::new_scale(0,0), Point::new_scale(200,0), Point::new_scale(200,80),
                                 Point::new_scale(120,80), Point::new_scale(120,110), Point::new_scale(200,110),
                                 Point::new_scale(200,200), Point::new_scale(0,200) };
        Slic3r::Points hole    { Point::new_scale(30,60), Point::new_scale(30,140), Point::new_scale(80,140), Point::new_scale(80,60) };
        Slic3r::ExPolygon expolygon{ Slic3r::Polygon(outline) };
        expolygon.holes.emplace_back(Slic3r::Polygon(hole));
        FillParams params1 = fill_params;
        params1.multiline  = 1;
        auto filler = make_filler(0);
        filler->bounding_box = get_extents(expolygon.contour);
        Slic3r::Surface surface(stInternal, expolygon);
        Slic3r::Polylines paths = filler->fill_surface(&surface, params1);
        CAPTURE(paths.size());
        REQUIRE(! paths.empty());
        require_no_retraced_segments(paths);
        require_no_overlapping_parallel_segments(paths, filler->spacing);
        const double stitch_max = 2.5 * scale_(filler->spacing);
        for (const Polyline &pl : paths)
            if (! pl.is_closed()) {
                const double mouth = (pl.points.back() - pl.points.front()).cast<double>().norm();
                CAPTURE(mouth * SCALING_FACTOR);
                CHECK(mouth > stitch_max);
            }
    }

    SECTION("Square 200x200, multiline 3 (odd: centerline spliced with the ring)") {
        // With an odd multiline the centerline itself is extruded: it must stay CLOSED so the splice
        // merges it with the offset ring walls into one closed loop (-> ExtrusionLoop -> free seam).
        Slic3r::Points square { Point::new_scale(0,0), Point::new_scale(200,0), Point::new_scale(200,200), Point::new_scale(0,200) };
        Slic3r::ExPolygon expolygon(square);
        FillParams params3 = fill_params;
        params3.multiline  = 3;
        auto filler = make_filler(0);
        filler->bounding_box = get_extents(expolygon.contour);
        Slic3r::Surface surface(stInternal, expolygon);
        Slic3r::Polylines paths = filler->fill_surface(&surface, params3);
        CAPTURE(paths.size());
        // A serpentine over an ODD number of rows cannot close (parity), so the centerline may stay an
        // open path - but the ring wall is spliced into it as a detour: still ONE path, zero travels.
        REQUIRE(paths.size() == 1);
        require_no_retraced_segments(paths);
        require_no_overlapping_parallel_segments(paths, filler->spacing);
    }

    SECTION("Square 200x200, grid trapezoidal (multiline 3)") {
        std::unique_ptr<Slic3r::Fill> filler(Slic3r::Fill::new_from_type("grid"));
        filler->angle    = 0.f;
        filler->spacing  = 5.0;
        filler->layer_id = 0;
        filler->z        = 0.9;
        FillParams params3 = fill_params;
        params3.multiline  = 3;
        Slic3r::Points square { Point::new_scale(0,0), Point::new_scale(200,0), Point::new_scale(200,200), Point::new_scale(0,200) };
        Slic3r::ExPolygon expolygon(square);
        filler->bounding_box = get_extents(expolygon.contour);
        Slic3r::Surface surface(stInternal, expolygon);
        Slic3r::Polylines paths = filler->fill_surface(&surface, params3);
        CAPTURE(paths.size());
        REQUIRE(paths.size() == 1); // single continuous path, no travels
        REQUIRE(paths.front().is_closed());
        require_no_retraced_segments(paths);
        require_no_overlapping_parallel_segments(paths, filler->spacing);
    }

    SECTION("Slanted wall nearly tangent to a row, multiline 2") {
        // A wall at ~14 deg from the row direction: one row always crosses it at a grazing angle, so
        // the row and the boundary stretch right under it nearly coincide. Selecting that boundary gap
        // would extrude the same line twice (an out-and-back spur, seen on a real part); the connector
        // must block it and route around.
        Slic3r::Points quad { Point::new_scale(0,0), Point::new_scale(200,0), Point::new_scale(200,200), Point::new_scale(50,200) };
        Slic3r::ExPolygon expolygon(quad);
        auto filler = make_filler(0);
        filler->bounding_box = get_extents(expolygon.contour);
        Slic3r::Surface surface(stInternal, expolygon);
        Slic3r::Polylines paths = filler->fill_surface(&surface, fill_params);
        CAPTURE(paths.size());
        REQUIRE(paths.size() == 1); // single continuous path, no travels
        REQUIRE(paths.front().is_closed());
        require_no_retraced_segments(paths);
        require_no_overlapping_parallel_segments(paths, filler->spacing);
    }
}

/*
{
    my $collection = Slic3r::Polyline::Collection->new(
            Slic3r::Polyline->new([0,15], [0,18], [0,20]),
            Slic3r::Polyline->new([0,10], [0,8], [0,5]),
            );
    is_deeply
        [ map $_->[Y], map @$_, @{$collection->chained_path_from(Slic3r::Point->new(0,30), 0)} ],
        [20, 18, 15, 10, 8, 5],
        'chained path';
}

{
    my $collection = Slic3r::Polyline::Collection->new(
            Slic3r::Polyline->new([4,0], [10,0], [15,0]),
            Slic3r::Polyline->new([10,5], [15,5], [20,5]),
            );
    is_deeply
        [ map $_->[X], map @$_, @{$collection->chained_path_from(Slic3r::Point->new(30,0), 0)} ],
        [reverse 4, 10, 15, 10, 15, 20],
        'chained path';
}

{
    my $collection = Slic3r::ExtrusionPath::Collection->new(
            map Slic3r::ExtrusionPath->new(polyline => $_, role => 0, mm3_per_mm => 1),
            Slic3r::Polyline->new([0,15], [0,18], [0,20]),
            Slic3r::Polyline->new([0,10], [0,8], [0,5]),
            );
    is_deeply
        [ map $_->[Y], map @{$_->polyline}, @{$collection->chained_path_from(Slic3r::Point->new(0,30), 0)} ],
        [20, 18, 15, 10, 8, 5],
        'chained path';
}

{
    my $collection = Slic3r::ExtrusionPath::Collection->new(
            map Slic3r::ExtrusionPath->new(polyline => $_, role => 0, mm3_per_mm => 1),
            Slic3r::Polyline->new([15,0], [10,0], [4,0]),
            Slic3r::Polyline->new([10,5], [15,5], [20,5]),
            );
    is_deeply
        [ map $_->[X], map @{$_->polyline}, @{$collection->chained_path_from(Slic3r::Point->new(30,0), 0)} ],
        [reverse 4, 10, 15, 10, 15, 20],
        'chained path';
}

for my $pattern (qw(rectilinear honeycomb hilbertcurve concentric)) {
    my $config = Slic3r::Config->new_from_defaults;
    $config->set('fill_pattern', $pattern);
    $config->set('external_fill_pattern', $pattern);
    $config->set('perimeters', 1);
    $config->set('skirts', 0);
    $config->set('fill_density', 20);
    $config->set('layer_height', 0.05);
    $config->set('perimeter_extruder', 1);
    $config->set('infill_extruder', 2);
    my $print = Slic3r::Test::init_print('20mm_cube', config => $config, scale => 2);
    ok my $gcode = Slic3r::Test::gcode($print), "successful $pattern infill generation";
    my $tool = undef;
    my @perimeter_points = my @infill_points = ();
    Slic3r::GCode::Reader->new->parse($gcode, sub {
            my ($self, $cmd, $args, $info) = @_;

            if ($cmd =~ /^T(\d+)/) {
            $tool = $1;
            } elsif ($cmd eq 'G1' && $info->{extruding} && $info->{dist_XY} > 0) {
            if ($tool == $config->perimeter_extruder-1) {
            push @perimeter_points, Slic3r::Point->new_scale($args->{X}, $args->{Y});
            } elsif ($tool == $config->infill_extruder-1) {
            push @infill_points, Slic3r::Point->new_scale($args->{X}, $args->{Y});
            }
            }
            });
    my $convex_hull = convex_hull(\@perimeter_points);
    ok !(defined first { !$convex_hull->contains_point($_) } @infill_points), "infill does not exceed perimeters ($pattern)";
}

{
    my $config = Slic3r::Config->new_from_defaults;
    $config->set('infill_only_where_needed', 1);
    $config->set('bottom_solid_layers', 0);
    $config->set('infill_extruder', 2);
    $config->set('infill_extrusion_width', 0.5);
    $config->set('fill_density', 40);
    $config->set('cooling', 0);                 # for preventing speeds from being altered
        $config->set('first_layer_speed', '100%');  # for preventing speeds from being altered

        my $test = sub {
            my $print = Slic3r::Test::init_print('pyramid', config => $config);

            my $tool = undef;
            my @infill_extrusions = ();  # array of polylines
                Slic3r::GCode::Reader->new->parse(Slic3r::Test::gcode($print), sub {
                        my ($self, $cmd, $args, $info) = @_;

                        if ($cmd =~ /^T(\d+)/) {
                        $tool = $1;
                        } elsif ($cmd eq 'G1' && $info->{extruding} && $info->{dist_XY} > 0) {
                        if ($tool == $config->infill_extruder-1) {
                        push @infill_extrusions, Slic3r::Line->new_scale(
                                [ $self->X, $self->Y ],
                                [ $info->{new_X}, $info->{new_Y} ],
                                );
                        }
                        }
                        });
            return 0 if !@infill_extrusions;  # prevent calling convex_hull() with no points

                my $convex_hull = convex_hull([ map $_->pp, map @$_, @infill_extrusions ]);
            return unscale unscale sum(map $_->area, @{offset([$convex_hull], scale(+$config->infill_extrusion_width/2))});
        };

    my $tolerance = 5;  # mm^2

        $config->set('solid_infill_below_area', 0);
    ok $test->() < $tolerance,
       'no infill is generated when using infill_only_where_needed on a pyramid';

    $config->set('solid_infill_below_area', 70);
    ok abs($test->() - $config->solid_infill_below_area) < $tolerance,
       'infill is only generated under the forced solid shells';
}

{
    my $config = Slic3r::Config->new_from_defaults;
    $config->set('skirts', 0);
    $config->set('perimeters', 1);
    $config->set('fill_density', 0);
    $config->set('top_solid_layers', 0);
    $config->set('bottom_solid_layers', 0);
    $config->set('solid_infill_below_area', 20000000);
    $config->set('solid_infill_every_layers', 2);
    $config->set('perimeter_speed', 99);
    $config->set('external_perimeter_speed', 99);
    $config->set('cooling', 0);
    $config->set('first_layer_speed', '100%');

    my $print = Slic3r::Test::init_print('20mm_cube', config => $config);
    my %layers_with_extrusion = ();
    Slic3r::GCode::Reader->new->parse(Slic3r::Test::gcode($print), sub {
            my ($self, $cmd, $args, $info) = @_;

            if ($cmd eq 'G1' && $info->{dist_XY} > 0 && $info->{extruding}) {
            if (($args->{F} // $self->F) != $config->perimeter_speed*60) {
            $layers_with_extrusion{$self->Z} = ($args->{F} // $self->F);
            }
            }
            });

    ok !%layers_with_extrusion,
       "solid_infill_below_area and solid_infill_every_layers are ignored when fill_density is 0";
}

{
    my $config = Slic3r::Config->new_from_defaults;
    $config->set('skirts', 0);
    $config->set('perimeters', 3);
    $config->set('fill_density', 0);
    $config->set('layer_height', 0.2);
    $config->set('first_layer_height', 0.2);
    $config->set('nozzle_diameter', [0.35]);
    $config->set('infill_extruder', 2);
    $config->set('solid_infill_extruder', 2);
    $config->set('infill_extrusion_width', 0.52);
    $config->set('solid_infill_extrusion_width', 0.52);
    $config->set('first_layer_extrusion_width', 0);

    my $print = Slic3r::Test::init_print('A', config => $config);
    my %infill = ();  # Z => [ Line, Line ... ]
        my $tool = undef;
    Slic3r::GCode::Reader->new->parse(Slic3r::Test::gcode($print), sub {
            my ($self, $cmd, $args, $info) = @_;

            if ($cmd =~ /^T(\d+)/) {
            $tool = $1;
            } elsif ($cmd eq 'G1' && $info->{extruding} && $info->{dist_XY} > 0) {
            if ($tool == $config->infill_extruder-1) {
            my $z = 1 * $self->Z;
            $infill{$z} ||= [];
            push @{$infill{$z}}, Slic3r::Line->new_scale(
                    [ $self->X, $self->Y ],
                    [ $info->{new_X}, $info->{new_Y} ],
                    );
            }
            }
            });
    my $grow_d = scale($config->infill_extrusion_width)/2;
    my $layer0_infill = union([ map @{$_->grow($grow_d)}, @{ $infill{0.2} } ]);
    my $layer1_infill = union([ map @{$_->grow($grow_d)}, @{ $infill{0.4} } ]);
    my $diff = diff($layer0_infill, $layer1_infill);
    $diff = offset2_ex($diff, -$grow_d, +$grow_d);
    $diff = [ grep { $_->area > 2*(($grow_d*2)**2) } @$diff ];
    is scalar(@$diff), 0, 'no missing parts in solid shell when fill_density is 0';
}

{
    # GH: #2697
    my $config = Slic3r::Config->new_from_defaults;
    $config->set('perimeter_extrusion_width', 0.72);
    $config->set('top_infill_extrusion_width', 0.1);
    $config->set('infill_extruder', 2);         # in order to distinguish infill
        $config->set('solid_infill_extruder', 2);   # in order to distinguish infill

        my $print = Slic3r::Test::init_print('20mm_cube', config => $config);
    my %infill = ();  # Z => [ Line, Line ... ]
        my %other  = ();  # Z => [ Line, Line ... ]
        my $tool = undef;
    Slic3r::GCode::Reader->new->parse(Slic3r::Test::gcode($print), sub {
            my ($self, $cmd, $args, $info) = @_;

            if ($cmd =~ /^T(\d+)/) {
            $tool = $1;
            } elsif ($cmd eq 'G1' && $info->{extruding} && $info->{dist_XY} > 0) {
            my $z = 1 * $self->Z;
            my $line = Slic3r::Line->new_scale(
                    [ $self->X, $self->Y ],
                    [ $info->{new_X}, $info->{new_Y} ],
                    );
            if ($tool == $config->infill_extruder-1) {
            $infill{$z} //= [];
            push @{$infill{$z}}, $line;
            } else {
            $other{$z} //= [];
            push @{$other{$z}}, $line;
            }
            }
            });
    my $top_z = max(keys %infill);
    my $top_infill_grow_d = scale($config->top_infill_extrusion_width)/2;
    my $top_infill = union([ map @{$_->grow($top_infill_grow_d)}, @{ $infill{$top_z} } ]);
    my $perimeters_grow_d = scale($config->perimeter_extrusion_width)/2;
    my $perimeters = union([ map @{$_->grow($perimeters_grow_d)}, @{ $other{$top_z} } ]);
    my $covered = union_ex([ @$top_infill, @$perimeters ]);
    my @holes = map @{$_->holes}, @$covered;
    ok sum(map unscale unscale $_->area*-1, @holes) < 1, 'no gaps between top solid infill and perimeters';
}
*/

bool test_if_solid_surface_filled(const ExPolygon& expolygon, double flow_spacing, double angle, double density)
{
    std::unique_ptr<Slic3r::Fill> filler(Slic3r::Fill::new_from_type("rectilinear"));
	filler->bounding_box = get_extents(expolygon.contour);
    filler->angle = float(angle);

	Flow flow(float(flow_spacing), 0.4f, float(flow_spacing));
	filler->spacing = flow.spacing();

	FillParams fill_params;
	fill_params.density = float(density);
	fill_params.dont_adjust = false;

	Surface surface(stBottom, expolygon);
	Slic3r::Polylines paths = filler->fill_surface(&surface, fill_params);

    // check whether any part was left uncovered
    Polygons grown_paths;
    grown_paths.reserve(paths.size());

    // figure out what is actually going on here re: data types
    float line_offset = float(scale_(filler->spacing / 2.0 + EPSILON));
    std::for_each(paths.begin(), paths.end(), [line_offset, &grown_paths] (const Slic3r::Polyline& p) {
        polygons_append(grown_paths, offset(p, line_offset));
    });

	// Shrink the initial expolygon a bit, this simulates the infill / perimeter overlap that we usually apply.
    ExPolygons uncovered = diff_ex(offset(expolygon, - float(0.2 * scale_(flow_spacing))), grown_paths, ApplySafetyOffset::Yes);

    // ignore very small dots
    const double scaled_flow_spacing = std::pow(scale_(flow_spacing), 2);
    uncovered.erase(std::remove_if(uncovered.begin(), uncovered.end(), [scaled_flow_spacing](const ExPolygon& poly) { return poly.area() < scaled_flow_spacing; }), uncovered.end());

#if 0
	if (! uncovered.empty()) {
		BoundingBox bbox = get_extents(expolygon.contour);
		bbox.merge(get_extents(uncovered));
		bbox.merge(get_extents(grown_paths));
		SVG svg("c:\\data\\temp\\test_if_solid_surface_filled.svg", bbox);
		svg.draw(expolygon);
		svg.draw(uncovered, "red");
		svg.Close();
	}
#endif

    return uncovered.empty(); // solid surface is fully filled
}

// Ginger: physical link rules of single_path_splice_loops with the `island` parameter. A link
// may be ANY length (no length policy), but it must stay inside the island and must not retrace
// an already extruded line; links up to 1.5 stagger always pass. island == nullptr keeps the
// legacy distance-capped behavior of the multiline (trapezoidal) caller.
TEST_CASE("Fill: single_path_splice_loops physical link rules", "[Fill]") {
    const double W = scale_(3.0); // extrusion width = link stagger

    auto square_ring = [](double cx, double cy, double half) -> Polyline {
        Polyline pl;
        pl.points = { Point(coord_t(cx - half), coord_t(cy - half)),
                      Point(coord_t(cx + half), coord_t(cy - half)),
                      Point(coord_t(cx + half), coord_t(cy + half)),
                      Point(coord_t(cx - half), coord_t(cy + half)),
                      Point(coord_t(cx - half), coord_t(cy - half)) };
        return pl;
    };

    SECTION("any-length weld inside the island") {
        // Two rings 50mm apart in a plain rectangular island: far beyond any legacy cap, still
        // welded because the link lies inside the part and retraces nothing.
        Polygons island { Polygon(Points{ Point(0, 0), Point(coord_t(scale_(200.)), 0),
                                          Point(coord_t(scale_(200.)), coord_t(scale_(100.))),
                                          Point(0, coord_t(scale_(100.))) }) };
        Polylines loops;
        loops.emplace_back(square_ring(scale_(50.), scale_(50.), scale_(15.)));
        loops.emplace_back(square_ring(scale_(130.), scale_(50.), scale_(15.)));
        single_path_splice_loops(loops, scale_(1000.), W, &island);
        REQUIRE(loops.size() == 1);
        REQUIRE(loops.front().points.front() == loops.front().points.back());
    }

    SECTION("a link across void outside the island is rejected") {
        // U-shaped island, one ring per arm: both rings sit above the bottom band, so EVERY
        // straight link between them crosses the notch void between the arms. Without the
        // island parameter this pair would weld; with it the rings must stay separate.
        Polygons island { Polygon(Points{
            Point(0, 0), Point(coord_t(scale_(110.)), 0),
            Point(coord_t(scale_(110.)), coord_t(scale_(100.))), Point(coord_t(scale_(70.)), coord_t(scale_(100.))),
            Point(coord_t(scale_(70.)), coord_t(scale_(20.))),   Point(coord_t(scale_(40.)), coord_t(scale_(20.))),
            Point(coord_t(scale_(40.)), coord_t(scale_(100.))),  Point(0, coord_t(scale_(100.))) }) };
        Polylines loops;
        loops.emplace_back(square_ring(scale_(20.), scale_(60.), scale_(12.)));
        loops.emplace_back(square_ring(scale_(90.), scale_(60.), scale_(12.)));
        single_path_splice_loops(loops, scale_(1000.), W, &island);
        REQUIRE(loops.size() == 2);
    }

    SECTION("island == nullptr keeps the legacy distance cap") {
        Polylines near_loops;
        near_loops.emplace_back(square_ring(scale_(50.), scale_(50.), scale_(15.)));
        near_loops.emplace_back(square_ring(scale_(90.), scale_(50.), scale_(15.))); // 10mm gap
        single_path_splice_loops(near_loops, scale_(20.), W);
        REQUIRE(near_loops.size() == 1);

        Polylines far_loops;
        far_loops.emplace_back(square_ring(scale_(50.), scale_(50.), scale_(15.)));
        far_loops.emplace_back(square_ring(scale_(130.), scale_(50.), scale_(15.))); // 50mm gap
        single_path_splice_loops(far_loops, scale_(20.), W);
        REQUIRE(far_loops.size() == 2);
    }
}
