#ifndef slic3r_DfmAnalyzer_hpp_
#define slic3r_DfmAnalyzer_hpp_

// GingerSlicer "Design for Manufacturing" mesh feasibility analysis for large-scale
// pellet printing. Detects, per facet of the world-space mesh:
//  - walls thinner than 1x nozzle diameter (physically unprintable)
//  - walls thinner than 2x nozzle diameter (an outbound + return wall bead pair does not fit)
//  - external overhangs (downward-facing faces), graded by lean angle 0..90 — no threshold
//  - internal overhangs (upward-tilted walls: the perimeters above them rest on sparse
//    infill, which cannot carry a pellet bead; filament slicers ignore this case), graded
//    0..90 as well — a flat top surface is the worst case (90°) of the same problem, not
//    a separate class
//
// The analysis is split in two passes so that interactive threshold changes stay cheap:
//  - dfm_measure(): expensive, threshold-invariant raw measurement (ray-cast thickness,
//    world normal z, facet area) — run once per mesh/transform on a worker thread.
//  - dfm_classify(): cheap application of DfmThresholds to a DfmMeasurement.

#include <array>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

#include <admesh/stl.h>

#include "ObjectID.hpp"

namespace Slic3r {

class ModelObject;

// All lengths in mm (world space). Overhangs carry no angle thresholds: every leaning
// facet is graded by its lean from the vertical (0 = plumb wall, 90 = horizontal) and the
// overlay paints the full gradient.
struct DfmThresholds
{
    double nozzle_diameter       = 3.0;
    // Thickness below thin_critical_factor * nozzle_diameter cannot be printed at all.
    double thin_critical_factor  = 1.0;
    // Below thin_warning_factor * nozzle_diameter a wall loop (out + return bead) does not fit.
    double thin_warning_factor   = 2.0;
    // Faces whose top vertex lies within bed_band_factor * nozzle_diameter of the mesh
    // bottom are bed contact, not overhangs.
    double bed_band_factor       = 0.5;
};

// Per-facet issue flags; a facet may carry several (e.g. thin and overhanging).
enum DfmIssueFlag : uint8_t {
    dfmNone             = 0,
    dfmThinCritical     = 1 << 0,
    dfmThinWarning      = 1 << 1,
    dfmOverhangExternal = 1 << 2,
    dfmOverhangInternal = 1 << 3,
};

static constexpr size_t DfmCategoryCount = 4;

inline DfmIssueFlag dfm_category_flag(size_t category) { return DfmIssueFlag(1 << category); }

// World-space merged copy of the printable volumes of one object instance, safe to hand
// to a worker thread (no references into the Model). Face order of each source volume is
// preserved inside [first, last), so per-volume facet indices map back by plain offset.
struct DfmObjectSnapshot
{
    indexed_triangle_set world_its;
    // volume id -> [first, last) face range inside world_its
    std::vector<std::pair<ObjectID, std::pair<size_t, size_t>>> volume_ranges;
    // negative volumes / modifiers are not printable geometry and are not analyzed
    size_t skipped_volumes = 0;
};

// Raw measurement of a world-space mesh; all vectors are parallel to the mesh faces.
// Invariant to thresholds and to translation of the mesh.
struct DfmMeasurement
{
    // Local wall thickness in mm measured by ray-casting from the facet centroid along
    // the inward normal; < 0 means the ray left the mesh without a hit (open/broken mesh).
    std::vector<float> thickness;
    // Z component of the world-space unit facet normal.
    std::vector<float> normal_z;
    // World-space facet area in mm^2; 0 for degenerate facets (which are never flagged).
    std::vector<float> area;
    // Highest vertex z of the facet, for the bed-contact test.
    std::vector<float> z_max;
    double min_z          = 0.;
    size_t ray_miss_count = 0;
    // Copied from the DfmObjectSnapshot when measuring a ModelObject.
    std::vector<std::pair<ObjectID, std::pair<size_t, size_t>>> volume_ranges;
    size_t skipped_volumes = 0;
    bool   cancelled       = false;

    size_t facet_count() const { return normal_z.size(); }
};

struct DfmCategoryStats
{
    size_t facets = 0;
    double area   = 0.; // mm^2, world space
    // Worst lean angle found, degrees; only meaningful for the overhang categories.
    int max_overhang_deg = 0;
};

struct DfmClassification
{
    static constexpr uint8_t NotOverhang = 255;

    // DfmIssueFlag bitmask per facet.
    std::vector<uint8_t> facet_flags;
    // Lean from the vertical quantized to 1 degree (0..90) for facets flagged as
    // overhang, NotOverhang otherwise. Drives the per-degree gradient overlay.
    std::vector<uint8_t> overhang_deg;
    // Indexed by the bit position of DfmIssueFlag (0 = thin critical .. 3 = internal overhang).
    std::array<DfmCategoryStats, DfmCategoryCount> stats;
    double total_area = 0.;
};

// Builds the world-space snapshot of `object`'s model-part volumes under instance
// `instance_idx`. Must be called on a thread that owns the Model (typically the UI thread).
DfmObjectSnapshot dfm_snapshot_object(const ModelObject &object, int instance_idx);

// Expensive measure pass; TBB-parallel. `cancel` is polled per block of facets and may be
// called from multiple threads, as may `progress` (monotonic percentage, 0..100).
DfmMeasurement dfm_measure(const indexed_triangle_set     &world_its,
                           const std::function<bool()>    &cancel   = {},
                           const std::function<void(int)> &progress = {});

// Convenience: dfm_snapshot_object + dfm_measure.
DfmMeasurement dfm_measure_object(const ModelObject              &object,
                                  int                             instance_idx,
                                  const std::function<bool()>    &cancel   = {},
                                  const std::function<void(int)> &progress = {});

// Cheap classify pass; re-run freely when thresholds or the nozzle diameter change.
DfmClassification dfm_classify(const DfmMeasurement &measurement, const DfmThresholds &thresholds);

} // namespace Slic3r

#endif // slic3r_DfmAnalyzer_hpp_
