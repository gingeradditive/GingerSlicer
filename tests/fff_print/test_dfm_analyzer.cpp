#include <catch2/catch.hpp>

#include <atomic>
#include <cmath>

#include "libslic3r/DfmAnalyzer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

// Default thresholds: nozzle 3.0 mm -> thin critical < 3 mm, thin warning < 6 mm.
// Overhangs carry no angle threshold: every leaning facet is graded 0..90 deg of lean
// from the vertical (external = downward-facing, internal = upward-facing over infill;
// a flat top surface is internal at 90). Plumb facets (quantized 0) and bed contact
// stay unflagged.

static size_t count_flagged(const DfmClassification &c, uint8_t flag)
{
    size_t n = 0;
    for (uint8_t f : c.facet_flags)
        if (f & flag)
            ++ n;
    return n;
}

static bool facet_normal_matches(const indexed_triangle_set &its, size_t facet, const Vec3f &expected)
{
    return its_face_normal(its, int(facet)).dot(expected) > 0.99f;
}

TEST_CASE("DfmAnalyzer: wall thickness bands on plates", "[DfmAnalyzer]")
{
    struct PlateCase { double width; uint8_t expected; };
    const PlateCase cases[] = {
        { 1.5, dfmThinCritical },  // < 1x nozzle
        { 4.5, dfmThinWarning },   // between 1x and 2x nozzle
        { 7.5, dfmNone },          // > 2x nozzle
    };
    for (const PlateCase &plate : cases) {
        DYNAMIC_SECTION("plate " << plate.width << " mm") {
            const indexed_triangle_set its = its_make_cube(plate.width, 100., 100.);
            const DfmMeasurement       m   = dfm_measure(its);
            const DfmClassification    c   = dfm_classify(m, DfmThresholds{});
            REQUIRE(m.ray_miss_count == 0);
            for (size_t f = 0; f < its.indices.size(); ++ f) {
                const bool is_x_face = facet_normal_matches(its, f, Vec3f(1.f, 0.f, 0.f)) ||
                                       facet_normal_matches(its, f, Vec3f(-1.f, 0.f, 0.f));
                if (is_x_face) {
                    REQUIRE(c.facet_flags[f] == plate.expected);
                    REQUIRE_THAT(double(m.thickness[f]), WithinAbs(plate.width, 0.01));
                } else {
                    REQUIRE((c.facet_flags[f] & (dfmThinCritical | dfmThinWarning)) == 0);
                }
            }
            if (plate.expected != dfmNone) {
                const size_t cat = plate.expected == dfmThinCritical ? 0 : 1;
                REQUIRE(c.stats[cat].facets == 4);
                // Two quads of 100x100 mm
                REQUIRE_THAT(c.stats[cat].area, WithinAbs(2. * 100. * 100., 1.));
            }
        }
    }
}

TEST_CASE("DfmAnalyzer: thick cube flags only its top surface", "[DfmAnalyzer]")
{
    const indexed_triangle_set its = its_make_cube(100., 100., 100.);
    std::atomic<int> last_progress{-1};
    const DfmMeasurement m = dfm_measure(its, {}, [&last_progress](int pct) { last_progress = pct; });
    const DfmClassification c = dfm_classify(m, DfmThresholds{});
    // Vertical 100 mm walls and the bed-contact bottom stay clean; the flat top is the
    // worst inward overhang by definition (90 deg, resting on infill).
    REQUIRE(c.stats[0].facets == 0);
    REQUIRE(c.stats[1].facets == 0);
    REQUIRE(c.stats[2].facets == 0);
    REQUIRE(c.stats[3].facets == 2);
    REQUIRE(c.stats[3].max_overhang_deg == 90);
    REQUIRE_THAT(c.stats[3].area, WithinAbs(100. * 100., 1.));
    REQUIRE_THAT(c.total_area, WithinAbs(6. * 100. * 100., 1.));
    REQUIRE(last_progress.load() == 100);
    REQUIRE(m.ray_miss_count == 0);
    REQUIRE_FALSE(m.cancelled);
}

