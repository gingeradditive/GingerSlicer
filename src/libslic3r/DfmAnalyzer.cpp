#include "DfmAnalyzer.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>

#include <tbb/parallel_for.h>

#include "AABBTreeIndirect.hpp"
#include "Model.hpp"
#include "TriangleMesh.hpp"

namespace Slic3r {

DfmObjectSnapshot dfm_snapshot_object(const ModelObject &object, int instance_idx)
{
    DfmObjectSnapshot snapshot;
    assert(instance_idx >= 0 && instance_idx < int(object.instances.size()));
    const Transform3d instance_trafo = object.instances[instance_idx]->get_matrix();
    for (const ModelVolume *volume : object.volumes) {
        if (! volume->is_model_part()) {
            ++ snapshot.skipped_volumes;
            continue;
        }
        indexed_triangle_set its = volume->mesh().its;
        if (its.indices.empty())
            continue;
        // fix_left_handed re-winds faces under mirroring so world normals stay outward.
        // Face count and order are preserved, keeping the offset mapping below valid.
        its_transform(its, Transform3d(instance_trafo * volume->get_matrix()), /*fix_left_handed=*/true);
        const size_t first_face = snapshot.world_its.indices.size();
        its_merge(snapshot.world_its, its);
        snapshot.volume_ranges.emplace_back(volume->id(),
            std::make_pair(first_face, snapshot.world_its.indices.size()));
    }
    return snapshot;
}

DfmMeasurement dfm_measure(const indexed_triangle_set     &its,
                           const std::function<bool()>    &cancel,
                           const std::function<void(int)> &progress)
{
    DfmMeasurement m;
    const size_t facet_count = its.indices.size();
    m.thickness.assign(facet_count, -1.f);
    m.normal_z.assign(facet_count, 0.f);
    m.area.assign(facet_count, 0.f);
    m.z_max.assign(facet_count, 0.f);
    if (facet_count == 0)
        return m;

    double min_z = std::numeric_limits<double>::max();
    for (const stl_vertex &v : its.vertices)
        min_z = std::min(min_z, double(v.z()));
    m.min_z = min_z;

    const auto tree = AABBTreeIndirect::build_aabb_tree_over_indexed_triangle_set(its.vertices, its.indices);

    // Stepping the ray origin slightly inside the solid avoids re-hitting the source
    // facet; geometry thinner than this offset is beyond broken for a pellet nozzle.
    constexpr double self_hit_eps   = 1e-3; // mm
    // Hits closer than this are numerical self-intersections through shared edges.
    constexpr double min_hit_t      = 1e-4; // mm
    // Hits within this distance of each other are netted together before testing whether
    // the ray left the solid: flush volumes produce coincident exit + enter face pairs
    // at their interface, and the ray must continue through them into the backing body.
    constexpr double coincident_eps = 1e-4; // mm

    std::atomic<size_t> misses{0};
    std::atomic<size_t> processed{0};
    std::atomic<int>    last_pct{-1};
    std::atomic<bool>   cancelled{false};

    tbb::parallel_for(tbb::blocked_range<size_t>(0, facet_count, 4096),
        [&](const tbb::blocked_range<size_t> &range) {
            if (cancelled.load(std::memory_order_relaxed))
                return;
            if (cancel && cancel()) {
                cancelled.store(true, std::memory_order_relaxed);
                return;
            }
            size_t local_misses = 0;
            std::vector<igl::Hit> hits;
            for (size_t f = range.begin(); f < range.end(); ++ f) {
                const stl_triangle_vertex_indices &face = its.indices[f];
                const Vec3d v0 = its.vertices[face(0)].cast<double>();
                const Vec3d v1 = its.vertices[face(1)].cast<double>();
                const Vec3d v2 = its.vertices[face(2)].cast<double>();
                const Vec3d cross      = (v1 - v0).cross(v2 - v1);
                const double cross_norm = cross.norm();
                if (cross_norm < 1e-12)
                    // Degenerate facet: unmeasured, never flagged.
                    continue;
                const Vec3d normal = cross / cross_norm;
                m.normal_z[f] = float(normal.z());
                m.area[f]     = float(0.5 * cross_norm);
                m.z_max[f]    = float(std::max(v0.z(), std::max(v1.z(), v2.z())));

                // Sample with skewed barycentric weights instead of the exact centroid:
                // on axis-aligned geometry the centroid of a face triangle projects
                // exactly onto the shared diagonal of the mirrored opposite face,
                // producing unstable double edge hits.
                const Vec3d sample = 0.42 * v0 + 0.33 * v1 + 0.25 * v2;
                const Vec3d dir    = - normal;
                const Vec3d origin = sample + dir * self_hit_eps;
                AABBTreeIndirect::intersect_ray_all_hits(its.vertices, its.indices, tree, origin, dir, hits);

                // The origin starts one self_hit_eps inside the solid (depth 1). Walk the
                // hits, tracking how many solids the ray is inside of; local thickness is
                // the distance at which it first reaches open space. This lets the ray
                // continue through interfaces of flush sibling volumes into backing
                // geometry instead of misreporting a laminated plate as thin.
                double thickness = -1.;
                int    depth     = 1;
                size_t i = 0;
                while (i < hits.size()) {
                    if (double(hits[i].t) < min_hit_t) {
                        ++ i;
                        continue;
                    }
                    const double t = double(hits[i].t);
                    // A ray crossing a shared triangle edge is reported once per
                    // triangle: count at most one exit and one enter per coincident
                    // group, so duplicated edge hits cannot fake an extra crossing.
                    bool   has_exit  = false;
                    bool   has_enter = false;
                    size_t j = i;
                    for (; j < hits.size() && double(hits[j].t) <= t + coincident_eps; ++ j) {
                        const double along = its_unnormalized_normal(its, hits[j].id).cast<double>().dot(dir);
                        if (along > 0.)
                            has_exit = true; // exiting a solid
                        else if (along < 0.)
                            has_enter = true; // entering a solid; grazing parallel faces are ignored
                    }
                    if (has_exit)
                        -- depth;
                    if (has_enter)
                        ++ depth;
                    if (depth <= 0) {
                        thickness = t + self_hit_eps;
                        break;
                    }
                    i = j;
                }
                if (thickness < 0.) {
                    // Never reached open space: open or self-intersecting mesh. With no
                    // hit at all count a miss and leave the facet unmeasured ("innocent
                    // until proven guilty"); otherwise the first hit is a defensible
                    // lower bound of the local thickness.
                    bool found = false;
                    for (const igl::Hit &hit : hits)
                        if (double(hit.t) >= min_hit_t) {
                            thickness = double(hit.t) + self_hit_eps;
                            found     = true;
                            break;
                        }
                    if (found)
                        m.thickness[f] = float(thickness);
                    else
                        ++ local_misses;
                } else
                    m.thickness[f] = float(thickness);
            }
            misses.fetch_add(local_misses, std::memory_order_relaxed);
            if (progress) {
                const size_t done = processed.fetch_add(range.size(), std::memory_order_relaxed) + range.size();
                const int pct  = int(done * 100 / facet_count);
                int       prev = last_pct.load(std::memory_order_relaxed);
                if (pct > prev && last_pct.compare_exchange_strong(prev, pct))
                    progress(pct);
            }
        });

    m.ray_miss_count = misses.load();
    m.cancelled      = cancelled.load();
    return m;
}

DfmMeasurement dfm_measure_object(const ModelObject              &object,
                                  int                             instance_idx,
                                  const std::function<bool()>    &cancel,
                                  const std::function<void(int)> &progress)
{
    DfmObjectSnapshot snapshot = dfm_snapshot_object(object, instance_idx);
    DfmMeasurement m  = dfm_measure(snapshot.world_its, cancel, progress);
    m.volume_ranges   = std::move(snapshot.volume_ranges);
    m.skipped_volumes = snapshot.skipped_volumes;
    return m;
}

DfmClassification dfm_classify(const DfmMeasurement &m, const DfmThresholds &t)
{
    DfmClassification c;
    const size_t facet_count = m.facet_count();
    c.facet_flags.assign(facet_count, uint8_t(dfmNone));
    c.overhang_deg.assign(facet_count, DfmClassification::NotOverhang);

    const double thin_critical = t.thin_critical_factor * t.nozzle_diameter;
    const double thin_warning  = t.thin_warning_factor * t.nozzle_diameter;
    const double bed_band_z    = m.min_z + t.bed_band_factor * t.nozzle_diameter;

    for (size_t f = 0; f < facet_count; ++ f) {
        const double area = double(m.area[f]);
        if (area <= 0.)
            continue;
        c.total_area += area;

        uint8_t flags = dfmNone;
        if (const double thickness = double(m.thickness[f]); thickness >= 0.) {
            if (thickness < thin_critical)
                flags |= dfmThinCritical;
            else if (thickness < thin_warning)
                flags |= dfmThinWarning;
        }

        const double nz   = std::clamp(double(m.normal_z[f]), -1., 1.);
        // Lean of the surface from the vertical: 0 = plumb wall, 90 = horizontal.
        const double lean     = std::asin(std::abs(nz)) * (180. / M_PI);
        const int    lean_deg = int(std::min(90l, std::lround(lean)));
        // No angle thresholds: every leaning facet is graded 0..90 so the overlay paints
        // the full gradient. Quantized 0 (plumb within half a degree) stays unflagged.
        bool is_overhang = false;
        if (lean_deg > 0) {
            if (nz < 0.) {
                // Downward-facing: external overhang, unless it is bed contact.
                if (double(m.z_max[f]) > bed_band_z) {
                    flags      |= dfmOverhangExternal;
                    is_overhang = true;
                }
            } else {
                // Upward-facing: on the layers above, the wall bead rests on sparse
                // infill. A flat top (90°) is the worst case of the same problem.
                flags      |= dfmOverhangInternal;
                is_overhang = true;
            }
        }

        c.facet_flags[f] = flags;
        if (is_overhang)
            c.overhang_deg[f] = uint8_t(lean_deg);
        for (size_t cat = 0; cat < DfmCategoryCount; ++ cat)
            if (flags & dfm_category_flag(cat)) {
                ++ c.stats[cat].facets;
                c.stats[cat].area += area;
                if (is_overhang && (dfm_category_flag(cat) & (dfmOverhangExternal | dfmOverhangInternal)))
                    c.stats[cat].max_overhang_deg = std::max(c.stats[cat].max_overhang_deg, lean_deg);
            }
    }
    return c;
}

} // namespace Slic3r