TEST_CASE("DfmAnalyzer: pyramid distinguishes internal from external overhang", "[DfmAnalyzer]")
{
    // Base 40, height 20: side walls lean exactly 45 degrees from the vertical.
    indexed_triangle_set its = its_make_pyramid(40.f, 20.f);
    // its_make_pyramid()'s base quad is wound facing +Z, i.e. into the solid; flip it
    // so the mesh is a proper solid and the base cannot pollute the overhang counts.
    std::swap(its.indices[0][0], its.indices[0][1]);
    std::swap(its.indices[1][0], its.indices[1][1]);

    SECTION("apex up: walls lean inward -> internal overhang") {
        const DfmMeasurement m = dfm_measure(its);
        const DfmClassification c = dfm_classify(m, DfmThresholds{});
        REQUIRE(c.stats[3].facets == 4);
        REQUIRE(c.stats[3].max_overhang_deg == 45);
        REQUIRE(c.stats[2].facets == 0);
        for (size_t f = 0; f < c.facet_flags.size(); ++ f)
            if (c.facet_flags[f] & dfmOverhangInternal)
                REQUIRE(int(c.overhang_deg[f]) == 45);
    }

    SECTION("apex down: walls lean outward -> external overhang") {
        its_rotate_x(its, 180.f); // admesh rotation helpers take degrees
        const DfmMeasurement m = dfm_measure(its);
        const DfmClassification c = dfm_classify(m, DfmThresholds{});
        REQUIRE(c.stats[2].facets == 4);
        REQUIRE(c.stats[2].max_overhang_deg == 45);
        // The base, now the flat top, is the worst inward overhang (90 deg).
        REQUIRE(c.stats[3].facets == 2);
        REQUIRE(c.stats[3].max_overhang_deg == 90);
    }
}

TEST_CASE("DfmAnalyzer: top surfaces are the worst inward overhang", "[DfmAnalyzer]")
{
    SECTION("flat top of a slab is flagged at 90 deg") {
        const indexed_triangle_set its = its_make_cube(100., 100., 2.);
        const DfmClassification c = dfm_classify(dfm_measure(its), DfmThresholds{});
        // Two top triangles at 90 deg; vertical walls (lean 0) stay clean; the bottom
        // is bed contact.
        REQUIRE(c.stats[3].facets == 2);
        REQUIRE(c.stats[3].max_overhang_deg == 90);
        REQUIRE(c.stats[2].facets == 0);
    }
    SECTION("tilting 15 deg grades the top at 75 and the raised side wall at 15") {
        indexed_triangle_set its = its_make_cube(100., 100., 2.);
        its_rotate_x(its, 15.f);
        const DfmClassification c = dfm_classify(dfm_measure(its), DfmThresholds{});
        // Upward-facing: the top quad now leans 75 deg, the +Y wall 15 deg.
        REQUIRE(c.stats[3].facets == 4);
        REQUIRE(c.stats[3].max_overhang_deg == 75);
        for (size_t f = 0; f < c.facet_flags.size(); ++ f)
            if (c.facet_flags[f] & dfmOverhangInternal) {
                const bool is_15_or_75 = c.overhang_deg[f] == 15 || c.overhang_deg[f] == 75;
                REQUIRE(is_15_or_75);
            }
        // Downward mirror: the bottom at 75, the -Y wall at 15 (both clear the bed band).
        REQUIRE(c.stats[2].facets == 4);
        REQUIRE(c.stats[2].max_overhang_deg == 75);
    }
}

TEST_CASE("DfmAnalyzer: analysis runs in world space", "[DfmAnalyzer]")
{
    SECTION("non-uniform instance scaling makes a cube thin") {
        Model model;
        ModelObject *object = model.add_object();
        object->name = "cube";
        object->add_volume(TriangleMesh(its_make_cube(15., 15., 15.)));
        ModelInstance *instance = object->add_instance();
        instance->set_scaling_factor(Vec3d(0.1, 1., 1.));
        const DfmMeasurement m = dfm_measure_object(*object, 0);
        const DfmClassification c = dfm_classify(m, DfmThresholds{});
        // World-space X extent is 1.5 mm: both X quads are critically thin.
        REQUIRE(c.stats[0].facets == 4);
        REQUIRE(m.volume_ranges.size() == 1);
        REQUIRE(m.volume_ranges.front().second.first == 0);
        REQUIRE(m.volume_ranges.front().second.second == m.facet_count());
        REQUIRE(m.skipped_volumes == 0);
    }
    SECTION("negative volumes are skipped") {
        Model model;
        ModelObject *object = model.add_object();
        object->name = "cube";
        object->add_volume(TriangleMesh(its_make_cube(15., 15., 15.)));
        object->add_volume(TriangleMesh(its_make_cube(5., 5., 5.)), ModelVolumeType::NEGATIVE_VOLUME);
        object->add_instance();
        const DfmMeasurement m = dfm_measure_object(*object, 0);
        REQUIRE(m.skipped_volumes == 1);
        REQUIRE(m.volume_ranges.size() == 1);
    }
    SECTION("overhang is evaluated on rotated normals") {
        indexed_triangle_set its = its_make_cube(20., 20., 20.);
        its_rotate_x(its, 50.f);
        const DfmClassification c = dfm_classify(dfm_measure(its), DfmThresholds{});
        // +Y quad leans 50 deg up, +Z quad 40 deg up; -Y and -Z mirror them downward.
        REQUIRE(c.stats[3].facets == 4);
        REQUIRE(c.stats[3].max_overhang_deg == 50);
        REQUIRE(c.stats[2].facets == 4);
        REQUIRE(c.stats[2].max_overhang_deg == 50);
        for (size_t f = 0; f < c.overhang_deg.size(); ++ f)
            if (c.overhang_deg[f] != DfmClassification::NotOverhang) {
                const bool is_40_or_50 = c.overhang_deg[f] == 40 || c.overhang_deg[f] == 50;
                REQUIRE(is_40_or_50);
            }
    }
}

TEST_CASE("DfmAnalyzer: rays continue through flush sibling volumes", "[DfmAnalyzer]")
{
    // A 2 mm plate laminated flat onto a 30 mm cube: the wall measured from the plate's
    // outer face is 32 mm thick, not 2 mm, because the ray must pass the coincident
    // interface faces at x = 0 into the backing cube.
    indexed_triangle_set merged = its_make_cube(30., 30., 30.);
    indexed_triangle_set plate  = its_make_cube(2., 30., 30.);
    its_translate(plate, stl_vertex(-2.f, 0.f, 0.f));
    its_merge(merged, plate);

    const DfmMeasurement    m = dfm_measure(merged);
    const DfmClassification c = dfm_classify(m, DfmThresholds{});
    size_t outer_faces = 0;
    for (size_t f = 0; f < merged.indices.size(); ++ f) {
        if (! facet_normal_matches(merged, f, Vec3f(-1.f, 0.f, 0.f)))
            continue;
        const its_triangle tri = its_triangle_vertices(merged, f);
        if (tri[0].x() > -1.5f)
            continue; // the cube's own -X face at x = 0, not the plate's outer face
        ++ outer_faces;
        REQUIRE((c.facet_flags[f] & (dfmThinCritical | dfmThinWarning)) == 0);
        REQUIRE_THAT(double(m.thickness[f]), WithinAbs(32., 0.01));
    }
    REQUIRE(outer_faces == 2);
}

TEST_CASE("DfmAnalyzer: open meshes degrade gracefully", "[DfmAnalyzer]")
{
    indexed_triangle_set its = its_make_cube(10., 10., 10.);
    // Remove the whole top: the two bottom facets' rays now escape through the hole.
    for (int f = int(its.indices.size()) - 1; f >= 0; -- f)
        if (its_face_normal(its, f).z() > 0.99f)
            its.indices.erase(its.indices.begin() + f);

    const DfmMeasurement    m = dfm_measure(its);
    const DfmClassification c = dfm_classify(m, DfmThresholds{});
    REQUIRE(m.ray_miss_count == 2);
    // Unmeasured facets stay unflagged: a broken mesh must not light up solid red.
    REQUIRE(count_flagged(c, dfmThinCritical | dfmThinWarning) == 0);
    for (size_t f = 0; f < its.indices.size(); ++ f)
        if (its_face_normal(its, f).z() < -0.99f)
            REQUIRE(m.thickness[f] < 0.f);
}

TEST_CASE("DfmAnalyzer: reclassification does not need a new measurement", "[DfmAnalyzer]")
{
    const indexed_triangle_set its = its_make_cube(4.5, 100., 100.);
    const DfmMeasurement       m   = dfm_measure(its);

    DfmThresholds nozzle3;
    const DfmClassification c3 = dfm_classify(m, nozzle3);
    REQUIRE(c3.stats[0].facets == 0);
    REQUIRE(c3.stats[1].facets == 4);

    DfmThresholds nozzle5 = nozzle3;
    nozzle5.nozzle_diameter = 5.;
    const DfmClassification c5 = dfm_classify(m, nozzle5);
    REQUIRE(c5.stats[0].facets == 4);
    REQUIRE(c5.stats[1].facets == 0);
}
