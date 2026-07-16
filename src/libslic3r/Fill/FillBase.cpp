#include <stdio.h>
#include <cstdlib>
#include <atomic>
#include <chrono>
#include <numeric>
#include <optional>

#include <cmath>
#include <boost/log/trivial.hpp>
#include "../AABBTreeLines.hpp"
#include "../ClipperUtils.hpp"
#include "../Clipper2Utils.hpp"
#include "../EdgeGrid.hpp"
#include "../Geometry.hpp"
#include "../Geometry/Circle.hpp"
#include "../Point.hpp"
#include "../PrintConfig.hpp"
#include "../Surface.hpp"
#include "../libslic3r.h"
#include "../VariableWidth.hpp"

#include "FillBase.hpp"
#include "FillConcentric.hpp"
#include "FillHoneycomb.hpp"
#include "Fill3DHoneycomb.hpp"
#include "FillGyroid.hpp"
#include "FillTpmsD.hpp"
#include "FillTpmsFK.hpp"
#include "FillPlanePath.hpp"
#include "FillLine.hpp"
#include "FillRectilinear.hpp"
#include "FillAdaptive.hpp"
#include "FillLightning.hpp"
// BBS: new infill pattern header
#include "FillConcentricInternal.hpp"
#include "FillCrossHatch.hpp"
// #define INFILL_DEBUG_OUTPUT

namespace Slic3r {

//BBS: 0% of sparse_infill_line_width, no anchor at the start of sparse infill
float Fill::infill_anchor = 400;
//BBS: 20mm
float Fill::infill_anchor_max = 20;

Fill* Fill::new_from_type(const InfillPattern type)
{
    switch (type) {
    case ipConcentric:          return new FillConcentric();
    case ipHoneycomb:           return new FillHoneycomb();
    case ipLateralHoneycomb:         return new FillLateralHoneycomb();
    case ip3DHoneycomb:         return new Fill3DHoneycomb();
    case ipGyroid:              return new FillGyroid();
    case ipTpmsD:               return new FillTpmsD();//from creality print
    case ipTpmsFK:              return new FillTpmsFK();
    case ipRectilinear:         return new FillRectilinear();
    case ipAlignedRectilinear:  return new FillAlignedRectilinear();
    case ipCrossHatch:          return new FillCrossHatch();
    case ipMonotonic:           return new FillMonotonic();
    case ipLine:                return new FillLine();
    case ipGrid:                return new FillGrid();
    case ipLateralLattice:           return new FillLateralLattice();
    case ipTriangles:           return new FillTriangles();
    case ipStars:               return new FillStars();
    case ipCubic:               return new FillCubic();
    case ipQuarterCubic:        return new FillQuarterCubic();
    case ipArchimedeanChords:   return new FillArchimedeanChords();
    case ipHilbertCurve:        return new FillHilbertCurve();
    case ipOctagramSpiral:      return new FillOctagramSpiral();
    case ipAdaptiveCubic:       return new FillAdaptive::Filler();
    case ipSupportCubic:        return new FillAdaptive::Filler();
    case ipSupportBase:         return new FillSupportBase();  // simply line fill
    case ipLightning:           return new FillLightning::Filler();
    // BBS: for internal solid infill only
    case ipConcentricInternal:  return new FillConcentricInternal();
    // BBS: for bottom and top surface only
    // Orca: Replace BBS implementation with Prusa implementation
    case ipMonotonicLine:       return new FillMonotonicLines();
    case ipZigZag:              return new FillZigZag();
    case ipCrossZag:            return new FillCrossZag();
    case ipLockedZag:           return new FillLockedZag();
    default: throw Slic3r::InvalidArgument("unknown type");
    }
}

Fill* Fill::new_from_type(const std::string &type)
{
    const t_config_enum_values &enum_keys_map = ConfigOptionEnum<InfillPattern>::get_enum_values();
    t_config_enum_values::const_iterator it = enum_keys_map.find(type);
    return (it == enum_keys_map.end()) ? nullptr : new_from_type(InfillPattern(it->second));
}

// Force initialization of the Fill::use_bridge_flow() internal static map in a thread safe fashion even on compilers
// not supporting thread safe non-static data member initializers.
static bool use_bridge_flow_initializer = Fill::use_bridge_flow(ipGrid);

bool Fill::use_bridge_flow(const InfillPattern type)
{
	static std::vector<unsigned char> cached;
	if (cached.empty()) {
		cached.assign(size_t(ipCount), 0);
		for (size_t i = 0; i < cached.size(); ++ i) {
			auto *fill = Fill::new_from_type((InfillPattern)i);
			cached[i] = fill->use_bridge_flow();
			delete fill;
		}
	}
	return cached[type] != 0;
}

Polylines Fill::fill_surface(const Surface *surface, const FillParams &params)
{
    // Perform offset.
    Slic3r::ExPolygons expp = offset_ex(surface->expolygon, float(scale_(this->overlap - 0.5 * this->spacing)));
    // Create the infills for each of the regions.
    Polylines polylines_out;
    for (size_t i = 0; i < expp.size(); ++ i)
        _fill_surface_single(
            params,
            surface->thickness_layers,
            _infill_direction(surface),
            std::move(expp[i]),
            polylines_out);
    return polylines_out;
}

ThickPolylines Fill::fill_surface_arachne(const Surface* surface, const FillParams& params)
{
    // Perform offset.
    Slic3r::ExPolygons expp = offset_ex(surface->expolygon, float(scale_(this->overlap - 0.5 * this->spacing)));
    // Create the infills for each of the regions.
    ThickPolylines thick_polylines_out;
    for (ExPolygon& expoly : expp)
        _fill_surface_single(params, surface->thickness_layers, _infill_direction(surface), std::move(expoly), thick_polylines_out);
    return thick_polylines_out;
}

// BBS: this method is used to fill the ExtrusionEntityCollection. It call fill_surface by default
void Fill::fill_surface_extrusion(const Surface* surface, const FillParams& params, ExtrusionEntitiesPtr& out)
{
    Polylines polylines;
    ThickPolylines thick_polylines;
    try {
        if (params.use_arachne)
            thick_polylines = this->fill_surface_arachne(surface, params);
        else
            polylines = this->fill_surface(surface, params);
    }
    catch (InfillFailedException&) {}

    // Ginger single-path (pellet): drop a surface whose whole fill is shorter than 2.5 line widths.
    // Sliver surfaces (0.3-6mm crescents where a sloped face meets the wall) each cost a dedicated
    // 25-150mm travel and a start/stop blob, and a sub-bead-length extrusion never forms anyway
    // ("non verranno mai") - skipping them entirely beats printing them badly. 2.5 widths of fill
    // is roughly a 26mm^2 patch: cosmetically invisible, structurally nothing.
    if (params.connect_polygons && params.flow.scaled_width() > 0) {
        double total = 0.;
        for (const Polyline &pl : polylines)
            total += pl.length();
        for (const ThickPolyline &pl : thick_polylines)
            total += pl.length();
        if (total > 0. && total < 2.5 * double(params.flow.scaled_width())) {
            polylines.clear();
            thick_polylines.clear();
        }
    }

    if (!polylines.empty() || !thick_polylines.empty()) {
        // calculate actual flow from spacing (which might have been adjusted by the infill
        // pattern generator)
        double flow_mm3_per_mm = params.flow.mm3_per_mm();
        double flow_width = params.flow.width();
        if (params.using_internal_flow) {
            // if we used the internal flow we're not doing a solid infill
            // so we can safely ignore the slight variation that might have
            // been applied to f->spacing
        }
        else {
            Flow new_flow = params.flow.with_spacing(this->spacing);
            flow_mm3_per_mm = new_flow.mm3_per_mm();
            flow_width = new_flow.width();
        }
        // Save into layer.
        ExtrusionEntityCollection* eec = nullptr;
        out.push_back(eec = new ExtrusionEntityCollection());
        // Only concentric fills are not sorted.
        // Ginger single-path: a connected fill (connect_polygons, single_path_mode) is one continuous
        // path per surface, so for the monotonic fillers there is no line order left to protect and
        // no_sort would only make the collection non-reversible: the chainer would be forced to enter
        // solid/top/bottom at its fixed first end, paying up to a full region-length approach travel
        // (measured 320mm on the real part), and every following surface would start from the wrong
        // side in cascade. Concentric keeps no_sort (ring order must stay outer-to-inner).
        eec->no_sort = this->no_sort() && ! (params.connect_polygons && this->reversible_when_connected());
        size_t idx   = eec->entities.size();
        if (params.use_arachne) {
            Flow new_flow = params.flow.with_spacing(float(this->spacing));
            variable_width(thick_polylines, params.extrusion_role, new_flow, eec->entities);
            thick_polylines.clear();
        }
        else if (params.connect_polygons) {
            // Cura-style single-path infill: a fully connected path closed by connect_infill() is emitted as
            // an ExtrusionLoop. GCode::extrude_loop() splits a non-perimeter loop at the point nearest to the
            // current position (the end of the last wall), so the infill starts right at the wall seam with
            // no wall->infill travel move.
            extrusion_entities_append_loops_and_paths(
                eec->entities, std::move(polylines),
                params.extrusion_role,
                flow_mm3_per_mm, float(flow_width), params.flow.height());
        }
        else {
            extrusion_entities_append_paths(
                eec->entities, std::move(polylines),
                params.extrusion_role,
                flow_mm3_per_mm, float(flow_width), params.flow.height());
        }
        if (!params.can_reverse) {
            for (size_t i = idx; i < eec->entities.size(); i++)
                eec->entities[i]->set_reverse();
        }

        // Orca: run gap fill
        this->_create_gap_fill(surface, params, eec);
    }
}

// Orca: Dedicated function to calculate gap fill lines for the provided surface, according to the print object parameters
// and append them to the out ExtrusionEntityCollection.
void Fill::_create_gap_fill(const Surface* surface, const FillParams& params, ExtrusionEntityCollection* out){

    //Orca: just to be safe, check against null pointer for the print object config and if NULL return.
    if (this->print_object_config == nullptr) return;

    // Orca: Enable gap fill as per the user preference. Return early if gap fill is to not be applied.
    if ((this->print_object_config->gap_fill_target.value == gftNowhere) ||
        (surface->surface_type == stInternalSolid && this->print_object_config->gap_fill_target.value != gftEverywhere))
        return;

    Flow new_flow = params.flow;
    ExPolygons unextruded_areas;
    unextruded_areas = diff_ex(this->no_overlap_expolygons, union_ex(out->polygons_covered_by_spacing(10)));
    ExPolygons gapfill_areas = union_ex(unextruded_areas);
    if (!this->no_overlap_expolygons.empty())
        gapfill_areas = intersection_ex(gapfill_areas, this->no_overlap_expolygons);

    if (gapfill_areas.size() > 0 && params.density >= 1) {
        double min = 0.2 * new_flow.scaled_spacing() * (1 - INSET_OVERLAP_TOLERANCE);
        double max = 2. * new_flow.scaled_spacing();
        ExPolygons gaps_ex = diff_ex(
                                     opening_ex(gapfill_areas, float(min / 2.)),
                                     offset2_ex(gapfill_areas, -float(max / 2.), float(max / 2. + ClipperSafetyOffset)));
        //BBS: sort the gap_ex to avoid mess travel
        Points ordering_points;
        ordering_points.reserve(gaps_ex.size());
        ExPolygons gaps_ex_sorted;
        gaps_ex_sorted.reserve(gaps_ex.size());
        for (const ExPolygon &ex : gaps_ex)
            ordering_points.push_back(ex.contour.first_point());
        std::vector<Points::size_type> order2 = chain_points(ordering_points);
        for (size_t i : order2)
            gaps_ex_sorted.emplace_back(std::move(gaps_ex[i]));

        ThickPolylines polylines;
        for (ExPolygon& ex : gaps_ex_sorted) {
            //BBS: Use DP simplify to avoid duplicated points and accelerate medial-axis calculation as well.
            ex.douglas_peucker(SCALED_RESOLUTION * 0.1);
            ex.medial_axis(min, max, &polylines);
        }

        if (!polylines.empty() && !is_bridge(params.extrusion_role)) {
            polylines.erase(std::remove_if(polylines.begin(), polylines.end(),
                                           [&](const ThickPolyline& p) {
                return p.length() < scale_(params.config->filter_out_gap_fill.value);
            }), polylines.end());

            ExtrusionEntityCollection gap_fill;
            variable_width(polylines, erGapFill, params.flow, gap_fill.entities);
            auto gap = std::move(gap_fill.entities);
            out->append(gap);
        }
    }
}

// Calculate a new spacing to fill width with possibly integer number of lines,
// the first and last line being centered at the interval ends.
// This function possibly increases the spacing, never decreases,
// and for a narrow width the increase in spacing may become severe,
// therefore the adjustment is limited to 20% increase.
coord_t Fill::_adjust_solid_spacing(const coord_t width, const coord_t distance)
{
    assert(width >= 0);
    assert(distance > 0);
    // floor(width / distance)
    const auto  number_of_intervals = coord_t((width - EPSILON) / distance);
    coord_t     distance_new        = (number_of_intervals == 0) ?
        distance :
        coord_t((width - EPSILON) / number_of_intervals);
    const coordf_t factor = coordf_t(distance_new) / coordf_t(distance);
    assert(factor > 1. - 1e-5);
    // How much could the extrusion width be increased? By 20%.
    const coordf_t factor_max = 1.2;
    if (factor > factor_max)
        distance_new = coord_t(floor((coordf_t(distance) * factor_max + 0.5)));
    return distance_new;
}

// Returns orientation of the infill and the reference point of the infill pattern.
// For a normal print, the reference point is the center of a bounding box of the STL.
std::pair<float, Point> Fill::_infill_direction(const Surface *surface) const
{
    // set infill angle
    float out_angle = this->angle;

	if (out_angle == FLT_MAX) {
		//FIXME Vojtech: Add a warning?
        printf("Using undefined infill angle\n");
        out_angle = 0.f;
    }

    // Bounding box is the bounding box of a perl object Slic3r::Print::Object (c++ object Slic3r::PrintObject)
    // The bounding box is only undefined in unit tests.
    Point out_shift = empty(this->bounding_box) ?
    	surface->expolygon.contour.bounding_box().center() :
        this->bounding_box.center();

#if 0
    if (empty(this->bounding_box)) {
        printf("Fill::_infill_direction: empty bounding box!");
    } else {
        printf("Fill::_infill_direction: reference point %d, %d\n", out_shift.x, out_shift.y);
    }
#endif

    if (surface->bridge_angle >= 0) {
	    // use bridge angle
		//FIXME Vojtech: Add a debugf?
        // Slic3r::debugf "Filling bridge with angle %d\n", rad2deg($surface->bridge_angle);
#ifdef SLIC3R_DEBUG
        printf("Filling bridge with angle %f\n", surface->bridge_angle);
#endif /* SLIC3R_DEBUG */
        out_angle = float(surface->bridge_angle);
    } else if (this->layer_id != size_t(-1)) {
        // alternate fill direction
        //Orca: if template angle is not empty, don't apply layer angle
        if(!is_using_template_angle) 
            out_angle += this->_layer_angle(this->layer_id / surface->thickness_layers);
    } else {
//    	printf("Layer_ID undefined!\n");
    }

    out_angle += float(M_PI/2.);
    return std::pair<float, Point>(out_angle, out_shift);
}

// A single T joint of an infill line to a closed contour or one of its holes.
struct ContourIntersectionPoint {
    // Contour and point on a contour where an infill line is connected to.
    size_t                      contour_idx;
    size_t                      point_idx;
    // Eucleidean parameter of point_idx along its contour.
    double                      param;
    // Other intersection points along the same contour. If there is only a single T-joint on a contour
    // with an intersection line, then the prev_on_contour and next_on_contour remain nulls.
    ContourIntersectionPoint*   prev_on_contour { nullptr };
    ContourIntersectionPoint*   next_on_contour { nullptr };
    // Length of the contour not yet allocated to some extrusion path going back (clockwise), or masked out by some overlapping infill line.
    double                      contour_not_taken_length_prev { std::numeric_limits<double>::max() };
    // Length of the contour not yet allocated to some extrusion path going forward (counter-clockwise), or masked out by some overlapping infill line.
    double                      contour_not_taken_length_next { std::numeric_limits<double>::max() };
    // End point is consumed if an infill line connected to this T-joint was already connected left or right along the contour,
    // or if the infill line was processed, but it was not possible to connect it left or right along the contour.
    bool                        consumed { false };
    // Whether the contour was trimmed by an overlapping infill line, or whether part of this contour was connected to some infill line.
    bool                        prev_trimmed { false };
    bool                        next_trimmed { false };

    void                        consume_prev() { this->contour_not_taken_length_prev = 0.; this->prev_trimmed = true; this->consumed = true; }
    void                        consume_next() { this->contour_not_taken_length_next = 0.; this->next_trimmed = true; this->consumed = true; }

    void                        trim_prev(const double new_len) {
        if (new_len < this->contour_not_taken_length_prev) {
            this->contour_not_taken_length_prev = new_len;
            this->prev_trimmed = true;
        }
    }
    void                        trim_next(const double new_len) {
        if (new_len < this->contour_not_taken_length_next) {
            this->contour_not_taken_length_next = new_len;
            this->next_trimmed = true;
        }
    }

    // The end point of an infill line connected to this T-joint was not processed yet and a piece of the contour could be extruded going backwards.
    bool                        could_take_prev() const throw() { return ! this->consumed && this->contour_not_taken_length_prev > SCALED_EPSILON; }
    // The end point of an infill line connected to this T-joint was not processed yet and a piece of the contour could be extruded going forward.
    bool                        could_take_next() const throw() { return ! this->consumed && this->contour_not_taken_length_next > SCALED_EPSILON; }

    // Could extrude a complete segment from this to this->prev_on_contour.
    bool                        could_connect_prev() const throw()
        { return ! this->consumed && this->prev_on_contour != this && ! this->prev_on_contour->consumed && ! this->prev_trimmed && ! this->prev_on_contour->next_trimmed; }
    // Could extrude a complete segment from this to this->next_on_contour.
    bool                        could_connect_next() const throw()
        { return ! this->consumed && this->next_on_contour != this && ! this->next_on_contour->consumed && ! this->next_trimmed && ! this->next_on_contour->prev_trimmed; }
};

// Distance from param1 to param2 when going counter-clockwise.
static inline double closed_contour_distance_ccw(double param1, double param2, double contour_length)
{
    assert(param1 >= 0. && param1 <= contour_length);
    assert(param2 >= 0. && param2 <= contour_length);
    double d = param2 - param1;
    if (d < 0.)
        d += contour_length;
    return d;
}

// Distance from param1 to param2 when going clockwise.
static inline double closed_contour_distance_cw(double param1, double param2, double contour_length)
{
    return closed_contour_distance_ccw(param2, param1, contour_length);
}

// Length along the contour from cp1 to cp2 going counter-clockwise.
double path_length_along_contour_ccw(const ContourIntersectionPoint *cp1, const ContourIntersectionPoint *cp2, double contour_length)
{
    assert(cp1 != nullptr);
    assert(cp2 != nullptr);
    assert(cp1->contour_idx == cp2->contour_idx);
    assert(cp1 != cp2);
    return closed_contour_distance_ccw(cp1->param, cp2->param, contour_length);
}

// Lengths along the contour from cp1 to cp2 going CCW and going CW.
std::pair<double, double> path_lengths_along_contour(const ContourIntersectionPoint *cp1, const ContourIntersectionPoint *cp2, double contour_length)
{
    // Zero'th param is the length of the contour.
    double param_lo  = cp1->param;
    double param_hi  = cp2->param;
    assert(param_lo >= 0. && param_lo <= contour_length);
    assert(param_hi >= 0. && param_hi <= contour_length);
    bool  reversed  = false;
    if (param_lo > param_hi) {
        std::swap(param_lo, param_hi);
        reversed = true;
    }
    auto out = std::make_pair(param_hi - param_lo, param_lo + contour_length - param_hi);
    if (reversed)
        std::swap(out.first, out.second);
    return out;
}

// Add contour points from interval (idx_start, idx_end> to polyline.
static inline void take_cw_full(Polyline &pl, const Points &contour, size_t idx_start, size_t idx_end)
{
    assert(! pl.empty() && pl.points.back() == contour[idx_start]);
    size_t i = (idx_start == 0) ? contour.size() - 1 : idx_start - 1;
    while (i != idx_end) {
        pl.points.emplace_back(contour[i]);
        if (i == 0)
            i = contour.size();
        -- i;
    }
    pl.points.emplace_back(contour[i]);
}

// Add contour points from interval (idx_start, idx_end> to polyline, limited by the Eucleidean length taken.
static inline double take_cw_limited(Polyline &pl, const Points &contour, const std::vector<double> &params, size_t idx_start, size_t idx_end, double length_to_take)
{
    // If appending to an infill line, then the start point of a perimeter line shall match the end point of an infill line.
    assert(pl.empty() || pl.points.back() == contour[idx_start]);
    assert(contour.size() + 1 == params.size());
    assert(length_to_take > SCALED_EPSILON);
    // Length of the contour.
    double length = params.back();
    // Parameter (length from contour.front()) for the first point.
    double p0     = params[idx_start];
    // Current (2nd) point of the contour.
    size_t i      = (idx_start == 0) ? contour.size() - 1 : idx_start - 1;
    // Previous point of the contour.
    size_t iprev  = idx_start;
    // Length of the contour curve taken for iprev.
    double lprev  = 0.;

    for (;;) {
        double l = closed_contour_distance_cw(p0, params[i], length);
        if (l >= length_to_take) {
            // Trim the last segment.
            double t = double(length_to_take - lprev) / (l - lprev);
            pl.points.emplace_back(lerp(contour[iprev], contour[i], t));
            return length_to_take;
        }
        // Continue with the other segments.
        pl.points.emplace_back(contour[i]);
        if (i == idx_end)
            return l;
        iprev = i;
        lprev = l;
        if (i == 0)
            i = contour.size();
        -- i;
    }
    assert(false);
    return 0;
}

// Add contour points from interval (idx_start, idx_end> to polyline.
static inline void take_ccw_full(Polyline &pl, const Points &contour, size_t idx_start, size_t idx_end)
{
    assert(! pl.empty() && pl.points.back() == contour[idx_start]);
    size_t i = idx_start;
    if (++ i == contour.size())
        i = 0;
    while (i != idx_end) {
        pl.points.emplace_back(contour[i]);
        if (++ i == contour.size())
            i = 0;
    }
    pl.points.emplace_back(contour[i]);
}

// Add contour points from interval (idx_start, idx_end> to polyline, limited by the Eucleidean length taken.
// Returns length of the contour taken.
static inline double take_ccw_limited(Polyline &pl, const Points &contour, const std::vector<double> &params, size_t idx_start, size_t idx_end, double length_to_take)
{
    // If appending to an infill line, then the start point of a perimeter line shall match the end point of an infill line.
    assert(pl.empty() || pl.points.back() == contour[idx_start]);
    assert(contour.size() + 1 == params.size());
    assert(length_to_take > SCALED_EPSILON);
    // Length of the contour.
    double length = params.back();
    // Parameter (length from contour.front()) for the first point.
    double p0     = params[idx_start];
    // Current (2nd) point of the contour.
    size_t i      = idx_start;
    if (++ i == contour.size())
        i = 0;
    // Previous point of the contour.
    size_t iprev  = idx_start;
    // Length of the contour curve taken at iprev.
    double lprev  = 0;
    for (;;) {
        double l = closed_contour_distance_ccw(p0, params[i], length);
        if (l >= length_to_take) {
            // Trim the last segment.
            double t = double(length_to_take - lprev) / (l - lprev);
            pl.points.emplace_back(lerp(contour[iprev], contour[i], t));
            return length_to_take;
        }
        // Continue with the other segments.
        pl.points.emplace_back(contour[i]);
        if (i == idx_end)
            return l;
        iprev = i;
        lprev = l;
        if (++ i == contour.size())
            i = 0;
    }
    assert(false);
    return 0;
}

// Connect end of pl1 to the start of pl2 using the perimeter contour.
// If clockwise, then a clockwise segment from idx_start to idx_end is taken, otherwise a counter-clockwise segment is being taken.
static void take(Polyline &pl1, const Polyline &pl2, const Points &contour, size_t idx_start, size_t idx_end, bool clockwise)
{
#ifndef NDEBUG
	assert(idx_start != idx_end);
    assert(pl1.size() >= 2);
    assert(pl2.size() >= 2);
#endif /* NDEBUG */

	{
		// Reserve memory at pl1 for the connecting contour and pl2.
		int new_points = int(idx_end) - int(idx_start) - 1;
		if (new_points < 0)
			new_points += int(contour.size());
		pl1.points.reserve(pl1.points.size() + size_t(new_points) + pl2.points.size());
	}

	if (clockwise)
        take_cw_full(pl1, contour, idx_start, idx_end);
	else
        take_ccw_full(pl1, contour, idx_start, idx_end);

    pl1.points.insert(pl1.points.end(), pl2.points.begin() + 1, pl2.points.end());
}

static void take(Polyline &pl1, const Polyline &pl2, const Points &contour, ContourIntersectionPoint *cp_start, ContourIntersectionPoint *cp_end, bool clockwise)
{
    assert(cp_start->prev_on_contour != nullptr);
    assert(cp_start->next_on_contour != nullptr);
    assert(cp_end  ->prev_on_contour != nullptr);
    assert(cp_end  ->next_on_contour != nullptr);
    assert(cp_start != cp_end);

    take(pl1, pl2, contour, cp_start->point_idx, cp_end->point_idx, clockwise);

    // Mark the contour segments in between cp_start and cp_end as consumed.
    if (clockwise)
        std::swap(cp_start, cp_end);
    if (cp_start->next_on_contour != cp_end)
        for (auto *cp = cp_start->next_on_contour; cp->next_on_contour != cp_end; cp = cp->next_on_contour) {
            cp->consume_prev();
            cp->consume_next();
        }
    cp_start->consume_next();
    cp_end->consume_prev();
}

static void take_limited(
    Polyline &pl1, const Points &contour, const std::vector<double> &params,
    ContourIntersectionPoint *cp_start, ContourIntersectionPoint *cp_end, bool clockwise, double take_max_length, double line_half_width)
{
#ifndef NDEBUG
    // This is a valid case, where a single infill line connect to two different contours (outer contour + hole or two holes).
//    assert(cp_start != cp_end);
    assert(cp_start->prev_on_contour != nullptr);
    assert(cp_start->next_on_contour != nullptr);
    assert(cp_end  ->prev_on_contour != nullptr);
    assert(cp_end  ->next_on_contour != nullptr);
    assert(pl1.size() >= 2);
    assert(contour.size() + 1 == params.size());
#endif /* NDEBUG */

    if (! (clockwise ? cp_start->could_take_prev() : cp_start->could_take_next()))
        return;

    assert(pl1.points.front() == contour[cp_start->point_idx] || pl1.points.back() == contour[cp_start->point_idx]);
    bool        add_at_start = pl1.points.front() == contour[cp_start->point_idx];
    Points      pl_tmp;
    if (add_at_start) {
        pl_tmp = std::move(pl1.points);
        pl1.points.clear();
    }

    {
        // Reserve memory at pl1 for the perimeter segment.
        // Pessimizing - take the complete segment.
        int new_points = int(cp_end->point_idx) - int(cp_start->point_idx) - 1;
        if (new_points < 0)
            new_points += int(contour.size());
        pl1.points.reserve(pl1.points.size() + pl_tmp.size() + size_t(new_points));
    }

    double length = params.back();
    double length_to_go = take_max_length;
    cp_start->consumed = true;
    if (cp_start == cp_end) {
        length_to_go = std::max(0., std::min(length_to_go, length - line_half_width));
        length_to_go = std::min(length_to_go, clockwise ? cp_start->contour_not_taken_length_prev : cp_start->contour_not_taken_length_next);
        cp_start->consume_prev();
        cp_start->consume_next();
        if (length_to_go > SCALED_EPSILON)
            clockwise ?
                take_cw_limited (pl1, contour, params, cp_start->point_idx, cp_start->point_idx, length_to_go) :
                take_ccw_limited(pl1, contour, params, cp_start->point_idx, cp_start->point_idx, length_to_go);
    } else if (clockwise) {
        // Going clockwise from cp_start to cp_end.
        assert(cp_start != cp_end);
        for (ContourIntersectionPoint *cp = cp_start; cp != cp_end; cp = cp->prev_on_contour) {
            // Length of the segment from cp to cp->prev_on_contour.
            double l = closed_contour_distance_cw(cp->param, cp->prev_on_contour->param, length);
            length_to_go = std::min(length_to_go, cp->contour_not_taken_length_prev);
            //if (cp->prev_on_contour->consumed)
                // Don't overlap with an already extruded infill line.
                length_to_go = std::max(0., std::min(length_to_go, l - line_half_width));
            cp->consume_prev();
            if (l >= length_to_go) {
                if (length_to_go > SCALED_EPSILON) {
                    cp->prev_on_contour->trim_next(l - length_to_go);
                    take_cw_limited(pl1, contour, params, cp->point_idx, cp->prev_on_contour->point_idx, length_to_go);
                }
                break;
            } else {
                cp->prev_on_contour->trim_next(0.);
                take_cw_full(pl1, contour, cp->point_idx, cp->prev_on_contour->point_idx);
                length_to_go -= l;
            }
        }
    } else {
        assert(cp_start != cp_end);
        for (ContourIntersectionPoint *cp = cp_start; cp != cp_end; cp = cp->next_on_contour) {
            double l = closed_contour_distance_ccw(cp->param, cp->next_on_contour->param, length);
            length_to_go = std::min(length_to_go, cp->contour_not_taken_length_next);
            //if (cp->next_on_contour->consumed)
                // Don't overlap with an already extruded infill line.
                length_to_go = std::max(0., std::min(length_to_go, l - line_half_width));
            cp->consume_next();
            if (l >= length_to_go) {
                if (length_to_go > SCALED_EPSILON) {
                    cp->next_on_contour->trim_prev(l - length_to_go);
                    take_ccw_limited(pl1, contour, params, cp->point_idx, cp->next_on_contour->point_idx, length_to_go);
                }
                break;
            } else {
                cp->next_on_contour->trim_prev(0.);
                take_ccw_full(pl1, contour, cp->point_idx, cp->next_on_contour->point_idx);
                length_to_go -= l;
            }
        }
    }

    if (add_at_start) {
        pl1.reverse();
        append(pl1.points, pl_tmp);
    }
}

// Return an index of start of a segment and a point of the clipping point at distance from the end of polyline.
struct SegmentPoint {
	// Segment index, defining a line <idx_segment, idx_segment + 1).
	size_t idx_segment = std::numeric_limits<size_t>::max();
	// Parameter of point in <0, 1) along the line <idx_segment, idx_segment + 1)
	double t;
	Vec2d  point;

	bool valid() const { return idx_segment != std::numeric_limits<size_t>::max(); }
};

static inline SegmentPoint clip_start_segment_and_point(const Points &polyline, double distance)
{
	assert(polyline.size() >= 2);
	assert(distance > 0.);
	// Initialized to "invalid".
	SegmentPoint out;
	if (polyline.size() >= 2) {
	    Vec2d pt_prev = polyline.front().cast<double>();
        for (size_t i = 1; i < polyline.size(); ++ i) {
			Vec2d pt = polyline[i].cast<double>();
			Vec2d v = pt - pt_prev;
	        double l = v.norm();
	        if (l > distance) {
	        	out.idx_segment = i - 1;
	        	out.t 			= distance / l;
	        	out.point 		= pt_prev + out.t * v;
	            break;
	        }
	        distance -= l;
	        pt_prev = pt;
	    }
	}
	return out;
}

static inline SegmentPoint clip_end_segment_and_point(const Points &polyline, double distance)
{
	assert(polyline.size() >= 2);
	assert(distance > 0.);
	// Initialized to "invalid".
	SegmentPoint out;
	if (polyline.size() >= 2) {
	    Vec2d pt_next = polyline.back().cast<double>();
		for (int i = int(polyline.size()) - 2; i >= 0; -- i) {
			Vec2d pt = polyline[i].cast<double>();
			Vec2d v = pt - pt_next;
	        double l = v.norm();
	        if (l > distance) {
	        	out.idx_segment = i;
	        	out.t 			= distance / l;
	        	out.point 		= pt_next + out.t * v;
				// Store the parameter referenced to the starting point of a segment.
				out.t			= 1. - out.t;
	            break;
	        }
	        distance -= l;
	        pt_next = pt;
	    }
	}
	return out;
}

// Calculate intersection of a line with a thick segment.
// Returns Eucledian parameters of the line / thick segment overlap.
static inline bool line_rounded_thick_segment_collision(
    const Vec2d &line_a,    const Vec2d &line_b,
    const Vec2d &segment_a, const Vec2d &segment_b, const double offset,
    std::pair<double, double> &out_interval)
{
    const Vec2d  line_v0   = line_b - line_a;
    double       lv        = line_v0.squaredNorm();

    const Vec2d  segment_v = segment_b - segment_a;
    const double segment_l = segment_v.norm();
    const double offset2   = offset * offset;

    bool intersects = false;
    if (lv < SCALED_EPSILON * SCALED_EPSILON)
    {
        // Very short line vector. Just test whether the center point is inside the offset line.
        Vec2d lpt = 0.5 * (line_a + line_b);
        if (segment_l > SCALED_EPSILON) {
            intersects = line_alg::distance_to_squared(Linef{ segment_a, segment_b }, lpt) < offset2;
        } else
            intersects = (0.5 * (segment_a + segment_b) - lpt).squaredNorm() < offset2;
        if (intersects) {
            out_interval.first = 0.;
            out_interval.second = sqrt(lv);
        }
    }
    else
    {
        // Output interval.
        double tmin = std::numeric_limits<double>::max();
        double tmax = -tmin;
        auto extend_interval = [&tmin, &tmax](double atmin, double atmax) {
            tmin = std::min(tmin, atmin);
            tmax = std::max(tmax, atmax);
        };

        // Intersections with the inflated segment end points.
        auto ray_circle_intersection_interval_extend = [&extend_interval](const Vec2d &segment_pt, const double offset2, const Vec2d &line_pt, const Vec2d &line_vec) {
            std::pair<Vec2d, Vec2d> pts;
            Vec2d  p0 = line_pt - segment_pt;
            double lv2 = line_vec.squaredNorm();
            if (Geometry::ray_circle_intersections_r2_lv2_c(offset2, line_vec.y(), - line_vec.x(), lv2, - line_vec.y() * p0.x() + line_vec.x() * p0.y(), pts)) {
                double tmin = (pts.first  - p0).dot(line_vec) / lv2;
                double tmax = (pts.second - p0).dot(line_vec) / lv2;
                if (tmin > tmax)
                    std::swap(tmin, tmax);
                tmin = std::max(tmin, 0.);
                tmax = std::min(tmax, 1.);
                if (tmin <= tmax)
                    extend_interval(tmin, tmax);
            }
        };

        // Intersections with the inflated segment.
        if (segment_l > SCALED_EPSILON) {
            ray_circle_intersection_interval_extend(segment_a, offset2, line_a, line_v0);
            ray_circle_intersection_interval_extend(segment_b, offset2, line_a, line_v0);
            // Clip the line segment transformed into a coordinate space of the segment,
            // where the segment spans (0, 0) to (segment_l, 0).
            const Vec2d dir_x = segment_v / segment_l;
            const Vec2d dir_y(- dir_x.y(), dir_x.x());
            const Vec2d line_p0(line_a - segment_a);
            std::pair<double, double> interval;
            if (Geometry::liang_barsky_line_clipping_interval(
                    Vec2d(line_p0.dot(dir_x), line_p0.dot(dir_y)),
                    Vec2d(line_v0.dot(dir_x), line_v0.dot(dir_y)),
                    BoundingBoxf(Vec2d(0., - offset), Vec2d(segment_l, offset)),
                    interval))
                extend_interval(interval.first, interval.second);
        } else
            ray_circle_intersection_interval_extend(0.5 * (segment_a + segment_b), offset, line_a, line_v0);

        intersects = tmin <= tmax;
        if (intersects) {
            lv = sqrt(lv);
            out_interval.first  = tmin * lv;
            out_interval.second = tmax * lv;
        }
    }

#if 0
    {
        BoundingBox bbox;
        bbox.merge(line_a.cast<coord_t>());
        bbox.merge(line_a.cast<coord_t>());
        bbox.merge(segment_a.cast<coord_t>());
        bbox.merge(segment_b.cast<coord_t>());
        static int iRun = 0;
        ::Slic3r::SVG svg(debug_out_path("%s-%03d.svg", "line-thick-segment-intersect", iRun ++), bbox);
        svg.draw(Line(line_a.cast<coord_t>(), line_b.cast<coord_t>()), "black");
        svg.draw(Line(segment_a.cast<coord_t>(), segment_b.cast<coord_t>()), "blue", offset * 2.);
        svg.draw(segment_a.cast<coord_t>(), "blue", offset);
        svg.draw(segment_b.cast<coord_t>(), "blue", offset);
        svg.draw(Line(segment_a.cast<coord_t>(), segment_b.cast<coord_t>()), "black");
        if (intersects)
            svg.draw(Line((line_a + (line_b - line_a).normalized() * out_interval.first).cast<coord_t>(),
                          (line_a + (line_b - line_a).normalized() * out_interval.second).cast<coord_t>()), "red");
    }
#endif

    return intersects;
}

#ifndef NDEBUG
static inline bool inside_interval(double low, double high, double p)
{
    return p >= low && p <= high;
}

static inline bool interval_inside_interval(double outer_low, double outer_high, double inner_low, double inner_high, double epsilon)
{
    outer_low -= epsilon;
    outer_high += epsilon;
    return inside_interval(outer_low, outer_high, inner_low) && inside_interval(outer_low, outer_high, inner_high);
}

static inline bool cyclic_interval_inside_interval(double outer_low, double outer_high, double inner_low, double inner_high, double length)
{
    if (outer_low > outer_high)
        outer_high += length;
    if (inner_low > inner_high)
        inner_high += length;
    else if (inner_high < outer_low) {
        inner_low += length;
        inner_high += length;
    }
    return interval_inside_interval(outer_low, outer_high, inner_low, inner_high, double(SCALED_EPSILON));
}
#endif // NDEBUG

#ifdef INFILL_DEBUG_OUTPUT
static void export_infill_to_svg(
    // Boundary contour, along which the perimeter extrusions will be drawn.
    const std::vector<Points>                              &boundary,
    // Parametrization of boundary with Euclidian length.
    const std::vector<std::vector<double>>                 &boundary_parameters,
    // Intersections (T-joints) of the infill lines with the boundary.
    std::vector<std::vector<ContourIntersectionPoint*>>    &boundary_intersections,
    // Infill lines, either completely inside the boundary, or touching the boundary.
    const Polylines                                        &infill,
    const coord_t                                           scaled_spacing,
    const std::string                                      &path,
    const Polylines                                        &overlap_lines = Polylines(),
    const Polylines                                        &polylines = Polylines(),
    const Points                                           &pts = Points())
{
    Polygons    polygons;
    std::transform(boundary.begin(), boundary.end(), std::back_inserter(polygons), [](auto &pts) { return Polygon(pts); });
    ExPolygons  expolygons = union_ex(polygons);
    BoundingBox bbox = get_extents(polygons);
    bbox.offset(scale_(3.));

    ::Slic3r::SVG svg(path, bbox);
    // Draw the filled infill polygons.
    svg.draw(expolygons);

    // Draw the pieces of boundary allowed to be used as anchors of infill lines, not yet consumed.
    const std::string color_boundary_trimmed     = "blue";
    const std::string color_boundary_not_trimmed = "yellow";
    const coordf_t    boundary_line_width        = scaled_spacing;
    svg.draw_outline(polygons, "red", boundary_line_width);
    for (const std::vector<ContourIntersectionPoint*> &intersections : boundary_intersections) {
        const size_t                 boundary_idx  = &intersections - boundary_intersections.data();
        const Points                &contour       = boundary[boundary_idx];
        const std::vector<double>   &contour_param = boundary_parameters[boundary_idx];
        for (const ContourIntersectionPoint *ip : intersections) {
            assert(ip->next_trimmed == ip->next_on_contour->prev_trimmed);
            assert(ip->prev_trimmed == ip->prev_on_contour->next_trimmed);
            {
                Polyline pl { contour[ip->point_idx] };
                if (ip->next_trimmed) {
                    if (ip->contour_not_taken_length_next > SCALED_EPSILON) {
                        take_ccw_limited(pl, contour, contour_param, ip->point_idx, ip->next_on_contour->point_idx, ip->contour_not_taken_length_next);
                        svg.draw(pl, color_boundary_trimmed, boundary_line_width);
                    }
                } else {
                    take_ccw_full(pl, contour, ip->point_idx, ip->next_on_contour->point_idx);
                    svg.draw(pl, color_boundary_not_trimmed, boundary_line_width);
                }
            }
            {
                Polyline pl { contour[ip->point_idx] };
                if (ip->prev_trimmed) {
                    if (ip->contour_not_taken_length_prev > SCALED_EPSILON) {
                        take_cw_limited(pl, contour, contour_param, ip->point_idx, ip->prev_on_contour->point_idx, ip->contour_not_taken_length_prev);
                        svg.draw(pl, color_boundary_trimmed, boundary_line_width);
                    }
                } else {
                    take_cw_full(pl, contour, ip->point_idx, ip->prev_on_contour->point_idx);
                    svg.draw(pl, color_boundary_not_trimmed, boundary_line_width);
                }
            }
        }
    }

    // Draw the full infill polygon boundary.
    svg.draw_outline(polygons, "green");

    // Draw the infill lines, first the full length with red color, then a slightly shortened length with black color.
    svg.draw(infill, "brown");
    static constexpr double trim_length = scale_(0.15);
    for (Polyline polyline : infill)
        if (! polyline.empty()) {
            Vec2d a = polyline.points.front().cast<double>();
            Vec2d d = polyline.points.back().cast<double>();
            if (polyline.size() == 2) {
                Vec2d v = d - a;
                double l = v.norm();
                if (l > 2. * trim_length) {
                    a += v * trim_length / l;
                    d -= v * trim_length / l;
                    polyline.points.front() = a.cast<coord_t>();
                    polyline.points.back() = d.cast<coord_t>();
                } else
                    polyline.points.clear();
            } else if (polyline.size() > 2) {
                Vec2d b = polyline.points[1].cast<double>();
                Vec2d c = polyline.points[polyline.points.size() - 2].cast<double>();
                Vec2d v = b - a;
                double l = v.norm();
                if (l > trim_length) {
                    a += v * trim_length / l;
                    polyline.points.front() = a.cast<coord_t>();
                } else
                    polyline.points.erase(polyline.points.begin());
                v = d - c;
                l = v.norm();
                if (l > trim_length)
                    polyline.points.back() = (d - v * trim_length / l).cast<coord_t>();
                else
                    polyline.points.pop_back();
            }
            svg.draw(polyline, "black");
        }

    svg.draw(overlap_lines, "red", scale_(0.05));
    svg.draw(polylines, "magenta", scale_(0.05));
    svg.draw(pts, "magenta");
}
#endif // INFILL_DEBUG_OUTPUT

#ifndef NDEBUG
bool validate_boundary_intersections(const std::vector<std::vector<ContourIntersectionPoint*>> &boundary_intersections)
{
    for (const std::vector<ContourIntersectionPoint*>& contour : boundary_intersections) {
        for (ContourIntersectionPoint* ip : contour) {
            assert(ip->next_trimmed == ip->next_on_contour->prev_trimmed);
            assert(ip->prev_trimmed == ip->prev_on_contour->next_trimmed);
        }
    }
    return true;
}
#endif // NDEBUG

// Mark the segments of split boundary as consumed if they are very close to some of the infill line.
void mark_boundary_segments_touching_infill(
    // Boundary contour, along which the perimeter extrusions will be drawn.
	const std::vector<Points>                              &boundary,
    // Parametrization of boundary with Euclidian length.
	const std::vector<std::vector<double>>                 &boundary_parameters,
    // Intersections (T-joints) of the infill lines with the boundary.
    std::vector<std::vector<ContourIntersectionPoint*>>    &boundary_intersections,
    // Bounding box around the boundary.
	const BoundingBox 		                               &boundary_bbox,
    // Infill lines, either completely inside the boundary, or touching the boundary.
	const Polylines 		                               &infill,
    // How much of the infill ends should be ignored when marking the boundary segments?
	const double			                                clip_distance,
    // Roughly width of the infill line.
	const double 				                            distance_colliding)
{
    assert(boundary.size() == boundary_parameters.size());
#ifndef NDEBUG
    for (size_t i = 0; i < boundary.size(); ++ i)
        assert(boundary[i].size() + 1 == boundary_parameters[i].size());
    assert(validate_boundary_intersections(boundary_intersections));
#endif

#ifdef INFILL_DEBUG_OUTPUT
    static int iRun = 0;
    ++ iRun;
    int iStep = 0;
    export_infill_to_svg(boundary, boundary_parameters, boundary_intersections, infill, distance_colliding * 2, debug_out_path("%s-%03d.svg", "FillBase-mark_boundary_segments_touching_infill-start", iRun));
    Polylines perimeter_overlaps;
#endif // INFILL_DEBUG_OUTPUT

	EdgeGrid::Grid grid;
    // Make sure that the the grid is big enough for queries against the thick segment.
	grid.set_bbox(boundary_bbox.inflated(distance_colliding * 1.43));
	// Inflate the bounding box by a thick line width.
	grid.create(boundary, coord_t(std::max(clip_distance, distance_colliding) + scale_(10.)));

    // Visitor for the EdgeGrid to trim boundary_intersections with existing infill lines.
	struct Visitor {
		Visitor(const EdgeGrid::Grid &grid,
                const std::vector<Points> &boundary, const std::vector<std::vector<double>> &boundary_parameters, std::vector<std::vector<ContourIntersectionPoint*>> &boundary_intersections,
                const double radius) :
			grid(grid), boundary(boundary), boundary_parameters(boundary_parameters), boundary_intersections(boundary_intersections), radius(radius), trim_l_threshold(0.5 * radius) {}

        // Init with a segment of an infill line.
		void init(const Vec2d &infill_pt1, const Vec2d &infill_pt2) {
			this->infill_pt1 = &infill_pt1;
			this->infill_pt2 = &infill_pt2;
            this->infill_bbox.reset();
            this->infill_bbox.merge(infill_pt1);
            this->infill_bbox.merge(infill_pt2);
            this->infill_bbox.offset(this->radius + SCALED_EPSILON);
        }

		bool operator()(coord_t iy, coord_t ix) {
			// Called with a row and colum of the grid cell, which is intersected by a line.
			auto cell_data_range = this->grid.cell_data_range(iy, ix);
			for (auto it_contour_and_segment = cell_data_range.first; it_contour_and_segment != cell_data_range.second; ++ it_contour_and_segment) {
				// End points of the line segment and their vector.
				auto segment = this->grid.segment(*it_contour_and_segment);
                std::vector<ContourIntersectionPoint*> &intersections = boundary_intersections[it_contour_and_segment->first];
                if (intersections.empty())
                    // There is no infil line touching this contour, thus effort will be saved to calculate overlap with other infill lines.
                    continue;
				const Vec2d seg_pt1 = segment.first.cast<double>();
				const Vec2d seg_pt2 = segment.second.cast<double>();
                std::pair<double, double> interval;
                BoundingBoxf bbox_seg;
                bbox_seg.merge(seg_pt1);
                bbox_seg.merge(seg_pt2);
#ifdef INFILL_DEBUG_OUTPUT
                //if (this->infill_bbox.overlap(bbox_seg)) this->perimeter_overlaps.push_back({ segment.first, segment.second });
#endif // INFILL_DEBUG_OUTPUT
                if (this->infill_bbox.overlap(bbox_seg) && line_rounded_thick_segment_collision(seg_pt1, seg_pt2, *this->infill_pt1, *this->infill_pt2, this->radius, interval)) {
                    // The boundary segment intersects with the infill segment thickened by radius.
                    // Interval is specified in Euclidian length from seg_pt1 to seg_pt2.
                    // 1) Find the Euclidian parameters of seg_pt1 and seg_pt2 on its boundary contour.
                    const std::vector<double> &contour_parameters = boundary_parameters[it_contour_and_segment->first];
                    const double contour_length = contour_parameters.back();
					const double param_seg_pt1  = contour_parameters[it_contour_and_segment->second];
                    const double param_seg_pt2  = contour_parameters[it_contour_and_segment->second + 1];
#ifdef INFILL_DEBUG_OUTPUT
                    this->perimeter_overlaps.push_back({ Point((seg_pt1 + (seg_pt2 - seg_pt1).normalized() * interval.first).cast<coord_t>()),
                                                         Point((seg_pt1 + (seg_pt2 - seg_pt1).normalized() * interval.second).cast<coord_t>()) });
#endif // INFILL_DEBUG_OUTPUT
                    assert(interval.first >= 0.);
                    assert(interval.second >= 0.);
                    assert(interval.first <= interval.second);
                    const auto param_overlap1 = std::min(param_seg_pt2, param_seg_pt1 + interval.first);
                    const auto param_overlap2 = std::min(param_seg_pt2, param_seg_pt1 + interval.second);
                    // 2) Find the ContourIntersectionPoints before param_overlap1 and after param_overlap2.
                    // Find the span of ContourIntersectionPoints, that is trimmed by the interval (param_overlap1, param_overlap2).
                    ContourIntersectionPoint *ip_low, *ip_high;
                    if (intersections.size() == 1) {
                        // Only a single infill line touches this contour.
                        ip_low = ip_high = intersections.front();
                    } else {
                        assert(intersections.size() > 1);
                        auto it_low  = Slic3r::lower_bound_by_predicate(intersections.begin(), intersections.end(), [param_overlap1](const ContourIntersectionPoint *l) { return l->param < param_overlap1; });
                        auto it_high = Slic3r::lower_bound_by_predicate(intersections.begin(), intersections.end(), [param_overlap2](const ContourIntersectionPoint *l) { return l->param < param_overlap2; });
                        ip_low  = it_low  == intersections.end() ? intersections.front() : *it_low;
                        ip_high = it_high == intersections.end() ? intersections.front() : *it_high;
                        if (ip_low->param != param_overlap1)
                            ip_low = ip_low->prev_on_contour;
                        assert(ip_low != ip_high);
                        // Verify that the interval (param_overlap1, param_overlap2) is inside the interval (ip_low->param, ip_high->param).
                        assert(cyclic_interval_inside_interval(ip_low->param, ip_high->param, param_overlap1, param_overlap2, contour_length));
                    }
                    assert(validate_boundary_intersections(boundary_intersections));
                    // Mark all ContourIntersectionPoints between ip_low and ip_high as consumed.
                    if (ip_low->next_on_contour != ip_high)
                        for (ContourIntersectionPoint *ip = ip_low->next_on_contour; ip != ip_high; ip = ip->next_on_contour) {
                            ip->consume_prev();
                            ip->consume_next();
                        }
                    // Subtract the interval from the first and last segments.
                    double trim_l = closed_contour_distance_ccw(ip_low->param, param_overlap1, contour_length);
                    //if (trim_l > trim_l_threshold)
                        ip_low->trim_next(trim_l);
                    trim_l = closed_contour_distance_ccw(param_overlap2, ip_high->param, contour_length);
                    //if (trim_l > trim_l_threshold)
                        ip_high->trim_prev(trim_l);
                    assert(ip_low->next_trimmed == ip_high->prev_trimmed);
                    assert(validate_boundary_intersections(boundary_intersections));
                    //FIXME mark point as consumed?
                    //FIXME verify the sequence between prev and next?
#ifdef INFILL_DEBUG_OUTPUT
					{
#if 0
                        static size_t iRun = 0;
						ExPolygon expoly(Polygon(*grid.contours().front()));
						for (size_t i = 1; i < grid.contours().size(); ++i)
							expoly.holes.emplace_back(Polygon(*grid.contours()[i]));
						SVG svg(debug_out_path("%s-%d.svg", "FillBase-mark_boundary_segments_touching_infill", iRun ++).c_str(), get_extents(expoly));
						svg.draw(expoly, "green");
						svg.draw(Line(segment.first, segment.second), "red");
						svg.draw(Line(this->infill_pt1->cast<coord_t>(), this->infill_pt2->cast<coord_t>()), "magenta");
#endif
                    }
#endif // INFILL_DEBUG_OUTPUT
				}
			}
			// Continue traversing the grid along the edge.
			return true;
		}

		const EdgeGrid::Grid 			   			        &grid;
		const std::vector<Points> 					        &boundary;
        const std::vector<std::vector<double>>              &boundary_parameters;
        std::vector<std::vector<ContourIntersectionPoint*>> &boundary_intersections;
		// Maximum distance between the boundary and the infill line allowed to consider the boundary not touching the infill line.
		const double								         radius;
        // Region around the contour / infill line intersection point, where the intersections are ignored.
        const double                                         trim_l_threshold;

		const Vec2d 								        *infill_pt1;
		const Vec2d 								        *infill_pt2;
        BoundingBoxf                                         infill_bbox;

#ifdef INFILL_DEBUG_OUTPUT
        Polylines                                            perimeter_overlaps;
#endif // INFILL_DEBUG_OUTPUT
	} visitor(grid, boundary, boundary_parameters, boundary_intersections, distance_colliding);

	for (const Polyline &polyline : infill) {
#ifdef INFILL_DEBUG_OUTPUT
        ++ iStep;
#endif // INFILL_DEBUG_OUTPUT
		// Clip the infill polyline by the Eucledian distance along the polyline.
		SegmentPoint start_point = clip_start_segment_and_point(polyline.points, clip_distance);
		SegmentPoint end_point   = clip_end_segment_and_point(polyline.points, clip_distance);
		if (start_point.valid() && end_point.valid() &&
			(start_point.idx_segment < end_point.idx_segment || (start_point.idx_segment == end_point.idx_segment && start_point.t < end_point.t))) {
			// The clipped polyline is non-empty.
#ifdef INFILL_DEBUG_OUTPUT
            visitor.perimeter_overlaps.clear();
#endif // INFILL_DEBUG_OUTPUT
			for (size_t point_idx = start_point.idx_segment; point_idx <= end_point.idx_segment; ++ point_idx) {
//FIXME extend the EdgeGrid to suport tracing a thick line.
#if 0
				Point pt1, pt2;
				Vec2d pt1d, pt2d;
				if (point_idx == start_point.idx_segment) {
					pt1d = start_point.point;
					pt1  = pt1d.cast<coord_t>();
				} else {
					pt1  = polyline.points[point_idx];
					pt1d = pt1.cast<double>();
				}
				if (point_idx == start_point.idx_segment) {
					pt2d = end_point.point;
					pt2  = pt1d.cast<coord_t>();
				} else {
					pt2  = polyline.points[point_idx];
					pt2d = pt2.cast<double>();
				}
				visitor.init(pt1d, pt2d);
				grid.visit_cells_intersecting_thick_line(pt1, pt2, distance_colliding, visitor);
#else
				Vec2d pt1 = (point_idx == start_point.idx_segment) ? start_point.point : polyline.points[point_idx    ].cast<double>();
				Vec2d pt2 = (point_idx == end_point  .idx_segment) ? end_point  .point : polyline.points[point_idx + 1].cast<double>();
#if 0
					{
						static size_t iRun = 0;
						ExPolygon expoly(Polygon(*grid.contours().front()));
						for (size_t i = 1; i < grid.contours().size(); ++i)
							expoly.holes.emplace_back(Polygon(*grid.contours()[i]));
						SVG svg(debug_out_path("%s-%d.svg", "FillBase-mark_boundary_segments_touching_infill0", iRun ++).c_str(), get_extents(expoly));
						svg.draw(expoly, "green");
						svg.draw(polyline, "blue");
						svg.draw(Line(pt1.cast<coord_t>(), pt2.cast<coord_t>()), "magenta", scale_(0.1));
					}
#endif
				visitor.init(pt1, pt2);
				// Simulate tracing of a thick line. This only works reliably if distance_colliding <= grid cell size.
				Vec2d v = (pt2 - pt1).normalized() * distance_colliding;
				Vec2d vperp = perp(v);
				Vec2d a = pt1 - v - vperp;
				Vec2d b = pt2 + v - vperp;
                assert(grid.bbox().contains(a.cast<coord_t>()));
                assert(grid.bbox().contains(b.cast<coord_t>()));
				grid.visit_cells_intersecting_line(a.cast<coord_t>(), b.cast<coord_t>(), visitor);
				a = pt1 - v + vperp;
				b = pt2 + v + vperp;
                assert(grid.bbox().contains(a.cast<coord_t>()));
                assert(grid.bbox().contains(b.cast<coord_t>()));
                grid.visit_cells_intersecting_line(a.cast<coord_t>(), b.cast<coord_t>(), visitor);
#endif
#ifdef INFILL_DEBUG_OUTPUT
//                export_infill_to_svg(boundary, boundary_parameters, boundary_intersections, infill, distance_colliding * 2, debug_out_path("%s-%03d-%03d-%03d.svg", "FillBase-mark_boundary_segments_touching_infill-step", iRun, iStep, int(point_idx)), { polyline });
#endif // INFILL_DEBUG_OUTPUT
			}
#ifdef INFILL_DEBUG_OUTPUT
            Polylines perimeter_overlaps;
            export_infill_to_svg(boundary, boundary_parameters, boundary_intersections, infill, distance_colliding * 2, debug_out_path("%s-%03d-%03d.svg", "FillBase-mark_boundary_segments_touching_infill-step", iRun, iStep), visitor.perimeter_overlaps, { polyline });
            append(perimeter_overlaps, std::move(visitor.perimeter_overlaps));
            perimeter_overlaps.clear();
#endif // INFILL_DEBUG_OUTPUT
        }
	}

#ifdef INFILL_DEBUG_OUTPUT
    export_infill_to_svg(boundary, boundary_parameters, boundary_intersections, infill, distance_colliding * 2, debug_out_path("%s-%03d.svg", "FillBase-mark_boundary_segments_touching_infill-end", iRun), perimeter_overlaps);
#endif // INFILL_DEBUG_OUTPUT
    assert(validate_boundary_intersections(boundary_intersections));
}

void Fill::connect_infill(Polylines &&infill_ordered, const ExPolygon &boundary_src, Polylines &polylines_out, const double spacing, const FillParams &params)
{
	assert(! boundary_src.contour.points.empty());
    auto polygons_src = reserve_vector<const Polygon*>(boundary_src.holes.size() + 1);
    polygons_src.emplace_back(&boundary_src.contour);
    for (const Polygon &polygon : boundary_src.holes)
        polygons_src.emplace_back(&polygon);

    connect_infill(std::move(infill_ordered), polygons_src, get_extents(boundary_src.contour), polylines_out, spacing, params);
}

void Fill::connect_infill(Polylines &&infill_ordered, const Polygons &boundary_src, const BoundingBox &bbox, Polylines &polylines_out, const double spacing, const FillParams &params)
{
    auto polygons_src = reserve_vector<const Polygon*>(boundary_src.size());
    for (const Polygon &polygon : boundary_src)
        polygons_src.emplace_back(&polygon);

    connect_infill(std::move(infill_ordered), polygons_src, bbox, polylines_out, spacing, params);
}

static constexpr auto boundary_idx_unconnected = std::numeric_limits<size_t>::max();

struct BoundaryInfillGraph
{
    std::vector<Points>                     boundary;
    std::vector<std::vector<double>>        boundary_params;
    std::vector<ContourIntersectionPoint>   map_infill_end_point_to_boundary;

    const Point&    point(const ContourIntersectionPoint &cp) const {
        assert(cp.contour_idx != size_t(-1));
        assert(cp.point_idx != size_t(-1));
        return this->boundary[cp.contour_idx][cp.point_idx];
    }

    const Point&    infill_end_point(size_t infill_end_point_idx) const {
        return this->point(this->map_infill_end_point_to_boundary[infill_end_point_idx]);
    }

    const Point     interpolate_contour_point(const ContourIntersectionPoint &cp, double param) {
        const Points                &contour        = this->boundary[cp.contour_idx];
        const std::vector<double>   &contour_params = this->boundary_params[cp.contour_idx];
        // Find the start of a contour segment with param.
        auto it = std::lower_bound(contour_params.begin(), contour_params.end(), param);
        if (*it != param) {
            assert(it != contour_params.begin());
            -- it;
        }
        size_t i = it - contour_params.begin();
        if (i == contour.size())
            i = 0;
        double t1 = contour_params[i];
        double t2 = next_value_modulo(i, contour_params);
        return lerp(contour[i], next_value_modulo(i, contour), (param - t1) / (t2 - t1));
    }

    enum Direction {
        Left,
        Right,
        Up,
        Down,
        Taken,
    };

    static Direction dir(const Point &p1, const Point &p2) {
        return p1.x() == p2.x() ?
            (p1.y() < p2.y() ? Up : Down) :
            (p1.x() < p2.x() ? Right : Left);
    }

    const Direction dir_prev(const ContourIntersectionPoint &cp) const {
        assert(cp.prev_on_contour);
        return cp.could_take_prev() ?
            dir(this->point(cp), this->point(*cp.prev_on_contour)) :
            Taken;
    }

    const Direction dir_next(const ContourIntersectionPoint &cp) const {
        assert(cp.next_on_contour);
        return cp.could_take_next() ?
            dir(this->point(cp), this->point(*cp.next_on_contour)) :
            Taken;
    }

    bool            first(const ContourIntersectionPoint &cp) const {
        return ((&cp - this->map_infill_end_point_to_boundary.data()) & 1) == 0;
    }

    const ContourIntersectionPoint& other(const ContourIntersectionPoint &cp) const {
        return this->map_infill_end_point_to_boundary[((&cp - this->map_infill_end_point_to_boundary.data()) ^ 1)];
    }

    ContourIntersectionPoint& other(const ContourIntersectionPoint &cp) {
        return this->map_infill_end_point_to_boundary[((&cp - this->map_infill_end_point_to_boundary.data()) ^ 1)];
    }

    bool            prev_vertical(const ContourIntersectionPoint &cp) const {
        return this->point(cp).x() == this->point(*cp.prev_on_contour).x();
    }

    bool            next_vertical(const ContourIntersectionPoint &cp) const {
        return this->point(cp).x() == this->point(*cp.next_on_contour).x();
    }

};


// After mark_boundary_segments_touching_infill() marks boundary segments overlapping trimmed infill lines,
// there are possibly some very short boundary segments unmarked, but overlapping the untrimmed infill lines fully
// Mark those short boundary segments.
static inline void mark_boundary_segments_overlapping_infill(
    BoundaryInfillGraph                                    &graph,
    // Infill lines, either completely inside the boundary, or touching the boundary.
    const Polylines                                        &infill,
    // Spacing (width) of the infill lines.
    const double                                            spacing)
{
    for (ContourIntersectionPoint &cp : graph.map_infill_end_point_to_boundary) {
        const Points                &contour         = graph.boundary[cp.contour_idx];
        const std::vector<double>   &contour_params  = graph.boundary_params[cp.contour_idx];
        const Polyline              &infill_polyline = infill[(&cp - graph.map_infill_end_point_to_boundary.data()) / 2];
        const double                 radius          = 0.5 * (spacing + SCALED_EPSILON);
        assert(infill_polyline.size() == 2);
        const Linef                  infill_line { infill_polyline.points.front().cast<double>(), infill_polyline.points.back().cast<double>() };
        if (cp.could_take_next()) {
            bool inside = true;
            for (size_t i = cp.point_idx; i != cp.next_on_contour->point_idx; ) {
                size_t j = next_idx_modulo(i, contour);
                const Vec2d seg_pt2 = contour[j].cast<double>();
                if (line_alg::distance_to_squared(infill_line, seg_pt2) < radius * radius) {
                    // The segment is completely inside.
                } else {
                    std::pair<double, double> interval;
                    line_rounded_thick_segment_collision(contour[i].cast<double>(), seg_pt2, infill_line.a, infill_line.b, radius, interval);
                    assert(interval.first == 0.);
                    double len_out = closed_contour_distance_ccw(contour_params[cp.point_idx], contour_params[i], contour_params.back()) + interval.second;
                    if (len_out < cp.contour_not_taken_length_next) {
                        // Leaving the infill line region before exiting cp.contour_not_taken_length_next,
                        // thus at least some of the contour is outside and we will extrude this segment.
                        inside = false;
                        break;
                    }
                }
                if (closed_contour_distance_ccw(contour_params[cp.point_idx], contour_params[j], contour_params.back()) >= cp.contour_not_taken_length_next)
                    break;
                i = j;
            }
            if (inside) {
                if (! cp.next_trimmed)
                    // The arc from cp to cp.next_on_contour was not trimmed yet, however it is completely overlapping the infill line.
                    cp.next_on_contour->trim_prev(0);
                cp.trim_next(0);
            }
        } else
            cp.trim_next(0);
        if (cp.could_take_prev()) {
            bool inside = true;
            for (size_t i = cp.point_idx; i != cp.prev_on_contour->point_idx; ) {
                size_t j = prev_idx_modulo(i, contour);
                const Vec2d seg_pt2 = contour[j].cast<double>();
                // Distance of the second segment line from the infill line.
                if (line_alg::distance_to_squared(infill_line, seg_pt2) < radius * radius) {
                    // The segment is completely inside.
                } else {
                    std::pair<double, double> interval;
                    line_rounded_thick_segment_collision(contour[i].cast<double>(), seg_pt2, infill_line.a, infill_line.b, radius, interval);
                    assert(interval.first == 0.);
                    double len_out = closed_contour_distance_cw(contour_params[cp.point_idx], contour_params[i], contour_params.back()) + interval.second;
                    if (len_out < cp.contour_not_taken_length_prev) {
                        // Leaving the infill line region before exiting cp.contour_not_taken_length_next,
                        // thus at least some of the contour is outside and we will extrude this segment.
                        inside = false;
                        break;
                    }
                }
                if (closed_contour_distance_cw(contour_params[cp.point_idx], contour_params[j], contour_params.back()) >= cp.contour_not_taken_length_prev)
                    break;
                i = j;
            }
            if (inside) {
                if (! cp.prev_trimmed)
                    // The arc from cp to cp.prev_on_contour was not trimmed yet, however it is completely overlapping the infill line.
                    cp.prev_on_contour->trim_next(0);
                cp.trim_prev(0);
            }
        } else
            cp.trim_prev(0);
    }
}

BoundaryInfillGraph create_boundary_infill_graph(const Polylines &infill_ordered, const std::vector<const Polygon*> &boundary_src, const BoundingBox &bbox, const double spacing, const bool skip_trimming = false)
{
    BoundaryInfillGraph out;
    out.boundary.assign(boundary_src.size(), Points());
    out.boundary_params.assign(boundary_src.size(), std::vector<double>());
    out.map_infill_end_point_to_boundary.assign(infill_ordered.size() * 2, ContourIntersectionPoint{ boundary_idx_unconnected, boundary_idx_unconnected });
    {
        // Project the infill_ordered end points onto boundary_src.
        std::vector<std::pair<EdgeGrid::Grid::ClosestPointResult, size_t>> intersection_points;
        {
            EdgeGrid::Grid grid;
            grid.set_bbox(bbox.inflated(SCALED_EPSILON));
            grid.create(boundary_src, coord_t(scale_(10.)));
            intersection_points.reserve(infill_ordered.size() * 2);
            for (const Polyline &pl : infill_ordered)
                for (const Point *pt : { &pl.points.front(), &pl.points.back() }) {
                    EdgeGrid::Grid::ClosestPointResult cp = grid.closest_point_signed_distance(*pt, coord_t(SCALED_EPSILON));
                    if (cp.valid()) {
                        // The infill end point shall lie on the contour.
                        assert(cp.distance <= 3.);
                        intersection_points.emplace_back(cp, (&pl - infill_ordered.data()) * 2 + (pt == &pl.points.front() ? 0 : 1));
                    }
                }
            std::sort(intersection_points.begin(), intersection_points.end(), [](const std::pair<EdgeGrid::Grid::ClosestPointResult, size_t> &cp1, const std::pair<EdgeGrid::Grid::ClosestPointResult, size_t> &cp2) {
                return   cp1.first.contour_idx < cp2.first.contour_idx ||
                        (cp1.first.contour_idx == cp2.first.contour_idx &&
                            (cp1.first.start_point_idx < cp2.first.start_point_idx ||
                                (cp1.first.start_point_idx == cp2.first.start_point_idx && cp1.first.t < cp2.first.t)));
            });
        }
        auto it = intersection_points.begin();
        auto it_end = intersection_points.end();
        std::vector<std::vector<ContourIntersectionPoint*>> boundary_intersection_points(out.boundary.size(), std::vector<ContourIntersectionPoint*>());
        for (size_t idx_contour = 0; idx_contour < boundary_src.size(); ++ idx_contour) {
            // Copy contour_src to contour_dst while adding intersection points.
            // Map infill end points map_infill_end_point_to_boundary to the newly inserted boundary points of contour_dst.
            // chain the points of map_infill_end_point_to_boundary along their respective contours.
            const Polygon &contour_src = *boundary_src[idx_contour];
            Points        &contour_dst = out.boundary[idx_contour];
            std::vector<ContourIntersectionPoint*> &contour_intersection_points = boundary_intersection_points[idx_contour];
            ContourIntersectionPoint *pfirst = nullptr;
            ContourIntersectionPoint *pprev  = nullptr;
            {
                // Reserve intersection points.
                size_t n_intersection_points = 0;
                for (auto itx = it; itx != it_end && itx->first.contour_idx == idx_contour; ++ itx)
                    ++ n_intersection_points;
                contour_intersection_points.reserve(n_intersection_points);
            }
            for (size_t idx_point = 0; idx_point < contour_src.points.size(); ++ idx_point) {
                const Point &ipt = contour_src.points[idx_point];
                if (contour_dst.empty() || contour_dst.back() != ipt)
                    contour_dst.emplace_back(ipt);
                for (; it != it_end && it->first.contour_idx == idx_contour && it->first.start_point_idx == idx_point; ++ it) {
                    // Add these points to the destination contour.
                    const Polyline  &infill_line = infill_ordered[it->second / 2];
                    const Point     &pt          = (it->second & 1) ? infill_line.points.back() : infill_line.points.front();
//#ifndef NDEBUG
//                    {
//                      const Vec2d pt1 = ipt.cast<double>();
//                      const Vec2d pt2 = (idx_point + 1 == contour_src.size() ? contour_src.points.front() : contour_src.points[idx_point + 1]).cast<double>();
//                      const Vec2d ptx = lerp(pt1, pt2, it->first.t);
//                      assert(std::abs(ptx.x() - pt.x()) < SCALED_EPSILON);
//                      assert(std::abs(ptx.y() - pt.y()) < SCALED_EPSILON);
//                    }
//#endif // NDEBUG
                    size_t idx_tjoint_pt = 0;
                    if (idx_point + 1 < contour_src.size() || pt != contour_dst.front()) {
                        if (pt != contour_dst.back())
                            contour_dst.emplace_back(pt);
                        idx_tjoint_pt = contour_dst.size() - 1;
                    }
                    out.map_infill_end_point_to_boundary[it->second] = ContourIntersectionPoint{ /* it->second, */ idx_contour, idx_tjoint_pt };
                    ContourIntersectionPoint *pthis = &out.map_infill_end_point_to_boundary[it->second];
                    if (pprev) {
                        pprev->next_on_contour = pthis;
                        pthis->prev_on_contour = pprev;
                    } else
                        pfirst = pthis;
                    contour_intersection_points.emplace_back(pthis);
                    pprev = pthis;
                }
                if (pfirst) {
                    pprev->next_on_contour = pfirst;
                    pfirst->prev_on_contour = pprev;
                }
            }
            // Parametrize the new boundary with the intersection points inserted.
            std::vector<double> &contour_params = out.boundary_params[idx_contour];
            contour_params.assign(contour_dst.size() + 1, 0.);
            for (size_t i = 1; i < contour_dst.size(); ++i) {
                contour_params[i] = contour_params[i - 1] + (contour_dst[i].cast<double>() - contour_dst[i - 1].cast<double>()).norm();
                assert(contour_params[i] > contour_params[i - 1]);
            }
            contour_params.back() = contour_params[contour_params.size() - 2] + (contour_dst.back().cast<double>() - contour_dst.front().cast<double>()).norm();
            assert(contour_params.back() > contour_params[contour_params.size() - 2]);
            // Map parameters from contour_params to boundary_intersection_points.
            for (ContourIntersectionPoint *ip : contour_intersection_points)
                ip->param = contour_params[ip->point_idx];
            // and measure distance to the previous and next intersection point.
            const double contour_length = contour_params.back();
            for (ContourIntersectionPoint *ip : contour_intersection_points)
                if (ip->next_on_contour == ip) {
                    assert(ip->prev_on_contour == ip);
                    ip->contour_not_taken_length_prev = ip->contour_not_taken_length_next = contour_length;
                } else {
                    assert(ip->prev_on_contour != ip);
                    ip->contour_not_taken_length_prev = closed_contour_distance_ccw(ip->prev_on_contour->param, ip->param, contour_length);
                    ip->contour_not_taken_length_next = closed_contour_distance_ccw(ip->param, ip->next_on_contour->param, contour_length);
                }
        }

        assert(out.boundary.size() == boundary_src.size());
#if 0
        // Adaptive Cubic Infill produces infill lines, which not always end at the outer boundary.
        assert(std::all_of(out.map_infill_end_point_to_boundary.begin(), out.map_infill_end_point_to_boundary.end(),
            [&out.boundary](const ContourIntersectionPoint &contour_point) {
                return contour_point.contour_idx < out.boundary.size() && contour_point.point_idx < out.boundary[contour_point.contour_idx].size();
            }));
#endif

        // Mark the points and segments of split out.boundary as consumed if they are very close to some of the infill line.
        // When connecting infill into a single path (Cura-style), we must NOT trim boundary segments that are close to
        // infill lines: tracing the whole inner wall is exactly what we want, so the trimming would create gaps in the
        // single continuous path (especially with Fill Multiline > 1, where infill lines run very close to the wall).
        if (! skip_trimming) {
            // @supermerill used 2. * scale_(spacing)
            const double clip_distance      = 1.7 * scale_(spacing);
            // Allow a bit of overlap. This value must be slightly higher than the overlap of FillAdaptive, otherwise
            // the anchors of the adaptive infill will mask the other side of the perimeter line.
            // (see connect_lines_using_hooks() in FillAdaptive.cpp)
            const double distance_colliding = 0.8 * scale_(spacing);
            mark_boundary_segments_touching_infill(out.boundary, out.boundary_params, boundary_intersection_points, bbox, infill_ordered, clip_distance, distance_colliding);
        }
    }

    return out;
}

// The extended bounding box of the whole object that covers any rotation of every layer.
BoundingBox Fill::extended_object_bounding_box() const
{
    BoundingBox out = bounding_box;
    out.merge(Point(out.min.y(), out.min.x()));
    out.merge(Point(out.max.y(), out.max.x()));

    // The bounding box is scaled by sqrt(2.) to ensure that the bounding box
    // covers any possible rotations.
    return out.scaled(sqrt(2.));
}

// ---------------------------------------------------------------------------------------------
// GINGER_SP_PROFILE=1: per-phase wall-clock profiler of the single-path connector. Pure
// observation - decisions are never touched; when the env var is unset the cost is one cached
// branch per scope. Totals are atomics (TBB threads aggregate lock-free) and are dumped to
// stderr at process exit.
namespace {

struct SPProfile
{
    enum Phase : unsigned {
        phGapBlocked, phExactSolve, phInitPhase, phPhaseSwap, phSectorFlips,
        phFreeRunClosure, phDefectSlide, phLastResort, phAugmentation, phHierholzerEmit,
        phJoinWeld, phSpliceRingScan, phSpliceAttachScan, phRebuildRetrace, phLinkValid,
        N_PHASES
    };
    static const char *name(unsigned i) {
        static const char *names[N_PHASES] = {
            "gap_blocked", "exact_solve", "init_phase", "phase_swap", "sector_flips",
            "freerun_closure", "defect_slide", "last_resort", "augmentation", "hierholzer_emit",
            "join_weld", "splice_ring_scan", "splice_attach_scan", "rebuild_retrace", "link_valid"
        };
        return names[i];
    }
    std::atomic<uint64_t> ns[N_PHASES] {};
    std::atomic<uint64_t> cnt[N_PHASES] {};
    // Work counters paired with the timers (what scales, not just how long).
    std::atomic<uint64_t> flip_iters {}, flip_cands {}, trails_calls {}, slide_steps {},
                          augment_pairs {}, splice_merges {}, clipper_calls {}, islands {};

    static bool enabled() {
        static const bool on = std::getenv("GINGER_SP_PROFILE") != nullptr;
        return on;
    }
    static SPProfile& get() {
        static SPProfile p;
        static const bool registered = []() { std::atexit(&SPProfile::dump); return true; }();
        (void)registered;
        return p;
    }
    static void dump() {
        SPProfile &p = get();
        uint64_t total = 0;
        for (unsigned i = 0; i < N_PHASES; ++ i)
            total += p.ns[i].load(std::memory_order_relaxed);
        fprintf(stderr, "[SPPROF] ============ single-path connector profile ============\n");
        for (unsigned i = 0; i < N_PHASES; ++ i) {
            const uint64_t t = p.ns[i].load(std::memory_order_relaxed);
            if (t == 0)
                continue;
            fprintf(stderr, "[SPPROF] %-20s %10.1f ms  (%8llu calls, %5.1f%%)\n",
                    name(i), double(t) * 1e-6,
                    (unsigned long long) p.cnt[i].load(std::memory_order_relaxed),
                    total > 0 ? 100. * double(t) / double(total) : 0.);
        }
        fprintf(stderr, "[SPPROF] phases total %.1f ms (threads overlap: wall time is lower)\n", double(total) * 1e-6);
        fprintf(stderr, "[SPPROF] islands=%llu flip_iters=%llu flip_cands=%llu trails_calls=%llu"
                        " slide_steps=%llu augment_pairs=%llu splice_merges=%llu clipper_calls=%llu\n",
                (unsigned long long) p.islands.load(), (unsigned long long) p.flip_iters.load(),
                (unsigned long long) p.flip_cands.load(), (unsigned long long) p.trails_calls.load(),
                (unsigned long long) p.slide_steps.load(), (unsigned long long) p.augment_pairs.load(),
                (unsigned long long) p.splice_merges.load(), (unsigned long long) p.clipper_calls.load());
    }
};

// RAII scope timer: negligible when profiling is off (one cached-static branch).
class SPTimer
{
public:
    SPTimer(SPProfile::Phase phase) : m_phase(phase), m_on(SPProfile::enabled()) {
        if (m_on)
            m_t0 = std::chrono::steady_clock::now();
    }
    ~SPTimer() {
        if (m_on) {
            const uint64_t dt = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - m_t0).count();
            SPProfile &p = SPProfile::get();
            p.ns[m_phase].fetch_add(dt, std::memory_order_relaxed);
            p.cnt[m_phase].fetch_add(1, std::memory_order_relaxed);
        }
    }
private:
    SPProfile::Phase                      m_phase;
    bool                                  m_on;
    std::chrono::steady_clock::time_point m_t0;
};

inline void sp_profile_count(std::atomic<uint64_t> SPProfile::*counter, uint64_t n = 1) {
    if (SPProfile::enabled())
        (SPProfile::get().*counter).fetch_add(n, std::memory_order_relaxed);
}

} // anonymous namespace
// ---------------------------------------------------------------------------------------------

// Splice a set of closed loops (e.g. the outline walls of a dilated infill band: one outer wall plus
// the hole walls of the pockets it encloses) into a single closed loop. Loops are linked pairwise at
// their closest approach with two short link segments crossing the empty space in between; the cuts in
// the two loops are staggered by ~one extrusion width, so the links land in the cut gaps instead of
// over already extruded material. Rings that remain (no other ring within reach) are attached to the
// closest open polyline as a detour; open polylines are otherwise left untouched.
void single_path_splice_loops(Polylines &loops, double max_link_distance, double stagger, const Polygons *island)
{
    // Separate the closed loops (normalized to an open ring representation) from open polylines.
    Polylines rings, open;
    for (Polyline &pl : loops) {
        if (pl.size() > 3 && pl.points.front() == pl.points.back()) {
            pl.points.pop_back();
            rings.emplace_back(std::move(pl));
        } else if (pl.size() > 1)
            open.emplace_back(std::move(pl));
    }
    loops.clear();

    // The closest point on the segment (a, b) to p. Clipper outlines have vertices only at corners and
    // caps: the closest approach between two rings (or a ring and a path) usually falls in the middle
    // of a long straight segment, so all searches below project onto segments instead of comparing
    // vertices - a vertex-based link can come out many millimeters long and run nearly parallel to an
    // already extruded wall.
    auto closest_on_segment = [](const Point &p, const Point &a, const Point &b) -> Point {
        Vec2d  ab   = (b - a).cast<double>();
        double len2 = ab.squaredNorm();
        if (len2 <= 0.)
            return a;
        double t = std::clamp((p - a).cast<double>().dot(ab) / len2, 0., 1.);
        return Point((a.cast<double>() + ab * t).cast<coord_t>());
    };

    // Physical link rule (Davide): a link may be ANY length, but (a) it must stay inside the
    // island - it is a real extruded bead - and (b) it must not RETRACE an already extruded
    // line: nearly parallel (<25deg) closer than 0.8 stagger over more than 1.5 stagger
    // accumulated, the same coincidence the gap_blocked rule measures (stagger is the line
    // width at every caller that passes an island). Links up to 1.5 stagger skip the test:
    // too short to accumulate more overlap than a legal gap arc is allowed anyway.
    // With island == nullptr (the trapezoidal multiline caller) every link is accepted.
    const double guard_free = 1.5 * stagger;
    std::vector<Line> retrace_lines;
    std::optional<AABBTreeLines::LinesDistancer<Line>> retrace;
    auto rebuild_retrace = [&]() {
        if (island == nullptr)
            return;
        SPTimer sp_timer_(SPProfile::phRebuildRetrace);
        retrace_lines.clear();
        for (const Polylines *set : { &rings, &open })
            for (const Polyline &pl : *set)
                for (size_t i = 0; i + 1 < pl.size(); ++ i)
                    retrace_lines.emplace_back(pl.points[i], pl.points[i + 1]);
        // A normalized ring is stored open: close it so the seam segment is testable too.
        for (const Polyline &pl : rings)
            if (pl.size() > 2)
                retrace_lines.emplace_back(pl.points.back(), pl.points.front());
        retrace.emplace(retrace_lines);
    };
    auto link_valid = [&](const Point &la, const Point &lb) -> bool {
        if (island == nullptr)
            return true;
        SPTimer sp_timer_(SPProfile::phLinkValid);
        const Vec2d  v   = (lb - la).cast<double>();
        const double len = v.norm();
        if (len <= guard_free)
            return true;
        // No-retrace first: the AABB scan is much cheaper than the Clipper clip below, and it is
        // the test that rejects most invalid candidates (links riding an existing bead).
        if (retrace) {
            const Vec2d  dir     = v / len;
            const double step    = 0.5 * stagger;
            const double d_close = 0.8 * stagger;
            const double cos_par = 0.90630779; // cos 25deg, as in gap_blocked
            double coinc = 0.;
            for (double s = 0.5 * step; s < len && coinc <= guard_free; s += step) {
                const Point p((la.cast<double>() + dir * s).cast<coord_t>());
                const auto [d, idx, np] = retrace->distance_from_lines_extra<false>(p);
                if (std::abs(d) < d_close) {
                    const Vec2d  t  = (retrace_lines[size_t(idx)].b - retrace_lines[size_t(idx)].a).cast<double>();
                    const double tn = t.norm();
                    if (tn > 0. && std::abs(dir.dot(t)) > cos_par * tn)
                        coinc += step;
                }
            }
            if (coinc > guard_free)
                return false;
        }
        sp_profile_count(&SPProfile::clipper_calls);
        Lines  kept = intersection_ln(Line(la, lb), *island);
        double tot  = 0.;
        for (const Line &l : kept)
            tot += l.length();
        return tot + SCALED_EPSILON >= 0.999 * len; // else: would extrude across void outside the part
    };
    // A candidate scan validates at most this many (distance-ordered) candidates before giving
    // up for the round: on an unweldable piece every candidate fails, and each validation ends
    // in a Clipper call - unbounded, that is 100k+ clips on a single pathological layer. The
    // budget counts DISTINCT regions: a failed candidate vetoes its neighborhood, because the
    // invalid cluster around a void (hundreds of point pairs through the same notch, measured
    // on the H part) would otherwise exhaust any budget before the first geometrically distinct
    // candidate gets a chance.
    constexpr size_t max_validations = 64;
    const double     dedup_r  = 3. * stagger;
    std::vector<std::pair<Point, Point>> failed_links;
    auto near_failed = [&failed_links, dedup_r](const Point &a, const Point &b) -> bool {
        for (const auto &f : failed_links) {
            if ((a - f.first).cast<double>().norm() < dedup_r && (b - f.second).cast<double>().norm() < dedup_r)
                return true;
            if ((a - f.second).cast<double>().norm() < dedup_r && (b - f.first).cast<double>().norm() < dedup_r)
                return true;
        }
        return false;
    };
    rebuild_retrace();

    // Traverse the whole ring once, entering at `entry` (a point on the segment R[seg] -> R[seg+1]) and
    // exiting at the point one `stagger` away from the entry on the far side of the walk; the arc
    // between exit and entry is the cut gap the link segments land in. `walk_forward` selects the
    // traversal direction (the cut gap is always on the side the walk does NOT start towards).
    auto walk_ring = [](const Points &R, size_t seg, const Point &entry, double stagger, bool walk_forward) -> Points {
        const size_t m = R.size();
        Points out;
        out.reserve(m + 2);
        out.emplace_back(entry);
        // Locate the exit point at arc distance `stagger` from the entry, moving against the walk
        // direction; the vertices inside that arc belong to the cut gap and are skipped.
        double remaining = stagger;
        Point  cur    = entry;
        size_t n_skip = 0;
        size_t ni     = walk_forward ? seg : (seg + 1) % m;
        while (true) {
            double seg_len = (R[ni] - cur).cast<double>().norm();
            if (seg_len >= remaining || n_skip + 2 >= m)
                break;
            remaining -= seg_len;
            cur = R[ni];
            ni  = walk_forward ? (ni + m - 1) % m : (ni + 1) % m;
            ++ n_skip;
        }
        Vec2d  dir = (R[ni] - cur).cast<double>();
        double dl  = dir.norm();
        Point  exit_pt = dl <= 0. ? cur : Point((cur.cast<double>() + dir * std::min(1., remaining / dl)).cast<coord_t>());
        size_t first   = walk_forward ? (seg + 1) % m : seg;
        for (size_t cnt = 0; cnt < m - n_skip; ++ cnt)
            out.emplace_back(R[walk_forward ? (first + cnt) % m : (first + m - cnt) % m]);
        out.emplace_back(exit_pt);
        return out;
    };

    const double max_link2 = max_link_distance * max_link_distance;
    for (bool merged_any = true; merged_any && rings.size() > 1; ) {
        merged_any = false;
        SPTimer sp_timer_(SPProfile::phSpliceRingScan);
        // Globally closest approach between any two rings: vertices of one against segments of the
        // other. The candidate set is conceptually ALL pairs under max_link_distance, but only a
        // d2-sorted prefix is ever consumed (the first VALID link wins and validation is
        // budget-capped), so enumerating every pair - O(V^2), measured 43s of a 52s slice on the
        // stool racetrack rings - buys nothing. Candidates are collected inside a GROWING RADIUS
        // instead: per-ring segment trees answer all-lines-in-radius queries, the radius only
        // grows while nothing within it validates and budget remains, and each pass processes the
        // exact d2 window [r_prev^2, r^2) so no candidate is seen twice. Ties on d2 order by
        // enumeration (i, j, v, s) via stable_sort: canonical, subset-independent.
        struct RingCand { double d2; size_t i, j, v, s; Point proj; };
        std::vector<RingCand> rcands;
        rcands.reserve(256);
        // Per-ring segment sets, index-aligned with the s of the original enumeration
        // (segment s = A[s] .. A[(s+1) % size], ring-closing seam included).
        std::vector<std::vector<Line>>                                  ring_lines(rings.size());
        std::vector<std::optional<AABBTreeLines::LinesDistancer<Line>>> ring_tree(rings.size());
        for (size_t i = 0; i < rings.size(); ++ i) {
            const Points &A = rings[i].points;
            ring_lines[i].reserve(A.size());
            for (size_t s = 0; s < A.size(); ++ s)
                ring_lines[i].emplace_back(A[s], A[(s + 1) % A.size()]);
            ring_tree[i].emplace(ring_lines[i]);
        }
        size_t bi = 0, bj = 0, b_vert = 0, b_seg = 0;
        Point  b_proj;
        bool   b_found = false;
        size_t tried   = 0;
        failed_links.clear();
        std::vector<size_t> near_segs;
        double r_prev2 = 0.; // exclusive-below bound of the d2 window already processed
        for (double r = std::min(std::max(8. * stagger, scale_(1.)), max_link_distance); ; ) {
            const double r2 = std::min(r * r, max_link2);
            rcands.clear();
            for (size_t i = 0; i < rings.size(); ++ i)
                for (size_t j = 0; j < rings.size(); ++ j) {
                    if (i == j)
                        continue;
                    const Points &B = rings[j].points; // vertices
                    for (size_t v = 0; v < B.size(); ++ v) {
                        // Inflated superset query; the exact filter below uses the same
                        // closest_on_segment arithmetic as the original full enumeration.
                        near_segs = ring_tree[i]->all_lines_in_radius(B[v], r * 1.01);
                        std::sort(near_segs.begin(), near_segs.end());
                        for (size_t s : near_segs) {
                            const Line &ln   = ring_lines[i][s];
                            Point       proj = closest_on_segment(B[v], ln.a, ln.b);
                            double      d    = (B[v] - proj).cast<double>().squaredNorm();
                            if (d >= r_prev2 && d < r2)
                                rcands.push_back({ d, i, j, v, s, proj });
                        }
                    }
                }
            std::stable_sort(rcands.begin(), rcands.end(), [](const RingCand &l, const RingCand &r) { return l.d2 < r.d2; });
            bool budget_out = false;
            for (const RingCand &c : rcands) {
                const Point &pb = rings[c.j].points[c.v];
                if (near_failed(c.proj, pb))
                    continue; // same invalid region: skip without spending budget
                if (++ tried > max_validations) {
                    budget_out = true;
                    break;
                }
                if (link_valid(c.proj, pb)) {
                    bi = c.i; bj = c.j; b_vert = c.v; b_seg = c.s; b_proj = c.proj;
                    b_found = true;
                    break;
                }
                failed_links.emplace_back(c.proj, pb);
            }
            if (b_found || budget_out || r >= max_link_distance)
                break;
            r_prev2 = r2;
            r       = std::min(r * 4., max_link_distance);
        }
        if (! b_found)
            break;
        const Points &A = rings[bi].points;
        const Points &B = rings[bj].points;
        // Cut B at its closest vertex, A at the projected point right across; stagger both cuts and
        // pick the walk direction on A that keeps the closing link short. The merged sequence is
        // B-walk, link, A-walk; the final re-close below adds the second link back to the B entry.
        Points pieceB   = walk_ring(B, b_vert, B[b_vert], stagger, false);
        Points pieceA_f = walk_ring(A, b_seg, b_proj, stagger, true);
        Points pieceA_b = walk_ring(A, b_seg, b_proj, stagger, false);
        // Prefer the A direction whose closing link does not cross the first link, then the shorter one.
        auto segments_cross = [](const Point &a1, const Point &a2, const Point &b1, const Point &b2) -> bool {
            auto ccw = [](const Point &p, const Point &q, const Point &r) -> double {
                return double(q.x() - p.x()) * double(r.y() - p.y()) - double(q.y() - p.y()) * double(r.x() - p.x());
            };
            return ccw(a1, a2, b1) * ccw(a1, a2, b2) < 0. && ccw(b1, b2, a1) * ccw(b1, b2, a2) < 0.;
        };
        bool   f_cross = segments_cross(pieceB.back(), b_proj, pieceA_f.back(), pieceB.front());
        bool   b_cross = segments_cross(pieceB.back(), b_proj, pieceA_b.back(), pieceB.front());
        double f_len   = (pieceA_f.back() - pieceB.front()).cast<double>().squaredNorm();
        double b_len   = (pieceA_b.back() - pieceB.front()).cast<double>().squaredNorm();
        Points &pieceA = (f_cross != b_cross) ? (b_cross ? pieceA_f : pieceA_b)
                                              : (f_len <= b_len ? pieceA_f : pieceA_b);
        Polyline merged;
        merged.points.reserve(pieceA.size() + pieceB.size());
        merged.points.insert(merged.points.end(), pieceB.begin(), pieceB.end());
        merged.points.insert(merged.points.end(), pieceA.begin(), pieceA.end());
        rings[bi] = std::move(merged);
        rings.erase(rings.begin() + bj);
        merged_any = true;
        sp_profile_count(&SPProfile::splice_merges);
        rebuild_retrace();
    }

    // Attach any leftover ring to the closest open polyline (odd multiline with an open centerline:
    // a serpentine over an odd number of rows cannot close, so the centerline stays an open path and
    // the ring wall around it must be reached from it). The path detours from its closest point
    // around the whole ring and back: the out and return links share the junction point but land on
    // the ring one stagger apart, so they diverge instead of overlapping.
    for (bool attached_any = true; attached_any && ! rings.empty() && ! open.empty(); ) {
        attached_any = false;
        SPTimer sp_timer_(SPProfile::phSpliceAttachScan);
        // Same growing-radius collection as the ring-ring scan above (the all-pairs enumeration
        // was 5.2s of the stool slice): per-set segment trees, exact d2 windows, stable_sort with
        // enumeration-order ties - decision-preserving for the same reasons.
        struct AttCand { double d2; size_t r, o, k, seg; Point entry, junction; bool insert_junction; };
        std::vector<AttCand> acands;
        acands.reserve(256);
        std::vector<std::vector<Line>>                                  ring_lines(rings.size()), open_lines(open.size());
        std::vector<std::optional<AABBTreeLines::LinesDistancer<Line>>> ring_tree(rings.size()), open_tree(open.size());
        for (size_t r = 0; r < rings.size(); ++ r) {
            const Points &R = rings[r].points;
            ring_lines[r].reserve(R.size());
            for (size_t s = 0; s < R.size(); ++ s)
                ring_lines[r].emplace_back(R[s], R[(s + 1) % R.size()]);
            ring_tree[r].emplace(ring_lines[r]);
        }
        for (size_t o = 0; o < open.size(); ++ o) {
            const Points &P = open[o].points;
            if (P.size() >= 2) {
                open_lines[o].reserve(P.size() - 1);
                for (size_t k = 0; k + 1 < P.size(); ++ k)
                    open_lines[o].emplace_back(P[k], P[k + 1]);
                open_tree[o].emplace(open_lines[o]);
            }
        }
        size_t br = 0, bo = 0, bk = 0, b_seg = 0;
        Point  b_entry, b_junction;
        bool   b_insert_junction = false;
        bool   b_found = false;
        size_t tried   = 0;
        failed_links.clear();
        std::vector<size_t> near_segs2;
        double r_prev2 = 0.;
        for (double rad = std::min(std::max(8. * stagger, scale_(1.)), max_link_distance); ; ) {
            const double r2 = std::min(rad * rad, max_link2);
            acands.clear();
            for (size_t r = 0; r < rings.size(); ++ r)
                for (size_t o = 0; o < open.size(); ++ o) {
                    const Points &R = rings[r].points;
                    const Points &P = open[o].points;
                    // Path vertex against ring segments...
                    for (size_t k = 0; k < P.size(); ++ k) {
                        near_segs2 = ring_tree[r]->all_lines_in_radius(P[k], rad * 1.01);
                        std::sort(near_segs2.begin(), near_segs2.end());
                        for (size_t s : near_segs2) {
                            const Line &ln   = ring_lines[r][s];
                            Point       proj = closest_on_segment(P[k], ln.a, ln.b);
                            double      d    = (P[k] - proj).cast<double>().squaredNorm();
                            if (d >= r_prev2 && d < r2)
                                acands.push_back({ d, r, o, k, s, proj, P[k], false });
                        }
                    }
                    // ... and ring vertex against path segments (the junction is inserted into the path).
                    if (open_tree[o])
                        for (size_t v = 0; v < R.size(); ++ v) {
                            near_segs2 = open_tree[o]->all_lines_in_radius(R[v], rad * 1.01);
                            std::sort(near_segs2.begin(), near_segs2.end());
                            for (size_t k : near_segs2) {
                                const Line &ln   = open_lines[o][k];
                                Point       proj = closest_on_segment(R[v], ln.a, ln.b);
                                double      d    = (R[v] - proj).cast<double>().squaredNorm();
                                if (d >= r_prev2 && d < r2)
                                    acands.push_back({ d, r, o, k, v, R[v], proj, true });
                            }
                        }
                }
            std::stable_sort(acands.begin(), acands.end(), [](const AttCand &l, const AttCand &r) { return l.d2 < r.d2; });
            bool budget_out = false;
            for (const AttCand &c : acands) {
                if (near_failed(c.junction, c.entry))
                    continue; // same invalid region: skip without spending budget
                if (++ tried > max_validations) {
                    budget_out = true;
                    break;
                }
                if (link_valid(c.junction, c.entry)) {
                    br = c.r; bo = c.o; bk = c.k; b_seg = c.seg;
                    b_entry = c.entry; b_junction = c.junction; b_insert_junction = c.insert_junction;
                    b_found = true;
                    break;
                }
                failed_links.emplace_back(c.junction, c.entry);
            }
            if (b_found || budget_out || rad >= max_link_distance)
                break;
            r_prev2 = r2;
            rad     = std::min(rad * 4., max_link_distance);
        }
        if (! b_found)
            break;
        const Points &R      = rings[br].points;
        Points        walk_f = walk_ring(R, b_seg, b_entry, stagger, true);
        Points        walk_b = walk_ring(R, b_seg, b_entry, stagger, false);
        Points       &walk   = (walk_f.back() - b_junction).cast<double>().squaredNorm() <
                               (walk_b.back() - b_junction).cast<double>().squaredNorm() ? walk_f : walk_b;
        Points ins;
        ins.reserve(walk.size() + 2);
        if (b_insert_junction)
            ins.emplace_back(b_junction);
        ins.insert(ins.end(), walk.begin(), walk.end());
        ins.emplace_back(b_junction); // return link, back to the junction point
        open[bo].points.insert(open[bo].points.begin() + bk + 1, ins.begin(), ins.end());
        rings.erase(rings.begin() + br);
        attached_any = true;
        rebuild_retrace();
    }

    // Re-close the rings and emit everything.
    for (Polyline &pl : rings) {
        if (pl.points.front() != pl.points.back())
            pl.points.emplace_back(pl.points.front());
        loops.emplace_back(std::move(pl));
    }
    append(loops, std::move(open));
}

// Append the boundary points from the vertex at idx_from to the vertex at idx_to (excluding the start
// point, including the end point), walking the contour forward (increasing indices) or backward.
static inline void single_path_append_arc(Points &dst, const Points &contour, size_t idx_from, size_t idx_to, bool forward)
{
    if (idx_from == idx_to)
        return;
    size_t i = idx_from;
    do {
        if (forward) {
            if (++ i == contour.size())
                i = 0;
        } else {
            if (i == 0)
                i = contour.size();
            -- i;
        }
        dst.emplace_back(contour[i]);
    } while (i != idx_to);
}

// Cura-style single-path infill: Eulerian-trail connector.
// Builds a small multigraph and extracts as few extrusion trails as possible:
//   - vertices:        infill fragment end points projected onto the boundary contours,
//   - mandatory edges: the infill fragments themselves (each extruded exactly once),
//   - optional edges:  boundary "gaps" between consecutive vertices along a contour, each usable at
//                      most once (a gap is extruded along the inner wall, never twice).
// Gap selection: (1) greedy in-order matching along each contour (the natural serpentine, CuraEngine's
// connectLines rule of never joining two ends of the same chain), then (2) a repair pass merging the
// remaining components with unused gaps - an extrusion along the inner wall, never a travel move.
// A single gap between two components always exists when they share a contour (the component id has to
// change somewhere along the contour, and that boundary gap cannot have been selected), so all
// components sharing a boundary always merge; the repair simply prefers merges that do not increase the
// number of odd-degree vertices. (3) Odd-degree reduction: loose ends cancel pairwise against free gaps,
// "walking" along the contour when needed, which turns open serpentines into closed circuits; a closed
// trail is emitted as an ExtrusionLoop downstream, so the G-code generator starts it wherever the
// previous wall ended -> no wall->infill travel. Finally Hierholzer's algorithm extracts maximal trails;
// every trail is one travel-free path (components with more than two odd-degree vertices decompose into
// several trails gracefully).
static void connect_infill_single_path(Polylines &&infill_ordered, const BoundaryInfillGraph &graph, const double spacing, Polylines &polylines_out, const bool final_emission, const double line_w, const double anchor_max, const bool wall_lining = false)
{
    sp_profile_count(&SPProfile::islands);
    // Cost of one OPEN trail, measured in closed pieces (see count_trails below). For the final
    // emission an open trail's two fixed ends each force a long, arrival-independent travel, so it
    // is worth up to (open_trail_cost - 1) extra closed loops to avoid one. Intermediate multiline
    // rows are re-closed by connect-before-multiply anyway: keep the plain trail count there (1).
    // `line_w` is the reliable extrusion width in scaled units (lightning's `spacing` is not);
    // `anchor_max` (scaled) is the user's material-generosity knob for closure overlap.
    const size_t open_trail_cost = final_emission ? 4 : 1;
    const std::vector<ContourIntersectionPoint> &cps = graph.map_infill_end_point_to_boundary;
    const size_t n_fragments = infill_ordered.size();
    const size_t n_vertices  = 2 * n_fragments;
    assert(cps.size() == n_vertices);
    const size_t out_begin = polylines_out.size();
    // Island region for the physical link checks (bridges and welds are real extruded beads:
    // they must stay inside the part). Built lazily: most calls never bridge nor weld.
    Polygons island_polys;
    auto island_region = [&island_polys, &graph]() -> const Polygons & {
        if (island_polys.empty() && ! graph.boundary.empty()) {
            island_polys.reserve(graph.boundary.size());
            for (const Points &c : graph.boundary)
                island_polys.emplace_back(c);
        }
        return island_polys;
    };

    // Fragments that cannot take part in the graph: degenerate, closed (e.g. a ring lying fully inside
    // the surface) or with an end point that could not be projected onto the boundary.
    std::vector<bool> standalone(n_fragments, false);
    for (size_t i = 0; i < n_fragments; ++ i) {
        const Polyline &pl = infill_ordered[i];
        standalone[i] = pl.size() < 2 || pl.points.front() == pl.points.back() ||
                        cps[2 * i].contour_idx == boundary_idx_unconnected ||
                        cps[2 * i + 1].contour_idx == boundary_idx_unconnected;
    }

    struct SinglePathEdge {
        size_t v1, v2;      // vertex = index into cps; for fragment edges v1 = 2i (front), v2 = 2i + 1 (back)
        bool   is_gap;
        bool   is_virtual;  // odd-degree pairing edge: never extruded, splits the trail (one travel move)
        size_t contour_idx; // gaps only
        size_t gap_pos;     // gaps only: gap index along the contour
        bool   active;      // a released gap stays in the adjacency lists but is skipped everywhere
        bool   used;
    };
    std::vector<SinglePathEdge>      edges;
    std::vector<std::vector<size_t>> adjacency(n_vertices);
    // Union-find over vertices, tracking connected components while gaps are only being added
    // (passes 1 and 2). Pass 3 releases gaps, where connectivity is re-checked with a BFS instead.
    std::vector<size_t> uf_parent(n_vertices);
    std::iota(uf_parent.begin(), uf_parent.end(), 0);
    auto uf_find = [&uf_parent](size_t x) -> size_t {
        while (uf_parent[x] != x) {
            uf_parent[x] = uf_parent[uf_parent[x]];
            x = uf_parent[x];
        }
        return x;
    };
    auto add_edge = [&edges, &adjacency, &uf_parent, &uf_find](size_t v1, size_t v2, bool is_gap, size_t contour_idx, size_t gap_pos) {
        adjacency[v1].emplace_back(edges.size());
        adjacency[v2].emplace_back(edges.size());
        edges.push_back({ v1, v2, is_gap, false, contour_idx, gap_pos, true, false });
        uf_parent[uf_find(v1)] = uf_find(v2);
    };

    bool has_fragments = false;
    for (size_t i = 0; i < n_fragments; ++ i)
        if (! standalone[i]) {
            add_edge(2 * i, 2 * i + 1, false, 0, 0);
            has_fragments = true;
        }

    if (has_fragments) {
        // Vertices of each contour, sorted along the contour.
        std::vector<std::vector<size_t>> contour_vertices(graph.boundary.size());
        for (size_t v = 0; v < n_vertices; ++ v)
            if (! standalone[v / 2] && cps[v].contour_idx != boundary_idx_unconnected)
                contour_vertices[cps[v].contour_idx].emplace_back(v);
        for (std::vector<size_t> &cv : contour_vertices)
            std::sort(cv.begin(), cv.end(), [&cps](size_t l, size_t r) { return cps[l].param < cps[r].param; });

        // A gap arc that runs nearly PARALLEL to an infill fragment at less than one extrusion width
        // apart would re-extrude that fragment: a row nearly tangent to the wall plus the boundary
        // stretch right under it would be printed twice (an out-and-back spur in the path). Block such
        // gaps for good - the solver treats them as nonexistent and routes around them.
        std::vector<std::vector<char>> gap_blocked(graph.boundary.size());
        {
            SPTimer sp_timer_(SPProfile::phGapBlocked);
            std::vector<Line> fragment_lines;
            for (size_t i = 0; i < n_fragments; ++ i)
                if (! standalone[i])
                    for (size_t j = 1; j < infill_ordered[i].size(); ++ j)
                        fragment_lines.emplace_back(infill_ordered[i].points[j - 1], infill_ordered[i].points[j]);
            AABBTreeLines::LinesDistancer<Line> distancer(fragment_lines);
            const double dist_colliding      = 0.8 * scale_(spacing);
            const double max_coincident      = 1.5 * scale_(spacing);
            const double cos_parallel        = cos(25. * M_PI / 180.);
            for (size_t c = 0; c < contour_vertices.size(); ++ c) {
                const std::vector<size_t> &cv = contour_vertices[c];
                gap_blocked[c].assign(cv.size(), 0);
                const Points &contour = graph.boundary[c];
                for (size_t k = 0; k < cv.size(); ++ k) {
                    size_t idx_from = cps[cv[k]].point_idx;
                    size_t idx_to   = cps[cv[(k + 1) % cv.size()]].point_idx;
                    double coincident = 0.;
                    for (size_t i = idx_from; i != idx_to && coincident <= max_coincident; ) {
                        const Point &prev = contour[i];
                        if (++ i == contour.size())
                            i = 0;
                        Vec2d  seg = (contour[i] - prev).cast<double>();
                        double len = seg.norm();
                        if (len <= 0.)
                            continue;
                        Point mid((prev + contour[i]) / 2);
                        auto [dist, line_idx, np] = distancer.distance_from_lines_extra<false>(mid);
                        if (dist < dist_colliding) {
                            Vec2d frag = (fragment_lines[line_idx].b - fragment_lines[line_idx].a).cast<double>();
                            double frag_len = frag.norm();
                            if (frag_len > 0. && std::abs(seg.dot(frag)) > cos_parallel * len * frag_len)
                                coincident += len;
                        }
                    }
                    if (coincident > max_coincident)
                        gap_blocked[c][k] = 1;
                }
            }
        }

        // Gap k of a contour joins its k-th and (k+1 modulo m)-th vertex along the contour.
        std::vector<std::vector<char>>   gap_taken(graph.boundary.size());
        std::vector<std::vector<size_t>> gap_edge(graph.boundary.size()); // (contour, gap) -> edge index
        std::vector<unsigned>            gap_degree(n_vertices, 0);
        for (size_t c = 0; c < contour_vertices.size(); ++ c) {
            gap_taken[c].assign(contour_vertices[c].size(), 0);
            gap_edge[c].assign(contour_vertices[c].size(), std::numeric_limits<size_t>::max());
        }
        auto take_gap = [&contour_vertices, &gap_taken, &gap_edge, &gap_degree, &edges, &add_edge, &gap_blocked](size_t c, size_t k, bool force_blocked = false) {
            if (gap_blocked[c][k] && ! force_blocked)
                // Re-extrusion of a coincident fragment - this gap may never be selected. Callers
                // (the initial phase and the sector flips) just leave a defect here; the repair
                // machinery and the Euler augmentation handle it like any other deleted gap.
                // force_blocked is the trail-closure LAST RESORT only (pass 3a (c) below).
                return;
            const std::vector<size_t> &cv = contour_vertices[c];
            size_t v1 = cv[k];
            size_t v2 = cv[(k + 1) % cv.size()];
            if (gap_edge[c][k] != std::numeric_limits<size_t>::max() && ! edges[gap_edge[c][k]].active)
                // Re-activate a previously released gap edge.
                edges[gap_edge[c][k]].active = true;
            else {
                gap_edge[c][k] = edges.size();
                add_edge(v1, v2, true, c, k);
            }
            gap_taken[c][k] = 1;
            ++ gap_degree[v1];
            ++ gap_degree[v2];
        };
        auto release_gap = [&contour_vertices, &gap_taken, &gap_edge, &gap_degree, &edges](size_t c, size_t k) {
            const std::vector<size_t> &cv = contour_vertices[c];
            assert(gap_taken[c][k] && gap_edge[c][k] != std::numeric_limits<size_t>::max());
            edges[gap_edge[c][k]].active = false;
            gap_taken[c][k] = 0;
            -- gap_degree[cv[k]];
            -- gap_degree[cv[(k + 1) % cv.size()]];
        };
        auto gap_length = [&graph, &cps](size_t c, size_t v_from, size_t v_to) -> double {
            double d = cps[v_to].param - cps[v_from].param;
            if (d < 0.)
                d += graph.boundary_params[c].back();
            return d;
        };

        // Pass 1+2: alternating-phase selection with greedy sector flips.
        // With every gap available each boundary contour is a ring and the fragments are chords, so
        // every vertex has degree 3. A selection where every vertex keeps exactly ONE of its two gaps
        // (all degrees even) is an alternating "phase" of kept gaps along the ring; a phase boundary
        // ("defect") makes its vertex odd. The number of trails is max(1, odd/2): a pure connected
        // phase is one closed loop, two defects give one open path, and so on. Strategy: start from
        // the per-contour phase that minimizes the component count, then greedily flip sectors
        // (toggling every gap state inside a range) - a flip merges components across its boundary
        // and flips that start or end at existing defects move or cancel them. Whatever odd vertices
        // survive are paired by the Euler augmentation below (one travel each).
        {
            auto component_labels = [&adjacency, &edges, &standalone, n_vertices](std::vector<int> &comp) -> size_t {
                comp.assign(n_vertices, -1);
                std::vector<size_t> stack;
                int                 count = 0;
                for (size_t v = 0; v < n_vertices; ++ v) {
                    if (standalone[v / 2] || comp[v] >= 0)
                        continue;
                    comp[v] = count;
                    stack.assign(1, v);
                    while (! stack.empty()) {
                        size_t u = stack.back();
                        stack.pop_back();
                        for (size_t e : adjacency[u])
                            if (edges[e].active) {
                                size_t w = edges[e].v1 == u ? edges[e].v2 : edges[e].v1;
                                if (comp[w] < 0) {
                                    comp[w] = count;
                                    stack.emplace_back(w);
                                }
                            }
                    }
                    ++ count;
                }
                return size_t(count);
            };
            auto count_odd = [&contour_vertices, &gap_degree]() -> size_t {
                size_t odd = 0;
                for (const std::vector<size_t> &cv : contour_vertices)
                    for (size_t v : cv)
                        if ((gap_degree[v] % 2) == 0)
                            ++ odd;
                return odd;
            };
            // Cost of a selection, in TRAVEL terms at G-code export. A component with all degrees
            // even comes out as ONE CLOSED loop: the exporter enters it wherever the toolhead
            // happens to arrive (free seam), so it costs one short hop. A component with 2k odd
            // vertices comes out as k OPEN trails whose two fixed ends the toolhead must reach and
            // leave EXACTLY there, wherever the construction dropped them - on the real part those
            // forced approaches were 60-290mm each, repeated identically on every layer. An open
            // trail is therefore priced as `open_trail_cost` closed pieces: the selection prefers
            // splitting into a couple of closed loops over one open trail (e.g. a RING region can
            // only close as two stacked serpentine cycles), but does not shatter the surface into
            // arbitrarily many pieces. Note that splitting a component CAN reduce the cost when it
            // splits the odd vertices as well (releasing the one bridge gap between the two ends of
            // an open ring trail turns it into two closed loops).
            auto count_trails = [&contour_vertices, &gap_degree, &component_labels, open_trail_cost]() -> size_t {
                sp_profile_count(&SPProfile::trails_calls);
                std::vector<int> comp;
                size_t n_comp = component_labels(comp);
                if (n_comp == 0)
                    return 0;
                std::vector<size_t> odd(n_comp, 0);
                for (const std::vector<size_t> &cv : contour_vertices)
                    for (size_t v : cv)
                        if ((gap_degree[v] % 2) == 0)
                            ++ odd[size_t(comp[v])];
                size_t cost = 0;
                for (size_t o : odd)
                    cost += o == 0 ? size_t(1) : open_trail_cost * (o / 2);
                return cost;
            };
            auto toggle_gap = [&contour_vertices, &gap_taken, &take_gap, &release_gap](size_t c, size_t k) {
                gap_taken[c][k] ? release_gap(c, k) : take_gap(c, k);
            };
            auto toggle_range = [&contour_vertices, &toggle_gap](size_t c, size_t first, size_t len) {
                const size_t m = contour_vertices[c].size();
                for (size_t l = 0; l < len; ++ l)
                    toggle_gap(c, (first + l) % m);
            };

            // Exact min-pieces solve for a SINGLE-contour island: with 0 defects the selection must
            // alternate (2 candidates); fixing a defect PAIR determines the whole selection up to the
            // same phase bit (the alternation propagates across every non-defect vertex and holds at
            // a defect). The full space is 2 + 2*C(m,2) candidates, small enough to enumerate, and
            // the greedy passes below cannot reach many of its optima: from k closed loops down to
            // one open trail the count_trails cost rises before it falls, which a strictly improving
            // search never crosses (measured on the fumidai H layers: 9 pieces greedy vs 1 exact).
            // NOT under wall lining (lightning): its phase choice must keep the max-wall-coverage
            // tie-break of the greedy swap below (the lining "second wall"), which this cost model
            // does not carry.
            bool exact_solved = false;
            if (! wall_lining && contour_vertices.size() == 1 && contour_vertices[0].size() >= 4 &&
                contour_vertices[0].size() <= 160 && (contour_vertices[0].size() % 2) == 0) {
                SPTimer sp_timer_(SPProfile::phExactSolve);
                const std::vector<size_t> &cv = contour_vertices[0];
                const size_t m = cv.size();
                // Fragment (chord) connectivity is selection-independent: precompute per-vertex pairs.
                std::vector<std::pair<size_t, size_t>> chord_pairs; // ring positions
                {
                    std::vector<size_t> pos_of(n_vertices, std::numeric_limits<size_t>::max());
                    for (size_t p = 0; p < m; ++ p)
                        pos_of[cv[p]] = p;
                    for (size_t i = 0; i < n_fragments; ++ i)
                        if (! standalone[i])
                            chord_pairs.emplace_back(pos_of[2 * i], pos_of[2 * i + 1]);
                }
                std::vector<size_t> uf(m);
                auto solve_components = [&](const std::vector<char> &sel) -> size_t {
                    std::iota(uf.begin(), uf.end(), 0);
                    auto find = [&](size_t x) {
                        while (uf[x] != x) { uf[x] = uf[uf[x]]; x = uf[x]; }
                        return x;
                    };
                    for (const auto &cp : chord_pairs)
                        uf[find(cp.first)] = find(cp.second);
                    for (size_t k = 0; k < m; ++ k)
                        if (sel[k])
                            uf[find(k)] = find((k + 1) % m);
                    size_t comps = 0;
                    for (size_t v = 0; v < m; ++ v)
                        if (find(v) == v)
                            ++ comps;
                    return comps;
                };
                // Build the selection determined by a defect set (ring positions) and the phase bit.
                // sel[k] is the state of the gap between vertex k and k+1; crossing a NON-defect
                // vertex flips it, a defect keeps it. Consistent iff the wrap parity matches.
                auto build_sel = [&](size_t da, size_t db, bool phase, std::vector<char> &sel) -> bool {
                    bool cur = phase;
                    for (size_t k = 0; k < m; ++ k) {
                        sel[k] = cur;
                        const size_t v = (k + 1) % m;
                        if (v != da && v != db)
                            cur = ! cur;
                    }
                    const bool v0_defect = (da == 0 || db == 0);
                    return v0_defect ? (sel[0] == sel[m - 1]) : (sel[0] != sel[m - 1]);
                };
                auto vpt = [&](size_t pos) -> const Point & {
                    const size_t v = cv[pos];
                    return graph.boundary[cps[v].contour_idx][cps[v].point_idx];
                };
                // Cost: pieces first, then blocked gaps taken (each is a stretch of doubled bead),
                // then open ends, then the mouth span (shorter = cheaper arrival if unclosed).
                struct ExactBest { size_t comps, blocked, defects; double mouth; std::vector<char> sel; };
                ExactBest best { std::numeric_limits<size_t>::max(), 0, 0, 0., {} };
                std::vector<char> sel(m);
                auto consider = [&](size_t da, size_t db, bool phase) {
                    if (! build_sel(da, db, phase, sel))
                        return;
                    const size_t comps = solve_components(sel);
                    size_t blocked_used = 0;
                    for (size_t k = 0; k < m; ++ k)
                        if (sel[k] && gap_blocked[0][k])
                            ++ blocked_used;
                    const size_t ndef  = (da == db) ? 0 : 2;
                    const double mouth = ndef ? (vpt(da) - vpt(db)).cast<double>().norm() : 0.;
                    if (comps < best.comps ||
                        (comps == best.comps && (blocked_used < best.blocked ||
                         (blocked_used == best.blocked && (ndef < best.defects ||
                          (ndef == best.defects && mouth < best.mouth)))))) {
                        best = { comps, blocked_used, ndef, mouth, sel };
                    }
                };
                // 0-defect candidates: the sentinel matches no vertex, so the alternation runs the
                // whole ring (the two pure phases).
                const size_t no_defect = std::numeric_limits<size_t>::max();
                for (int phase = 0; phase < 2; ++ phase)
                    consider(no_defect, no_defect, phase != 0);
                for (size_t da = 0; da < m; ++ da)
                    for (size_t db = da + 1; db < m; ++ db)
                        for (int phase = 0; phase < 2; ++ phase)
                            consider(da, db, phase != 0);
                if (! best.sel.empty()) {
                    for (size_t k = 0; k < m; ++ k)
                        if (best.sel[k])
                            take_gap(0, k, /* force_blocked */ gap_blocked[0][k] != 0);
                    exact_solved = true;
                    if (::getenv("GINGER_SINGLE_PATH_DEBUG") != nullptr)
                        std::fprintf(stderr, "[SPEXACT] m=%zu pieces=%zu blocked=%zu defects=%zu mouth=%.1fmm\n",
                                     m, best.comps, best.blocked, best.defects, best.mouth * SCALING_FACTOR);
                }
            }

            // Initial alternating phase per contour.
            if (! exact_solved) {
                SPTimer sp_timer_(SPProfile::phInitPhase);
                for (size_t c = 0; c < contour_vertices.size(); ++ c)
                    if (contour_vertices[c].size() >= 2)
                        for (size_t k = 0; k < contour_vertices[c].size(); k += 2)
                            if (k + 1 < contour_vertices[c].size() || (contour_vertices[c].size() % 2) == 0)
                                take_gap(c, k);
            }
            // Extruded wall coverage of one contour's current phase (taken gap length).
            auto contour_coverage = [&contour_vertices, &gap_taken, &gap_length](size_t c) -> double {
                const std::vector<size_t> &cv = contour_vertices[c];
                double cov = 0.;
                for (size_t k = 0; k < cv.size(); ++ k)
                    if (gap_taken[c][k])
                        cov += gap_length(c, cv[k], cv[(k + 1) % cv.size()]);
                return cov;
            };
            // Greedy per-contour phase swap (toggle all gaps of one contour) while it helps.
            // WALL LINING (final lightning emission): the two phases of a contour often cost the
            // same in trails - e.g. a lone tree rooted on the wall gives one phase with two tiny
            // arcs and one with the whole wall hug. Demand-driven lightning leaves whole bands of
            // layers with such lone trees, and the tiny-arc phase drops the inner lining bead
            // ("second wall") that every high-demand layer has: banding on the inner surface and
            // no rail carrying the walk to the wall seam (rib). On ties, prefer the phase that
            // covers MORE wall - material is explicitly not a concern (pellet printing).
            if (! exact_solved) {
                SPTimer sp_timer_(SPProfile::phPhaseSwap);
                size_t trails = count_trails();
                for (size_t c = 0; c < contour_vertices.size(); ++ c) {
                    const size_t m = contour_vertices[c].size();
                    if (m < 2)
                        continue;
                    const size_t odds_before = wall_lining ? count_odd() : 0;
                    const double cov_before  = wall_lining ? contour_coverage(c) : 0.;
                    toggle_range(c, 0, m);
                    size_t trails_swapped = count_trails();
                    if (trails_swapped < trails)
                        trails = trails_swapped;
                    else if (wall_lining && final_emission && trails_swapped == trails &&
                             count_odd() == odds_before && contour_coverage(c) > cov_before)
                        ; // equal cost, more wall covered: keep the lining phase
                    else
                        toggle_range(c, 0, m); // revert
                }
            }

            // Greedy sector flips. Candidate sector boundaries: gaps adjacent to defect vertices and
            // deleted gaps on a component frontier; a sector is any range between two candidates of
            // the same contour. Accept the flip that lexicographically improves
            // (components, odd vertices, flipped range length).
            for (size_t guard = 0; ! exact_solved && guard < n_vertices + 16; ++ guard) {
                SPTimer sp_timer_(SPProfile::phSectorFlips);
                sp_profile_count(&SPProfile::flip_iters);
                std::vector<int> comp;
                component_labels(comp);
                size_t trails = count_trails();
                size_t odds   = count_odd();
                if (trails <= 1 && odds == 0)
                    break;
                size_t best_c = 0, best_first = 0, best_len = 0;
                size_t best_trails = trails, best_odds = odds;
                for (size_t c = 0; c < contour_vertices.size(); ++ c) {
                    const std::vector<size_t> &cv = contour_vertices[c];
                    const size_t m = cv.size();
                    if (m < 2)
                        continue;
                    // Candidate boundary gap positions on this contour.
                    std::vector<size_t> cand;
                    for (size_t pos = 0; pos < m; ++ pos) {
                        if ((gap_degree[cv[pos]] % 2) == 0) {
                            // defect vertex: both its gaps qualify as boundaries
                            cand.emplace_back((pos + m - 1) % m);
                            cand.emplace_back(pos);
                        }
                        if (! gap_taken[c][pos] && comp[cv[pos]] != comp[cv[(pos + 1) % m]]) {
                            // deleted frontier gap
                            cand.emplace_back(pos);
                            cand.emplace_back((pos + 1) % m);
                        }
                    }
                    std::sort(cand.begin(), cand.end());
                    cand.erase(std::unique(cand.begin(), cand.end()), cand.end());
                    if (cand.size() < 2)
                        continue;
                    if (cand.size() > 48)
                        // Keep the search bounded on huge islands; the guard loop iterates anyway.
                        cand.resize(48);
                    for (size_t a = 0; a < cand.size(); ++ a)
                        for (size_t b = 0; b < cand.size(); ++ b) {
                            if (a == b)
                                continue;
                            size_t first = cand[a];
                            size_t len   = (cand[b] + m - cand[a]) % m;
                            if (len == 0 || len >= m)
                                continue;
                            sp_profile_count(&SPProfile::flip_cands);
                            toggle_range(c, first, len);
                            size_t trails2 = count_trails();
                            size_t odds2   = count_odd();
                            toggle_range(c, first, len); // revert
                            if (trails2 < best_trails ||
                                (trails2 == best_trails && odds2 < best_odds) ||
                                (trails2 == best_trails && odds2 == best_odds && best_len != 0 && len < best_len)) {
                                best_c = c; best_first = first; best_len = len;
                                best_trails = trails2; best_odds = odds2;
                            }
                        }
                }
                if (best_len == 0 || (best_trails == trails && best_odds >= odds))
                    break;
                toggle_range(best_c, best_first, best_len);
            }

            // Pass 3a: the flips above minimize the trail / defect COUNT, but they leave the surviving
            // defects wherever the search stopped, and every defect pair still costs one travel move
            // (a virtual edge below) whose LENGTH nobody optimized - measured on the real part, those
            // were 60-290mm jumps at a fixed, arrival-independent spot. Two mitigations, both pure
            // extrusion (no travel, no re-extrusion):
            //
            // (a) free-run closure: when two defects are joined by a run of consecutive FREE gaps, take
            //     the whole run. The run's interior vertices gain degree 2 (parity kept), the two ends
            //     gain 1 each -> both defects resolved, and every component the run touches merges into
            //     one, so the trail count can only drop. The run is extruded along the inner wall like
            //     any other gap. Shortest runs first; a run longer than a quarter of its contour is
            //     rejected (a near-full wall-hug is worse than the travel it saves). The flip search
            //     above can miss these: its candidate list is capped and a blocked gap inside a range
            //     silently breaks a flip.
            //
            // (b) defect sliding: pair the remaining defects like the virtual pairing below will
            //     (greedy nearest, per component) and walk them toward each other one gap at a time.
            //     Toggling a single gap flips the parity of exactly its two end vertices, i.e. it
            //     moves the defect to the adjacent vertex. Taking an untaken gap only ever merges
            //     components (always safe); releasing a taken gap may disconnect the serpentine, so
            //     that step is validated with count_trails() and rolled back when it hurts. A blocked
            //     gap stops the walk. Sliding does not remove the final travel move, but it shrinks
            //     it - and once the two ends are within stitch range, run_trail below closes the
            //     trail into a LOOP whose seam the G-code generator places freely (that is the real
            //     prize: a closed sparse path is entered wherever the toolhead happens to arrive).
            if (final_emission && ! exact_solved) {
            size_t sp_runs_closed = 0, sp_slide_steps = 0;
            {
            SPTimer sp_timer_(SPProfile::phFreeRunClosure);
            for (;;) {
                size_t best_c = 0, best_first = 0, best_len = 0;
                double best_cost = std::numeric_limits<double>::max();
                for (size_t c = 0; c < contour_vertices.size(); ++ c) {
                    const std::vector<size_t> &cv = contour_vertices[c];
                    const size_t m = cv.size();
                    if (m < 2)
                        continue;
                    // Free runs hug the wall with clean (non-overlapped) bead; a quarter contour is
                    // the conservative default, the user's anchor length raises it (unlimited for
                    // Davide: the wall-hug connection IS the desired behaviour, material is not a
                    // concern on these prints).
                    const double max_run = std::max(0.25 * graph.boundary_params[c].back(), anchor_max);
                    std::vector<size_t> defects;
                    for (size_t k = 0; k < m; ++ k)
                        if ((gap_degree[cv[k]] % 2) == 0)
                            defects.emplace_back(k);
                    for (size_t a = 0; a + 1 < defects.size(); ++ a)
                        for (size_t b = a + 1; b < defects.size(); ++ b)
                            for (int dir = 0; dir < 2; ++ dir) {
                                const size_t from = dir ? defects[b] : defects[a];
                                const size_t to   = dir ? defects[a] : defects[b];
                                const size_t len  = (to + m - from) % m;
                                if (len == 0 || len >= m)
                                    continue;
                                const double cost = gap_length(c, cv[from], cv[to]);
                                if (cost > max_run || cost >= best_cost)
                                    continue;
                                bool free_run = true;
                                for (size_t l = 0; l < len && free_run; ++ l) {
                                    const size_t k = (from + l) % m;
                                    free_run = ! gap_taken[c][k] && ! gap_blocked[c][k];
                                }
                                if (free_run) {
                                    best_cost = cost; best_c = c; best_first = from; best_len = len;
                                }
                            }
                }
                if (best_len == 0)
                    break;
                const size_t m = contour_vertices[best_c].size();
                for (size_t l = 0; l < best_len; ++ l)
                    take_gap(best_c, (best_first + l) % m);
                ++ sp_runs_closed;
            }
            } // free-run timer scope
            {
                SPTimer sp_timer_(SPProfile::phDefectSlide);
                std::vector<int> comp;
                component_labels(comp);
                constexpr size_t no_pos = std::numeric_limits<size_t>::max();
                // vertex -> its position along its contour (index into contour_vertices[c]).
                std::vector<std::pair<size_t, size_t>> vpos(n_vertices, { no_pos, 0 });
                for (size_t c = 0; c < contour_vertices.size(); ++ c)
                    for (size_t k = 0; k < contour_vertices[c].size(); ++ k)
                        vpos[contour_vertices[c][k]] = { c, k };
                auto defect_point = [&graph, &cps](size_t v) -> const Point& {
                    return graph.boundary[cps[v].contour_idx][cps[v].point_idx];
                };
                std::vector<std::vector<size_t>> defects_by_comp;
                for (const std::vector<size_t> &cv : contour_vertices)
                    for (size_t v : cv)
                        if ((gap_degree[v] % 2) == 0) {
                            if (defects_by_comp.size() <= size_t(comp[v]))
                                defects_by_comp.resize(size_t(comp[v]) + 1);
                            defects_by_comp[size_t(comp[v])].emplace_back(v);
                        }
                for (std::vector<size_t> &defs : defects_by_comp)
                    while (defs.size() >= 2) {
                        // Nearest pair first, mirroring the virtual pairing of pass 3b.
                        size_t bi = 0, bj = 1;
                        double bd = std::numeric_limits<double>::max();
                        for (size_t i = 0; i < defs.size(); ++ i)
                            for (size_t j = i + 1; j < defs.size(); ++ j) {
                                const double d = (defect_point(defs[j]) - defect_point(defs[i])).cast<double>().squaredNorm();
                                if (d < bd) { bd = d; bi = i; bj = j; }
                            }
                        size_t va = defs[bi], vb = defs[bj];
                        defs.erase(defs.begin() + bj);
                        defs.erase(defs.begin() + bi);
                        bool closed = false;
                        for (int who = 0; who < 2 && ! closed; ++ who) {
                            size_t self  = who ? vb : va;
                            size_t other = who ? va : vb;
                            for (int guard2 = 0; guard2 < 512; ++ guard2) {
                                const auto [c, k] = vpos[self];
                                if (c == no_pos)
                                    break;
                                const std::vector<size_t> &cv = contour_vertices[c];
                                const size_t m = cv.size();
                                if (m < 2)
                                    break;
                                const double d_cur = (defect_point(other) - defect_point(self)).cast<double>().norm();
                                size_t best_gap = no_pos, best_to = 0;
                                double best_d   = d_cur;
                                for (int dir = 0; dir < 2; ++ dir) {
                                    const size_t gk = dir ? (k + m - 1) % m : k;
                                    const size_t vt = dir ? cv[(k + m - 1) % m] : cv[(k + 1) % m];
                                    if (vt == self || gap_blocked[c][gk])
                                        continue;
                                    const double d = (defect_point(other) - defect_point(vt)).cast<double>().norm();
                                    if (d < best_d - SCALED_EPSILON) { best_d = d; best_gap = gk; best_to = vt; }
                                }
                                if (best_gap == no_pos)
                                    break;
                                const bool   releases      = gap_taken[c][best_gap] != 0;
                                const size_t trails_before = releases ? count_trails() : 0;
                                const size_t odds_before   = count_odd();
                                toggle_gap(c, best_gap);
                                if (releases && count_trails() > trails_before) {
                                    toggle_gap(c, best_gap); // roll back: it would split the serpentine
                                    break;
                                }
                                ++ sp_slide_steps;
                                sp_profile_count(&SPProfile::slide_steps);
                                if (count_odd() + 2 <= odds_before) {
                                    // Stepped onto another defect: both resolved, the trail closed here.
                                    closed = true;
                                    break;
                                }
                                self = best_to;
                            }
                        }
                    }
            }

            // (c) LAST RESORT: the surviving pairs sit behind arcs the earlier passes cannot use -
            // BLOCKED gaps (the arc runs parallel to a racetrack hugging the wall: taking it is a
            // stretch of overlapped bead) and/or TAKEN gaps (already serving as serpentine links).
            // On the real part these are the top-ring layers, whose 3-metre trails stay open with a
            // 30-140mm mouth that costs a ~250mm fixed-end arrival travel on EVERY layer. Fix by
            // TOGGLING a whole arc between the two defects: a range toggle flips the parity of
            // exactly its two boundary vertices (both defects -> both resolved), releasing the taken
            // gaps inside (the serpentine re-routes; under the seam-free cost splitting into CLOSED
            // pieces is acceptable) and taking the untaken ones, including short blocked stretches
            // (force_blocked). Guards: the forced overlapped-bead length is hard-capped, and the
            // toggle is reverted unless the defect pair is resolved without raising the cost.
            {
            SPTimer sp_timer_(SPProfile::phLastResort);
            for (;;) {
                // Overlap budget for the forced blocked stretches: at least 25 line widths, and
                // beyond that whatever the user's infill-anchor length allows (Davide runs it
                // unlimited: closing the trail into a loop outranks material use on pellet).
                const double max_blocked_take = std::max(25. * line_w, anchor_max);
                bool applied = false;
                // Candidate arcs, shortest first, so the rewiring stays local.
                struct Arc3c { size_t c, first, len; double cost; };
                std::vector<Arc3c> cand;
                for (size_t c = 0; c < contour_vertices.size(); ++ c) {
                    const std::vector<size_t> &cv = contour_vertices[c];
                    const size_t m = cv.size();
                    if (m < 2)
                        continue;
                    std::vector<size_t> defects;
                    for (size_t k = 0; k < m; ++ k)
                        if ((gap_degree[cv[k]] % 2) == 0)
                            defects.emplace_back(k);
                    for (size_t a = 0; a + 1 < defects.size(); ++ a)
                        for (size_t b = a + 1; b < defects.size(); ++ b)
                            for (int dir = 0; dir < 2; ++ dir) {
                                const size_t from = dir ? defects[b] : defects[a];
                                const size_t to   = dir ? defects[a] : defects[b];
                                const size_t len  = (to + m - from) % m;
                                if (len == 0 || len >= m)
                                    continue;
                                double blocked_take = 0.;
                                for (size_t l = 0; l < len; ++ l) {
                                    const size_t k = (from + l) % m;
                                    if (gap_blocked[c][k] && ! gap_taken[c][k])
                                        blocked_take += gap_length(c, cv[k], cv[(k + 1) % m]);
                                }
                                if (blocked_take <= max_blocked_take)
                                    cand.push_back({ c, from, len, gap_length(c, cv[from], cv[to]) });
                            }
                }
                std::sort(cand.begin(), cand.end(), [](const Arc3c &l, const Arc3c &r) { return l.cost < r.cost; });
                for (const Arc3c &a : cand) {
                    const std::vector<size_t> &cv = contour_vertices[a.c];
                    const size_t m = cv.size();
                    // The defect pair may already be resolved by a previous toggle.
                    if ((gap_degree[cv[a.first]] % 2) != 0 || (gap_degree[cv[(a.first + a.len) % m]] % 2) != 0)
                        continue;
                    const size_t trails_before = count_trails();
                    const size_t odds_before   = count_odd();
                    auto flip_range = [&]() {
                        for (size_t l = 0; l < a.len; ++ l) {
                            const size_t k = (a.first + l) % m;
                            if (gap_taken[a.c][k])
                                release_gap(a.c, k);
                            else
                                take_gap(a.c, k, /* force_blocked */ true);
                        }
                    };
                    flip_range();
                    if (count_odd() + 2 <= odds_before && count_trails() <= trails_before) {
                        ++ sp_runs_closed;
                        applied = true;
                        break;
                    }
                    flip_range(); // revert
                }
                if (! applied)
                    break;
            }
            } // last-resort timer scope
            if (::getenv("GINGER_SINGLE_PATH_DEBUG") != nullptr) {
                std::fprintf(stderr, "[SPCLOSE] runs_closed=%zu slide_steps=%zu odds_left=%zu\n",
                             sp_runs_closed, sp_slide_steps, count_odd());
                for (const std::vector<size_t> &cv : contour_vertices)
                    for (size_t v : cv)
                        if ((gap_degree[v] % 2) == 0) {
                            const Point &p = graph.boundary[cps[v].contour_idx][cps[v].point_idx];
                            std::fprintf(stderr, "[SPDEFECT] v=%zu contour=%zu at (%.1f, %.1f) gap_taken(prev,next)=(%d,%d) gap_blocked(prev,next)=(%d,%d)\n",
                                         v, cps[v].contour_idx, p.x() * SCALING_FACTOR, p.y() * SCALING_FACTOR,
                                         [&]{ const auto pr = std::find(contour_vertices[cps[v].contour_idx].begin(), contour_vertices[cps[v].contour_idx].end(), v);
                                              const size_t k = size_t(pr - contour_vertices[cps[v].contour_idx].begin());
                                              const size_t m = contour_vertices[cps[v].contour_idx].size();
                                              return int(gap_taken[cps[v].contour_idx][(k + m - 1) % m]); }(),
                                         [&]{ const auto pr = std::find(contour_vertices[cps[v].contour_idx].begin(), contour_vertices[cps[v].contour_idx].end(), v);
                                              const size_t k = size_t(pr - contour_vertices[cps[v].contour_idx].begin());
                                              return int(gap_taken[cps[v].contour_idx][k]); }(),
                                         [&]{ const auto pr = std::find(contour_vertices[cps[v].contour_idx].begin(), contour_vertices[cps[v].contour_idx].end(), v);
                                              const size_t k = size_t(pr - contour_vertices[cps[v].contour_idx].begin());
                                              const size_t m = contour_vertices[cps[v].contour_idx].size();
                                              return int(gap_blocked[cps[v].contour_idx][(k + m - 1) % m]); }(),
                                         [&]{ const auto pr = std::find(contour_vertices[cps[v].contour_idx].begin(), contour_vertices[cps[v].contour_idx].end(), v);
                                              const size_t k = size_t(pr - contour_vertices[cps[v].contour_idx].begin());
                                              return int(gap_blocked[cps[v].contour_idx][k]); }());
                        }
            }
            } // if (final_emission)
        }

        // Pass 3b: Euler augmentation. The stack-based Hierholzer below is only valid on Eulerian
        // components (all degrees even, or exactly two odd ones when starting at an odd vertex);
        // anything else corrupts the vertex/edge association and materializes retraced segments
        // (double extrusion). Pair ALL remaining odd-degree vertices of each component with virtual
        // edges - a virtual edge is never extruded, it splits the trail there (one travel move), which
        // is exactly the topological lower bound. Components without odd vertices come out as closed
        // loops, components with 2k odd vertices as k open trails.
        {
            SPTimer sp_timer_(SPProfile::phAugmentation);
            // Component labels (BFS over active edges).
            std::vector<int> comp(n_vertices, -1);
            int              n_comp = 0;
            {
                std::vector<size_t> stack;
                for (size_t v = 0; v < n_vertices; ++ v) {
                    if (standalone[v / 2] || comp[v] >= 0)
                        continue;
                    comp[v] = n_comp;
                    stack.assign(1, v);
                    while (! stack.empty()) {
                        size_t u = stack.back();
                        stack.pop_back();
                        for (size_t e : adjacency[u])
                            if (edges[e].active) {
                                size_t w = edges[e].v1 == u ? edges[e].v2 : edges[e].v1;
                                if (comp[w] < 0) {
                                    comp[w] = n_comp;
                                    stack.emplace_back(w);
                                }
                            }
                    }
                    ++ n_comp;
                }
            }
            // Odd-degree vertices per component (degree = 1 fragment + gap_degree -> odd when
            // gap_degree is even). A virtual edge is where the trail gets split, i.e. where the
            // printer travels: pair the odd vertices greedily by EUCLIDEAN distance so that every
            // travel move is as short as possible.
            std::vector<std::vector<size_t>> odd_by_comp(n_comp);
            for (size_t v = 0; v < n_vertices; ++ v)
                if (! standalone[v / 2] && (gap_degree[v] % 2) == 0)
                    odd_by_comp[size_t(comp[v])].emplace_back(v);
            for (std::vector<size_t> &odd : odd_by_comp) {
                assert((odd.size() % 2) == 0);
                while (odd.size() >= 2) {
                    size_t best_i = 0, best_j = 1;
                    double best_d = std::numeric_limits<double>::max();
                    for (size_t i = 0; i < odd.size(); ++ i)
                        for (size_t j = i + 1; j < odd.size(); ++ j) {
                            const Point &pi = graph.boundary[cps[odd[i]].contour_idx][cps[odd[i]].point_idx];
                            const Point &pj = graph.boundary[cps[odd[j]].contour_idx][cps[odd[j]].point_idx];
                            double d = (pj - pi).cast<double>().squaredNorm();
                            if (d < best_d) {
                                best_d = d;
                                best_i = i;
                                best_j = j;
                            }
                        }
                    sp_profile_count(&SPProfile::augment_pairs);
                    adjacency[odd[best_i]].emplace_back(edges.size());
                    adjacency[odd[best_j]].emplace_back(edges.size());
                    edges.push_back({ odd[best_i], odd[best_j], false, true, 0, 0, true, false });
                    // best_j > best_i: erase in this order to keep indices valid.
                    odd.erase(odd.begin() + best_j);
                    odd.erase(odd.begin() + best_i);
                }
            }
        }

        // Pass 4: extract Eulerian trails (Hierholzer) and materialize them into polylines.
        std::vector<size_t> cursor(n_vertices, 0);
        auto next_unused = [&adjacency, &edges, &cursor](size_t v) -> int {
            const std::vector<size_t> &lst = adjacency[v];
            size_t                    &cur = cursor[v];
            while (cur < lst.size() && (edges[lst[cur]].used || ! edges[lst[cur]].active))
                ++ cur;
            return cur < lst.size() ? int(lst[cur]) : -1;
        };
        auto vertex_point = [&graph, &cps](size_t v) -> const Point& {
            return graph.boundary[cps[v].contour_idx][cps[v].point_idx];
        };
        // Mouth-stitch limit for nearly-closed trails: ends this close get joined by one extruded
        // segment so the trail is emitted as a closed loop (seam freely placeable at export). A few
        // line spacings covers ends facing each other across a hole / rib corridor, which the defect
        // sliding above brings together but can never merge exactly (different contours).
        const double stitch_max = final_emission ? 2.5 * line_w : 0.;
        // Virtual edges are extruded (bridged) instead of split whenever physically sound. Up to
        // 1.5 line widths no check is needed: too short to lay more doubled bead than a legal gap
        // arc is allowed anyway. Beyond that the PHYSICAL rule applies (no length policy): the
        // bridge must stay inside the island and must not retrace an extruded fragment.
        const double bridge_free = final_emission ? 1.5 * line_w : 0.;
        size_t sp_bridged = 0;
        double sp_bridged_len = 0.;
        std::vector<Line> bridge_frag_lines;
        std::optional<AABBTreeLines::LinesDistancer<Line>> bridge_dist;
        auto bridge_valid = [&](const Point &a, const Point &b) -> bool {
            if (! final_emission)
                return false;
            const Vec2d  v   = (b - a).cast<double>();
            const double len = v.norm();
            if (len <= 0.)
                return true;
            {
                sp_profile_count(&SPProfile::clipper_calls);
                Lines  kept = intersection_ln(Line(a, b), island_region());
                double tot  = 0.;
                for (const Line &l : kept)
                    tot += l.length();
                if (tot + SCALED_EPSILON < 0.999 * len)
                    return false;
            }
            if (! bridge_dist) {
                for (size_t i = 0; i < n_fragments; ++ i)
                    if (! standalone[i])
                        for (size_t k = 1; k < infill_ordered[i].size(); ++ k)
                            bridge_frag_lines.emplace_back(infill_ordered[i].points[k - 1], infill_ordered[i].points[k]);
                bridge_dist.emplace(bridge_frag_lines);
            }
            if (bridge_frag_lines.empty())
                return true;
            const Vec2d  dir     = v / len;
            const double step    = 0.5 * line_w;
            const double cos_par = 0.90630779; // cos 25deg
            double coinc = 0.;
            for (double s = 0.5 * step; s < len && coinc <= 1.5 * line_w; s += step) {
                const Point p((a.cast<double>() + dir * s).cast<coord_t>());
                const auto [d, idx, np] = bridge_dist->distance_from_lines_extra<false>(p);
                if (std::abs(d) < 0.8 * line_w) {
                    const Vec2d  t  = (bridge_frag_lines[size_t(idx)].b - bridge_frag_lines[size_t(idx)].a).cast<double>();
                    const double tn = t.norm();
                    if (tn > 0. && std::abs(dir.dot(t)) > cos_par * tn)
                        coinc += step;
                }
            }
            return coinc <= 1.5 * line_w;
        };
        auto run_trail = [&adjacency, &edges, &cps, &graph, &infill_ordered, &polylines_out, &next_unused, &vertex_point, stitch_max, bridge_free, &bridge_valid, &sp_bridged, &sp_bridged_len](size_t start) {
            std::vector<std::pair<size_t, int>> stack, trail; // (vertex, edge used to arrive)
            stack.emplace_back(start, -1);
            while (! stack.empty()) {
                std::pair<size_t, int> top = stack.back();
                int e = next_unused(top.first);
                if (e < 0) {
                    trail.emplace_back(top);
                    stack.pop_back();
                } else {
                    edges[size_t(e)].used = true;
                    stack.emplace_back(edges[size_t(e)].v1 == top.first ? edges[size_t(e)].v2 : edges[size_t(e)].v1, e);
                }
            }
            if (trail.size() < 2)
                return;
            std::reverse(trail.begin(), trail.end());
            Polylines pieces;
            Polyline  pl;
            pl.points.emplace_back(vertex_point(trail.front().first));
            for (size_t i = 1; i < trail.size(); ++ i) {
                size_t                v_from = trail[i - 1].first;
                size_t                v_to   = trail[i].first;
                const SinglePathEdge &e      = edges[size_t(trail[i].second)];
                if (e.is_virtual) {
                    const double jump = (vertex_point(v_to) - vertex_point(v_from)).cast<double>().norm();
                    if (jump <= bridge_free || bridge_valid(vertex_point(v_from), vertex_point(v_to))) {
                        // Bridge: extrude across the split instead of traveling (a hop costs a
                        // retract/wipe cycle and an ERS transition; the bead costs nothing on
                        // pellet). Also the only way to join defects on DIFFERENT contours,
                        // which no gap arc can ever connect.
                        pl.points.emplace_back(vertex_point(v_to));
                        ++ sp_bridged;
                        sp_bridged_len += jump;
                    } else {
                        // Trail split point: never extruded, the printer travels here.
                        if (pl.size() > 1)
                            pieces.emplace_back(std::move(pl));
                        pl = Polyline();
                        pl.points.emplace_back(vertex_point(v_to));
                    }
                } else if (e.is_gap)
                    single_path_append_arc(pl.points, graph.boundary[e.contour_idx],
                                           cps[v_from].point_idx, cps[v_to].point_idx, /* forward */ v_from == e.v1);
                else {
                    const Polyline &frag = infill_ordered[v_from / 2];
                    if ((v_from & 1) == 0)
                        pl.points.insert(pl.points.end(), frag.points.begin() + 1, frag.points.end());
                    else
                        pl.points.insert(pl.points.end(), frag.points.rbegin() + 1, frag.points.rend());
                }
            }
            if (pl.size() > 1)
                pieces.emplace_back(std::move(pl));
            // The circuit starts at an arbitrary vertex: when it was split by virtual edges, the first
            // and the last piece are the two halves of one and the same trail - join them back.
            if (pieces.size() > 1 && pieces.front().points.front() == pieces.back().points.back()) {
                pieces.back().points.insert(pieces.back().points.end(), pieces.front().points.begin() + 1, pieces.front().points.end());
                pieces.front() = std::move(pieces.back());
                pieces.pop_back();
            }
            // Close a nearly-closed piece: extrude one short segment across the mouth so the polyline
            // is closed and gets emitted as an ExtrusionLoop downstream - the G-code generator then
            // starts this whole trail wherever the toolhead arrives (a fixed pair of open ends was the
            // single biggest source of long, arrival-independent travels on the real part).
            if (stitch_max > 0.) {
                const bool sp_debug = ::getenv("GINGER_SINGLE_PATH_DEBUG") != nullptr;
                for (Polyline &piece : pieces)
                    if (piece.size() > 2 && piece.points.front() != piece.points.back()) {
                        const double mouth = (piece.points.back() - piece.points.front()).cast<double>().norm();
                        if (mouth <= stitch_max)
                            piece.points.emplace_back(piece.points.front());
                        else if (sp_debug)
                            std::fprintf(stderr, "[SPOPEN] mouth=%.1fmm len=%.1fmm\n",
                                         mouth * SCALING_FACTOR, piece.length() * SCALING_FACTOR);
                    }
            }
            append(polylines_out, std::move(pieces));
        };
        // After the Euler augmentation every component is a circuit; any vertex with unused edges may
        // start it. A circuit containing k virtual edges materializes into k open trails.
        {
            SPTimer sp_timer_(SPProfile::phHierholzerEmit);
            for (size_t v = 0; v < n_vertices; ++ v)
                if (! standalone[v / 2] && next_unused(v) >= 0)
                    run_trail(v);
        }
        if (sp_bridged > 0 && ::getenv("GINGER_SINGLE_PATH_DEBUG") != nullptr)
            std::fprintf(stderr, "[SPBRIDGE] bridged=%zu total=%.1fmm\n", sp_bridged, sp_bridged_len * SCALING_FACTOR);
    }

    // Emit the fragments that could not take part in the graph.
    for (size_t i = 0; i < n_fragments; ++ i)
        if (standalone[i] && infill_ordered[i].size() > 1)
            polylines_out.emplace_back(std::move(infill_ordered[i]));

    // Close-range weld of the emitted pieces (splice, per Davide). A residual closed loop next to
    // another piece costs two chainer hops per layer (enter at the free seam, hop back out); welding
    // it in costs two extruded links instead. Link budget 2.5 line widths: below that the doubled
    // stretch where the diverging links leave the shared junction stays within the coincidence the
    // blocked rule already tolerates on a legal gap arc.
    if (final_emission && polylines_out.size() - out_begin > 1) {
        SPTimer sp_timer_(SPProfile::phJoinWeld);
        Polylines tail(std::make_move_iterator(polylines_out.begin() + out_begin),
                       std::make_move_iterator(polylines_out.end()));
        polylines_out.erase(polylines_out.begin() + out_begin, polylines_out.end());
        const size_t before_join = tail.size();
        // Join OPEN pieces whose ends nearly touch: a trail split re-emitted as two pieces sharing
        // an almost-exact endpoint otherwise survives as a sub-bead travel move. The joint segment
        // is extruded; 1.5 line widths keeps it inside the tolerated coincidence.
        const double join_max2 = (1.5 * line_w) * (1.5 * line_w);
        for (bool joined = true; joined; ) {
            joined = false;
            for (size_t i = 0; i < tail.size() && ! joined; ++ i) {
                Polyline &a = tail[i];
                if (a.size() < 2 || a.points.front() == a.points.back())
                    continue;
                for (size_t j = i + 1; j < tail.size(); ++ j) {
                    Polyline &b = tail[j];
                    if (b.size() < 2 || b.points.front() == b.points.back())
                        continue;
                    const double d_bf = (a.points.back()  - b.points.front()).cast<double>().squaredNorm();
                    const double d_bb = (a.points.back()  - b.points.back()).cast<double>().squaredNorm();
                    const double d_ff = (a.points.front() - b.points.front()).cast<double>().squaredNorm();
                    const double d_fb = (a.points.front() - b.points.back()).cast<double>().squaredNorm();
                    const double dmin = std::min(std::min(d_bf, d_bb), std::min(d_ff, d_fb));
                    if (dmin > join_max2)
                        continue;
                    if      (dmin == d_bb) b.reverse();
                    else if (dmin == d_ff) a.reverse();
                    else if (dmin == d_fb) { a.reverse(); b.reverse(); }
                    a.points.insert(a.points.end(),
                                    a.points.back() == b.points.front() ? b.points.begin() + 1 : b.points.begin(),
                                    b.points.end());
                    tail.erase(tail.begin() + j);
                    joined = true;
                    break;
                }
            }
        }
        // Weld residual pieces into the walk under the physical rule only (no length policy):
        // the search is bounded by the island extent, acceptance by inside-island + no-retrace.
        const size_t before_weld = tail.size();
        BoundingBox  bb;
        for (const Polygon &p : island_region())
            bb.merge(get_extents(p));
        const double search_reach = std::max((bb.max - bb.min).cast<double>().norm(), 10. * line_w);
        single_path_splice_loops(tail, search_reach, line_w, &island_region());
        if (tail.size() != before_join && ::getenv("GINGER_SINGLE_PATH_DEBUG") != nullptr)
            std::fprintf(stderr, "[SPWELD] pieces %zu -> %zu (join %zu, weld %zu)\n",
                         before_join, tail.size(), before_join - before_weld, before_weld - tail.size());
        append(polylines_out, std::move(tail));
    }
}

void Fill::connect_infill(Polylines &&infill_ordered, const std::vector<const Polygon*> &boundary_src, const BoundingBox &bbox, Polylines &polylines_out, const double spacing, const FillParams &params)
{
	assert(! infill_ordered.empty());
    assert(params.anchor_length     >= 0.);
    assert(params.anchor_length_max >= 0.01f);
    assert(params.anchor_length_max >= params.anchor_length);
    double anchor_length     = scale_(params.anchor_length);
    double anchor_length_max = scale_(params.anchor_length_max);

#if 0
    append(polylines_out, infill_ordered);
    return;
#endif

    // Cura-style single-path infill: multiline_fill() produces closed rings whose seam vertex can fall
    // inside the fill surface. intersection_pl() then splits such a ring into two open fragments that meet
    // at the seam - an interior point which can never be projected onto the boundary graph below, leaving
    // the fragments unconnectable. Stitch fragments sharing an exact endpoint back together first; this is
    // always valid (no travel, no extra extrusion) wherever the shared point lies.
    if (params.connect_polygons && infill_ordered.size() > 1) {
        for (bool stitched = true; stitched; ) {
            stitched = false;
            for (size_t i = 0; i < infill_ordered.size() && ! stitched; ++ i) {
                Polyline &a = infill_ordered[i];
                if (a.empty() || a.points.front() == a.points.back())
                    continue;
                for (size_t j = i + 1; j < infill_ordered.size(); ++ j) {
                    Polyline &b = infill_ordered[j];
                    if (b.empty() || b.points.front() == b.points.back())
                        continue;
                    if      (a.points.back()  == b.points.front()) {}
                    else if (a.points.back()  == b.points.back())  b.reverse();
                    else if (a.points.front() == b.points.front()) a.reverse();
                    else if (a.points.front() == b.points.back())  { a.reverse(); b.reverse(); }
                    else continue;
                    a.points.insert(a.points.end(), b.points.begin() + 1, b.points.end());
                    b.points.clear();
                    stitched = true;
                    break;
                }
            }
        }
        infill_ordered.erase(std::remove_if(infill_ordered.begin(), infill_ordered.end(), [](const Polyline &pl) { return pl.empty(); }), infill_ordered.end());
        if (infill_ordered.empty())
            return;
    }

    // Deviate a fill-line stretch that grazes the boundary (Davide's green line): a stretch running
    // nearly tangent to the contour inside the collision distance makes gap_blocked veto the whole
    // arc under it - on the H part the only arcs that could weld the leg cells into the serpentine
    // (a 157mm scanline tangent to each leg fillet for ~27mm). Re-route just that stretch to sit
    // exactly one line width off the boundary: bead beside bead, the arc becomes legal, and the
    // endpoints stay put so the boundary graph is untouched. Interior stretches only - line ends
    // legitimately land on the contour. NOT under wall lining (lightning): there a wall-hugging
    // stretch is the PRODUCT - the racetrack lining is the "second wall" fused to the perimeter -
    // while on scanline patterns it is a geometric accident. Same signature, opposite meaning.
    if (params.connect_polygons && ! params.sparse_wall_lining) {
        const double line_w = params.flow.scaled_width() > 0 ? double(params.flow.scaled_width()) : scale_(spacing);
        Lines bnd;
        for (const Polygon *p : boundary_src)
            append(bnd, p->lines());
        if (! bnd.empty() && line_w > 0.) {
            AABBTreeLines::LinesDistancer<Line> wall(bnd);
            const double d_trip    = 0.8 * line_w;  // the gap_blocked collision distance
            const double d_target  = line_w;        // deviated stretch: one bead off the wall
            const double end_guard = 2.0 * line_w;
            const double scan_step = 0.5 * line_w;
            const double min_run   = 1.5 * line_w;  // the gap_blocked coincidence threshold
            auto point_at = [](const Polyline &pl, double s) -> Point {
                for (size_t i = 0; i + 1 < pl.size(); ++ i) {
                    const double l = (pl.points[i + 1] - pl.points[i]).cast<double>().norm();
                    if (s <= l || i + 2 == pl.size()) {
                        const double t = l > 0. ? std::min(1., s / l) : 0.;
                        return Point((pl.points[i].cast<double>() * (1. - t) + pl.points[i + 1].cast<double>() * t).cast<coord_t>());
                    }
                    s -= l;
                }
                return pl.points.back();
            };
            // Direction of the polyline SEGMENT containing arc length s - same walk as point_at.
            // The riding test must compare segment vs segment (as gap_blocked does): a forward
            // difference taken across a pattern corner points into the next flank and would break
            // a legitimate grazing run right where the trapezoid wave folds.
            auto seg_dir_at = [](const Polyline &pl, double s) -> Vec2d {
                for (size_t i = 0; i + 1 < pl.size(); ++ i) {
                    const Vec2d  v = (pl.points[i + 1] - pl.points[i]).cast<double>();
                    const double l = v.norm();
                    if (s <= l || i + 2 == pl.size())
                        return v;
                    s -= l;
                }
                return Vec2d(0., 0.);
            };
            size_t sp_deviated = 0;
            for (Polyline &pl : infill_ordered) {
                if (pl.size() < 2)
                    continue;
                const double len = pl.length();
                if (len <= 2. * end_guard + min_run)
                    continue;
                // Scan the interior for grazing runs. Same riding metric as gap_blocked: a
                // sample counts only when the line also runs PARALLEL (<25deg) to the projected
                // boundary segment - an angled near-miss (a concave spike reaching toward the
                // line) is not a graze and must not trigger a reroute.
                std::vector<std::pair<double, double>> runs;
                double run_start = -1.;
                for (double s = end_guard; s <= len - end_guard + 0.5 * scan_step; s += scan_step) {
                    const Point q = point_at(pl, s);
                    const auto [d, li, np] = wall.distance_from_lines_extra<false>(q);
                    bool grazing = std::abs(d) < d_trip;
                    if (grazing) {
                        const Line  &bl = bnd[size_t(li)];
                        const Vec2d  bt = (bl.b - bl.a).cast<double>();
                        const Vec2d  ct = seg_dir_at(pl, s);
                        const double bn = bt.norm(), cn = ct.norm();
                        grazing = bn > 0. && cn > 0. && std::abs(ct.dot(bt)) > 0.90630779 * bn * cn; // cos 25deg
                    }
                    if (grazing) {
                        if (run_start < 0.)
                            run_start = s;
                    } else if (run_start >= 0.) {
                        if (s - run_start > min_run)
                            runs.emplace_back(run_start, s);
                        run_start = -1.;
                    }
                }
                if (run_start >= 0. && len - end_guard - run_start > min_run)
                    runs.emplace_back(run_start, len - end_guard);
                if (runs.empty())
                    continue;
                // Compute the replacement points per run - the offset direction comes from the
                // projected boundary SEGMENT's left normal (material is always to the left of a
                // Slic3r contour/hole), which stays stable at distance ~0 where the
                // point-to-projection direction does not - and validate them against the OTHER
                // fill lines: at high densities the deviated stretch could land on the next
                // scanline. A colliding run keeps its original geometry (its arc simply stays
                // blocked, exactly as before this pass existed).
                std::vector<Points> repl(runs.size());
                {
                    std::vector<Line> other_lines;
                    for (const Polyline &pl2 : infill_ordered)
                        if (&pl2 != &pl)
                            for (size_t i2 = 0; i2 + 1 < pl2.size(); ++ i2)
                                other_lines.emplace_back(pl2.points[i2], pl2.points[i2 + 1]);
                    std::optional<AABBTreeLines::LinesDistancer<Line>> others;
                    if (! other_lines.empty())
                        others.emplace(other_lines);
                    for (size_t r = 0; r < runs.size(); ++ r) {
                        double coinc = 0.; // parallel-coincident stretch vs the other fill lines
                        for (double s = runs[r].first; s <= runs[r].second + 1e-9 && coinc <= 1.5 * line_w; s += scan_step) {
                            const Point  q = point_at(pl, std::min(s, runs[r].second));
                            const auto [d, li, np] = wall.distance_from_lines_extra<false>(q);
                            const Line  &bl  = bnd[size_t(li)];
                            Vec2d        dir = (bl.b - bl.a).cast<double>();
                            const double dn  = dir.norm();
                            if (dn <= 0.)
                                continue;
                            const Vec2d nrm(-dir.y() / dn, dir.x() / dn); // left normal = inward
                            const Point dev((Vec2d(np.x(), np.y()) + nrm * d_target).cast<coord_t>());
                            if (others) {
                                // Same physical rule as everywhere else: only RIDING another line
                                // is a collision - parallel (<25deg) closer than 0.8 bead for more
                                // than 1.5 bead accumulated. Passing near a chord end at an angle
                                // is the normal neighborhood of the fillet and must not veto.
                                const auto [od, oi, onp] = others->distance_from_lines_extra<false>(dev);
                                if (std::abs(od) < 0.8 * line_w) {
                                    const Line  &ol = (*others).get_line(size_t(oi));
                                    const Vec2d  ot = (ol.b - ol.a).cast<double>();
                                    const double on = ot.norm();
                                    if (on > 0. && std::abs(dir.dot(ot)) > 0.90630779 * dn * on) // cos 25deg
                                        coinc += scan_step;
                                }
                            }
                            if (repl[r].empty() || (dev - repl[r].back()).cast<double>().norm() > 0.2 * line_w)
                                repl[r].emplace_back(dev);
                        }
                        if (coinc > 1.5 * line_w)
                            repl[r].clear(); // would ride another line: keep the original geometry
                    }
                }
                {
                    std::vector<std::pair<double, double>> keep;
                    std::vector<Points>                    keep_repl;
                    for (size_t r = 0; r < runs.size(); ++ r)
                        if (! repl[r].empty()) {
                            keep.emplace_back(runs[r]);
                            keep_repl.emplace_back(std::move(repl[r]));
                        }
                    runs = std::move(keep);
                    repl = std::move(keep_repl);
                }
                if (runs.empty())
                    continue;
                // Rebuild: original vertices outside the runs, the precomputed points inside.
                Points  out;
                out.reserve(pl.size() + runs.size() * 32);
                double  acc = 0.;
                size_t  ri  = 0;
                bool    ri_emitted = false;
                out.emplace_back(pl.points.front());
                for (size_t i = 0; i + 1 < pl.size(); ++ i) {
                    const double l = (pl.points[i + 1] - pl.points[i]).cast<double>().norm();
                    const double s1 = acc + l;
                    while (ri < runs.size() && runs[ri].first < s1) {
                        if (! ri_emitted) {
                            for (const Point &dev : repl[ri])
                                if (out.empty() || (dev - out.back()).cast<double>().norm() > 0.2 * line_w)
                                    out.emplace_back(dev);
                            ri_emitted = true;
                        }
                        if (runs[ri].second <= s1 + 1e-9) {
                            ++ ri;
                            ri_emitted = false;
                        } else
                            break;
                    }
                    const bool inside_run = ri < runs.size() && runs[ri].first <= s1 && runs[ri].second >= s1;
                    if (! inside_run && (pl.points[i + 1] - out.back()).cast<double>().norm() > 0.)
                        out.emplace_back(pl.points[i + 1]);
                    acc = s1;
                }
                if (out.back() != pl.points.back())
                    out.emplace_back(pl.points.back());
                if (out.size() >= 2) {
                    pl.points = std::move(out);
                    ++ sp_deviated;
                }
            }
            if (sp_deviated > 0 && ::getenv("GINGER_SINGLE_PATH_DEBUG") != nullptr)
                std::fprintf(stderr, "[SPDEVIATE] lines=%zu\n", sp_deviated);
        }
    }

    // Cura-style single-path infill: skip boundary trimming so the whole inner wall can be traced.
    BoundaryInfillGraph graph = create_boundary_infill_graph(infill_ordered, boundary_src, bbox, spacing, params.connect_polygons);

    if (params.connect_polygons) {
        // Cura-style single-path infill: dedicated Eulerian-trail connector, replaces the greedy
        // anchor-based machinery below entirely.
        connect_infill_single_path(std::move(infill_ordered), graph, spacing, polylines_out, ! params.multiline_intermediate,
                                   params.flow.scaled_width() > 0 ? double(params.flow.scaled_width()) : scale_(spacing),
                                   double(scale_(params.anchor_length_max)),
                                   params.sparse_wall_lining);
        return;
    }

    std::vector<size_t> merged_with(infill_ordered.size());
    std::iota(merged_with.begin(), merged_with.end(), 0);

    auto get_and_update_merged_with = [&merged_with](size_t polyline_idx) -> size_t {
        for (size_t last = polyline_idx;;) {
            size_t lower = merged_with[last];
            assert(lower <= last);
            if (lower == last) {
                merged_with[polyline_idx] = last;
                return last;
            }
            last = lower;
        }
        assert(false);
        return std::numeric_limits<size_t>::max();
    };

    const double line_half_width = 0.5 * scale_(spacing);

#if 0
    // Connection from end of one infill line to the start of another infill line.
    //const double length_max = scale_(spacing);
//  const auto length_max = double(scale_((2. / params.density) * spacing));
    const auto length_max = double(scale_((1000. / params.density) * spacing));
    struct ConnectionCost {
        ConnectionCost(size_t idx_first, double cost, bool reversed) : idx_first(idx_first), cost(cost), reversed(reversed) {}
        size_t  idx_first;
        double  cost;
        bool    reversed;
    };
    std::vector<ConnectionCost> connections_sorted;
    connections_sorted.reserve(infill_ordered.size() * 2 - 2);
    for (size_t idx_chain = 1; idx_chain < infill_ordered.size(); ++ idx_chain) {
        const ContourIntersectionPoint      *cp1            = &graph.map_infill_end_point_to_boundary[(idx_chain - 1) * 2 + 1];
        const ContourIntersectionPoint      *cp2            = &graph.map_infill_end_point_to_boundary[idx_chain * 2];
        if (cp1->contour_idx != boundary_idx_unconnected && cp1->contour_idx == cp2->contour_idx) {
            // End points on the same contour. Try to connect them.
            std::pair<double, double> len = path_lengths_along_contour(cp1, cp2, graph.boundary_params[cp1->contour_idx].back());
            if (len.first < length_max)
                connections_sorted.emplace_back(idx_chain - 1, len.first, false);
            if (len.second < length_max)
                connections_sorted.emplace_back(idx_chain - 1, len.second, true);
        }
    }
    std::sort(connections_sorted.begin(), connections_sorted.end(), [](const ConnectionCost& l, const ConnectionCost& r) { return l.cost < r.cost; });

    for (ConnectionCost &connection_cost : connections_sorted) {
		ContourIntersectionPoint *cp1    = &graph.map_infill_end_point_to_boundary[connection_cost.idx_first * 2 + 1];
		ContourIntersectionPoint *cp2    = &graph.map_infill_end_point_to_boundary[(connection_cost.idx_first + 1) * 2];
        assert(cp1 != cp2);
        assert(cp1->contour_idx == cp2->contour_idx && cp1->contour_idx != boundary_idx_unconnected);
        if (cp1->consumed || cp2->consumed)
            continue;
        const double              length = connection_cost.cost;
        bool                      could_connect;
        {
            // cp1, cp2 sorted CCW.
            ContourIntersectionPoint *cp_low  = connection_cost.reversed ? cp2 : cp1;
            ContourIntersectionPoint *cp_high = connection_cost.reversed ? cp1 : cp2;
            assert(std::abs(length - closed_contour_distance_ccw(cp_low->param, cp_high->param, graph.boundary_params[cp1->contour_idx].back())) < SCALED_EPSILON);
            could_connect = ! cp_low->next_trimmed && ! cp_high->prev_trimmed;
            if (could_connect && cp_low->next_on_contour != cp_high) {
                // Other end of cp1, may or may not be on the same contour as cp1.
                const ContourIntersectionPoint *cp1prev = cp1 - 1;
                // Other end of cp2, may or may not be on the same contour as cp2.
                const ContourIntersectionPoint *cp2next = cp2 + 1;
                for (auto *cp = cp_low->next_on_contour; cp != cp_high; cp = cp->next_on_contour)
                    if (cp->consumed || cp == cp1prev || cp == cp2next || cp->prev_trimmed || cp->next_trimmed) {
                        could_connect = false;
                        break;
                    }
            }
        }
        // Indices of the polylines to be connected by a perimeter segment.
        size_t idx_first  = connection_cost.idx_first;
        size_t idx_second = idx_first + 1;
        idx_first = get_and_update_merged_with(idx_first);
        assert(idx_first < idx_second);
        assert(idx_second == merged_with[idx_second]);
        if (could_connect && length < anchor_length_max) {
            // Take the complete contour.
            // Connect the two polygons using the boundary contour.
            take(infill_ordered[idx_first], infill_ordered[idx_second], graph.boundary[cp1->contour_idx], cp1, cp2, connection_cost.reversed);
            // Mark the second polygon as merged with the first one.
            merged_with[idx_second] = merged_with[idx_first];
            infill_ordered[idx_second].points.clear();
        } else {
            // Try to connect cp1 resp. cp2 with a piece of perimeter line.
            take_limited(infill_ordered[idx_first],  graph.boundary[cp1->contour_idx], graph.boundary_params[cp1->contour_idx], cp1, cp2, connection_cost.reversed, anchor_length, line_half_width);
            take_limited(infill_ordered[idx_second], graph.boundary[cp1->contour_idx], graph.boundary_params[cp1->contour_idx], cp2, cp1, ! connection_cost.reversed, anchor_length, line_half_width);
        }
	}
#endif

    struct Arc {
        ContourIntersectionPoint    *intersection;
        double                       arc_length;
    };
    std::vector<Arc> arches;
    if (!params.dont_sort) {
        arches.reserve(graph.map_infill_end_point_to_boundary.size());
        for (ContourIntersectionPoint& cp : graph.map_infill_end_point_to_boundary)
            if (cp.contour_idx != boundary_idx_unconnected && cp.next_on_contour != &cp && cp.could_connect_next())
                arches.push_back({ &cp, path_length_along_contour_ccw(&cp, cp.next_on_contour, graph.boundary_params[cp.contour_idx].back()) });
        std::sort(arches.begin(), arches.end(), [](const auto& l, const auto& r) { return l.arc_length < r.arc_length; });
    }

    //FIXME improve the Traveling Salesman problem with 2-opt and 3-opt local optimization.
    for (Arc &arc : arches)
        if (! arc.intersection->consumed && ! arc.intersection->next_on_contour->consumed) {
            // Indices of the polylines to be connected by a perimeter segment.
            ContourIntersectionPoint *cp1            = arc.intersection;
            ContourIntersectionPoint *cp2            = arc.intersection->next_on_contour;
            size_t                    polyline_idx1  = get_and_update_merged_with(((cp1 - graph.map_infill_end_point_to_boundary.data()) / 2));
            size_t                    polyline_idx2  = get_and_update_merged_with(((cp2 - graph.map_infill_end_point_to_boundary.data()) / 2));
            const Points             &contour        = graph.boundary[cp1->contour_idx];

            // Orca: If multiline infill is requested, skip connections that are too short.
            if (params.multiline > 1 && arc.arc_length < scale_(spacing) * params.multiline) {
                continue;
            }

            const std::vector<double> &contour_params = graph.boundary_params[cp1->contour_idx];
            if (polyline_idx1 != polyline_idx2) {
                Polyline &polyline1 = infill_ordered[polyline_idx1];
                Polyline &polyline2 = infill_ordered[polyline_idx2];
                if (arc.arc_length < anchor_length_max) {
                    // Not closing a loop, connecting the lines.
                    assert(contour[cp1->point_idx] == polyline1.points.front() || contour[cp1->point_idx] == polyline1.points.back());
                    if (contour[cp1->point_idx] == polyline1.points.front())
                        polyline1.reverse();
                    assert(contour[cp2->point_idx] == polyline2.points.front() || contour[cp2->point_idx] == polyline2.points.back());
                    if (contour[cp2->point_idx] == polyline2.points.back())
                        polyline2.reverse();
                    take(polyline1, polyline2, contour, cp1, cp2, false);
                    // Mark the second polygon as merged with the first one.
                    if (polyline_idx2 < polyline_idx1) {
                        polyline2 = std::move(polyline1);
                        polyline1.points.clear();
                        merged_with[polyline_idx1] = merged_with[polyline_idx2];
                    } else {
                        polyline2.points.clear();
                        merged_with[polyline_idx2] = merged_with[polyline_idx1];
                    }
                } else if (anchor_length > SCALED_EPSILON) {
                    // Move along the perimeter, but don't take the whole arc.
                    take_limited(polyline1, contour, contour_params, cp1, cp2, false, anchor_length, line_half_width);
                    take_limited(polyline2, contour, contour_params, cp2, cp1, true,  anchor_length, line_half_width);
                }
            }
        }

    // Connect the remaining open infill lines to the perimeter lines if possible.
    for (ContourIntersectionPoint &contour_point : graph.map_infill_end_point_to_boundary)
        if (! contour_point.consumed && contour_point.contour_idx != boundary_idx_unconnected) {
            const Points              &contour        = graph.boundary[contour_point.contour_idx];
            const std::vector<double> &contour_params = graph.boundary_params[contour_point.contour_idx];

            double    lprev         = contour_point.could_connect_prev() ?
                path_length_along_contour_ccw(contour_point.prev_on_contour, &contour_point, contour_params.back()) :
                std::numeric_limits<double>::max();
            double    lnext         = contour_point.could_connect_next() ?
                path_length_along_contour_ccw(&contour_point, contour_point.next_on_contour, contour_params.back()) :
                std::numeric_limits<double>::max();
            size_t    polyline_idx  = get_and_update_merged_with(((&contour_point - graph.map_infill_end_point_to_boundary.data()) / 2));
            Polyline &polyline      = infill_ordered[polyline_idx];
            assert(! polyline.empty());
            assert(contour[contour_point.point_idx] == polyline.points.front() || contour[contour_point.point_idx] == polyline.points.back());
            bool connected = false;
            for (double l : { std::min(lprev, lnext), std::max(lprev, lnext) }) {
                if (l == std::numeric_limits<double>::max() || l > anchor_length_max)
                    break;
                // Take the complete contour.
                bool      reversed      = l == lprev;
                ContourIntersectionPoint *cp2 = reversed ? contour_point.prev_on_contour : contour_point.next_on_contour;
                // Identify which end of the polyline touches the boundary.
                size_t    polyline_idx2 = get_and_update_merged_with(((cp2 - graph.map_infill_end_point_to_boundary.data()) / 2));
                if (polyline_idx == polyline_idx2)
                    // Try the other side.
                    continue;
                // Not closing a loop.
                if (contour[contour_point.point_idx] == polyline.points.front())
                    polyline.reverse();
                Polyline &polyline2 = infill_ordered[polyline_idx2];
                assert(! polyline.empty());
                assert(contour[cp2->point_idx] == polyline2.points.front() || contour[cp2->point_idx] == polyline2.points.back());
                if (contour[cp2->point_idx] == polyline2.points.back())
                    polyline2.reverse();
                take(polyline, polyline2, contour, &contour_point, cp2, reversed);
                if (polyline_idx < polyline_idx2) {
                    // Mark the second polyline as merged with the first one.
                    merged_with[polyline_idx2] = polyline_idx;
                    polyline2.points.clear();
                } else {
                    // Mark the first polyline as merged with the second one.
                    merged_with[polyline_idx] = polyline_idx2;
                    polyline2 = std::move(polyline);
                    polyline.points.clear();
                }
                connected = true;
                break;
            }
            if (! connected && anchor_length > SCALED_EPSILON) {
                // Which to take? One could optimize for:
                // 1) Shortest path
                // 2) Hook length
                // ...
                // Let's take the longer now, as this improves the chance of another hook to be placed on the other side of this contour point.
                double l = std::max(contour_point.contour_not_taken_length_prev, contour_point.contour_not_taken_length_next);
                if (l > SCALED_EPSILON) {
                    if (contour_point.contour_not_taken_length_prev > contour_point.contour_not_taken_length_next)
                        take_limited(polyline, contour, contour_params, &contour_point, contour_point.prev_on_contour, true, anchor_length, line_half_width);
                    else
                        take_limited(polyline, contour, contour_params, &contour_point, contour_point.next_on_contour, false, anchor_length, line_half_width);
                }
            }
        }

    polylines_out.reserve(polylines_out.size() + std::count_if(infill_ordered.begin(), infill_ordered.end(), [](const Polyline &pl) { return ! pl.empty(); }));
	for (Polyline &pl : infill_ordered)
		if (! pl.empty())
			polylines_out.emplace_back(std::move(pl));
}

void Fill::chain_or_connect_infill(Polylines &&infill_ordered, const ExPolygon &boundary, Polylines &polylines_out, const double spacing, const FillParams &params)
{
    if (!infill_ordered.empty()) {
        if (params.dont_connect()) {
            if (infill_ordered.size() > 1)
                infill_ordered = chain_polylines(std::move(infill_ordered));
            append(polylines_out, std::move(infill_ordered));
        } else {
            // Ginger single-path infill cleanup around connect_infill. line_w = the CONSTANT extrusion
            // width (params.flow); lightning's `spacing` is not a reliable scale here (and at low layers
            // a multiline stub can come out OPEN, not a closed racetrack).
            const double line_w = (params.connect_polygons && params.flow.scaled_width() > 0)
                                  ? double(params.flow.scaled_width()) : scale_(spacing);

            // (2, BEFORE connect) OPEN closed loops that run along the inner wall, so connect_infill can
            // stitch them into the single path via boundary arcs. multiline>=2 turns a real lightning
            // sub-tree that REACHES the wall into a closed racetrack; connect_infill drops closed loops as
            // standalone, leaving the sub-tree as a separate fragment reached by a travel even though it
            // grazes the wall (the natural connection path). Cutting the loop open at its wall-nearest
            // vertex gives two adjacent endpoints on the wall, which the boundary graph then connects.
            // This applies to a LONE racetrack too: its cut ends project onto the boundary and the Euler
            // connector re-closes the trail through the wall arc, so the loop comes back closed WITH a
            // wall anchor (two approach beads + the arc). A lone tree in the band between two walls used
            // to float unanchored, reached by a ~100-150mm travel on 55 consecutive layers of the real
            // part. (An old guard skipped lone loops because the pre-closure connector could not re-close
            // a self-loop; the closure passes made that obsolete.)
            if (params.connect_polygons && ! infill_ordered.empty()) {
                Lines bnd = boundary.contour.lines();
                for (const Polygon &h : boundary.holes)
                    append(bnd, h.lines());
                if (! bnd.empty()) {
                    AABBTreeLines::LinesDistancer<Line> wall(bnd);
                    // Cut radius: racetracks within the user's infill-anchor reach of the wall are
                    // opened so the connector can weld them into the walk via boundary arcs - the
                    // approach from the cut vertex to the wall is EXTRUDED (an anchor bead), which
                    // on pellet printers beats the travel it replaces (a lone tree pocket 20-30mm
                    // off the wall used to stay a separate island reached by a 160mm travel).
                    // The radius honours the user's anchor setting AS IS - Davide sets it unlimited
                    // and explicitly does not care about material use ("preferiamo estrudere"); an
                    // earlier 12-line-width sanity cap silently overrode it and made identical
                    // islands weld on some layers and travel on others.
                    const double near_wall = std::max(1.5 * line_w, double(scale_(params.anchor_length_max)));
                    const bool sp_cut_debug = ::getenv("GINGER_SINGLE_PATH_DEBUG") != nullptr;
                    // An endpoint shared by two or more open fragments is an interior JUNCTION of one
                    // tree (lightning fragments split at branch points): the fragments already chain
                    // there, so the junction needs no anchor. Extending it gave every sharing fragment
                    // its own bead to the SAME nearest wall point - coincident mandatory edges the
                    // Euler trail was then FORCED to extrude twice (out-and-back full-flow spikes,
                    // ~200 on the real part). Only a UNIQUE floating end is a true dead end that
                    // needs the approach bead to enter the boundary graph.
                    std::map<Point, int> open_end_count;
                    for (const Polyline &pl : infill_ordered)
                        if (pl.points.size() >= 2 && pl.points.front() != pl.points.back()) {
                            ++ open_end_count[pl.points.front()];
                            ++ open_end_count[pl.points.back()];
                        }
                    if (sp_cut_debug) {
                        BoundingBox bb = get_extents(boundary.contour);
                        std::fprintf(stderr, "[SPCUT] z=%.2f call frags=%zu contour_pts=%zu holes=%zu bbox=(%.1f,%.1f)-(%.1f,%.1f)\n",
                                     this->z, infill_ordered.size(), boundary.contour.points.size(), boundary.holes.size(),
                                     bb.min.x() * SCALING_FACTOR, bb.min.y() * SCALING_FACTOR,
                                     bb.max.x() * SCALING_FACTOR, bb.max.y() * SCALING_FACTOR);
                        if (const char *zenv = ::getenv("GINGER_SPCUT_Z"); zenv != nullptr && std::abs(this->z - atof(zenv)) < 0.7)
                            for (const Polygon &h : boundary.holes) {
                                BoundingBox hb = get_extents(h);
                                std::fprintf(stderr, "[SPCUT] z=%.2f   hole n=%zu bbox=(%.1f,%.1f)-(%.1f,%.1f)\n",
                                             this->z, h.points.size(),
                                             hb.min.x() * SCALING_FACTOR, hb.min.y() * SCALING_FACTOR,
                                             hb.max.x() * SCALING_FACTOR, hb.max.y() * SCALING_FACTOR);
                            }
                    }
                    for (Polyline &pl : infill_ordered) {
                        if (pl.points.size() < 4 || pl.points.front() != pl.points.back()) {
                            // OPEN fragment (multiline stub, tree arm): the boundary-graph projection
                            // radius is EPSILON, so an end that does not lie exactly ON the wall makes
                            // the whole fragment unconnectable (standalone -> its own travel). Extend
                            // each floating end with a straight anchor bead to its nearest wall point,
                            // within the user's anchor reach: the end then projects and the connector
                            // welds the fragment into the walk.
                            if (pl.points.size() >= 2 && pl.points.front() != pl.points.back()) {
                                const Point orig_front = pl.points.front();
                                const Point orig_back  = pl.points.back();
                                int    extended  = 0;
                                double dbg_d[2]  = { -1., -1. };
                                for (int side = 0; side < 2; ++ side) {
                                    const Point &end = side ? pl.points.back() : pl.points.front();
                                    if (auto it = open_end_count.find(end); it != open_end_count.end() && it->second > 1)
                                        continue; // interior junction: fragments chain here, a bead would print twice
                                    auto [d, li, np] = wall.distance_from_lines_extra<false>(end);
                                    dbg_d[side] = std::abs(d);
                                    if (std::abs(d) <= double(SCALED_EPSILON) || std::abs(d) >= near_wall)
                                        continue;
                                    const Point anchor(coord_t(std::round(np.x())), coord_t(std::round(np.y())));
                                    if (anchor == (side ? pl.points.front() : pl.points.back()))
                                        continue; // degenerate: both ends onto the same wall point
                                    if (side)
                                        pl.points.emplace_back(anchor);
                                    else
                                        pl.points.insert(pl.points.begin(), anchor);
                                    ++ extended;
                                }
                                if (sp_cut_debug)
                                    std::fprintf(stderr, "[SPCUT] z=%.2f open n=%zu ends (%.1f,%.1f)d=%.1f (%.1f,%.1f)d=%.1f len=%.1f extended=%d\n",
                                                 this->z, pl.points.size(),
                                                 orig_front.x() * SCALING_FACTOR, orig_front.y() * SCALING_FACTOR, dbg_d[0] * SCALING_FACTOR,
                                                 orig_back.x() * SCALING_FACTOR, orig_back.y() * SCALING_FACTOR, dbg_d[1] * SCALING_FACTOR,
                                                 pl.length() * SCALING_FACTOR, extended);
                            }
                            continue;
                        }
                        size_t best_i = 0;
                        double best_d = std::numeric_limits<double>::max();
                        for (size_t k = 0; k + 1 < pl.points.size(); ++ k) {
                            auto [d, line_idx, np] = wall.distance_from_lines_extra<false>(pl.points[k]);
                            if (std::abs(d) < best_d) { best_d = std::abs(d); best_i = k; }
                        }
                        // A LONE loop that already hugs the wall (the single spliced loop of the
                        // trapezoidal pipeline) is a perfect closed path with a free seam: leave it.
                        // Cut a lone loop only when it floats AWAY from the wall and needs the
                        // anchor; with company, cut whenever within reach so the weld can happen.
                        const bool lone = infill_ordered.size() == 1;
                        if (sp_cut_debug) {
                            const char *verdict = (best_d < near_wall && (! lone || best_d > 1.5 * line_w)) ? "CUT" : "keep";
                            std::fprintf(stderr, "[SPCUT] z=%.2f loop n=%zu at (%.1f,%.1f) len=%.1f best_d=%.1f near_wall=%.1f lone=%d -> %s\n",
                                         this->z, pl.points.size(), pl.points.front().x() * SCALING_FACTOR, pl.points.front().y() * SCALING_FACTOR,
                                         pl.length() * SCALING_FACTOR, best_d * SCALING_FACTOR, near_wall * SCALING_FACTOR, lone ? 1 : 0, verdict);
                        }
                        if (best_d < near_wall && (! lone || best_d > 1.5 * line_w)) {
                            const size_t n   = pl.points.size() - 1; // closed: last == first
                            const Point  p_a = pl.points[best_i];
                            const Point  p_b = pl.points[(best_i + n - 1) % n];
                            // The graph projection radius is EPSILON: an end that merely comes NEAR
                            // the wall never enters the boundary graph. EXTEND both cut ends with a
                            // straight approach bead to their nearest wall points, so they lie ON
                            // the contour and the connector can weld the tree into the walk - this
                            // is the anchor Davide asks for ("lo sparse si ancora al wall"); a tree
                            // floating mid-band used to stay a separate island reached by a travel.
                            auto [da, ia, npa] = wall.distance_from_lines_extra<false>(p_a);
                            auto [db, ib, npb] = wall.distance_from_lines_extra<false>(p_b);
                            const Point a_a(coord_t(std::round(npa.x())), coord_t(std::round(npa.y())));
                            const Point a_b(coord_t(std::round(npb.x())), coord_t(std::round(npb.y())));
                            if (a_a != a_b) {
                                pl.points.pop_back();                                            // drop duplicated closing vertex
                                std::rotate(pl.points.begin(), pl.points.begin() + best_i, pl.points.end()); // open at the wall-nearest vertex
                                if ((p_a - a_a).cast<double>().squaredNorm() > double(SCALED_EPSILON) * double(SCALED_EPSILON))
                                    pl.points.insert(pl.points.begin(), a_a);
                                if ((p_b - a_b).cast<double>().squaredNorm() > double(SCALED_EPSILON) * double(SCALED_EPSILON))
                                    pl.points.emplace_back(a_b);
                            }
                        }
                    }
                }
            }

            const size_t out_start = polylines_out.size();
            connect_infill(std::move(infill_ordered), boundary, polylines_out, spacing, params);

            // (1, AFTER connect) Remove tiny isolated STRAY fragments from the connector OUTPUT (open OR
            // closed). Post-connection these are the final isolated pieces: connect_infill has already
            // stitched everything it could, so a leftover whose whole extent is < 1.5 line widths in BOTH
            // axes is a negligible lightning stub that would otherwise print as a separate fragment reached
            // by a long travel (the "micro loops" on the real part - some come out OPEN, so closure can't
            // be required). Filtering the OUTPUT (not the input) is what makes it robust: it never touches
            // the short scanlines of line patterns that connect_infill joins into the path. Real small
            // islands ("feet", ~8 mm) and the big interior trees (tens of mm) are kept.
            if (params.connect_polygons && line_w > 0.) {
                const double max_stray = 1.5 * line_w;
                polylines_out.erase(std::remove_if(polylines_out.begin() + out_start, polylines_out.end(),
                    [max_stray](const Polyline &pl) {
                        if (pl.size() < 2)
                            return true;
                        const BoundingBox bb = pl.bounding_box();
                        return double(std::max(bb.size().x(), bb.size().y())) < max_stray;
                    }), polylines_out.end());
            }
        }
    }
}

// Extend the infill lines along the perimeters, this is mainly useful for grid aligned support, where a perimeter line may be nearly
// aligned with the infill lines.
static inline void base_support_extend_infill_lines(Polylines &infill, BoundaryInfillGraph &graph, const double spacing, const FillParams &params)
{
/*
    // Backup the source lines.
    Lines lines;
    lines.reserve(linfill.size());
    std::transform(infill.begin(), infill.end(), std::back_inserter(lines), [](const Polyline &pl) { assert(pl.size() == 2); return Line(pl.points.begin(), pl.points.end()); });
*/

    const double    line_spacing    = scale_(spacing) / params.density;
    // Maximum deviation perpendicular to the infill line to allow merging as a continuation of the same infill line.
    const auto      dist_max_x      = coord_t(line_spacing * 0.33);
    // Minimum length of the arc away from the infill end point to allow merging as a continuation of the same infill line.
    const auto      dist_min_y      = coord_t(line_spacing * 0.5);

    for (ContourIntersectionPoint &cp : graph.map_infill_end_point_to_boundary) {
        const Points                &contour         = graph.boundary[cp.contour_idx];
        const std::vector<double>   &contour_param   = graph.boundary_params[cp.contour_idx];
        const Point                 &pt              = contour[cp.point_idx];
        const bool                   first           = graph.first(cp);
        int                          extend_next_idx = -1;
        int                          extend_prev_idx = -1;
        coord_t                      dist_y_prev;
        coord_t                      dist_y_next;
        double                       arc_len_prev;
        double                       arc_len_next;

        if (! graph.next_vertical(cp)){
            size_t i = cp.point_idx;
            size_t j = next_idx_modulo(i, contour);
            while (j != cp.next_on_contour->point_idx) {
                //const Point &p1 = contour[i];
                const Point &p2 = contour[j];
                if (std::abs(p2.x() - pt.x()) > dist_max_x)
                    break;
                i = j;
                j = next_idx_modulo(j, contour);
            }
            if (i != cp.point_idx) {
                const Point &p2 = contour[i];
                coord_t      dist_y = p2.y() - pt.y();
                if (first)
                    dist_y = - dist_y;
                if (dist_y > dist_min_y) {
                    arc_len_next    = closed_contour_distance_ccw(contour_param[cp.point_idx], contour_param[i], contour_param.back());
                    if (arc_len_next < cp.contour_not_taken_length_next) {
                        extend_next_idx = i;
                        dist_y_next     = dist_y;
                    }
                }
            }
        }

        if (! graph.prev_vertical(cp)) {
            size_t i = cp.point_idx;
            size_t j = prev_idx_modulo(i, contour);
            while (j != cp.prev_on_contour->point_idx) {
                //const Point &p1 = contour[i];
                const Point &p2 = contour[j];
                if (std::abs(p2.x() - pt.x()) > dist_max_x)
                    break;
                i = j;
                j = prev_idx_modulo(j, contour);
            }
            if (i != cp.point_idx) {
                const Point &p2 = contour[i];
                coord_t      dist_y = p2.y() - pt.y();
                if (first)
                    dist_y = - dist_y;
                if (dist_y > dist_min_y) {
                    arc_len_prev = closed_contour_distance_ccw(contour_param[i], contour_param[cp.point_idx], contour_param.back());
                    if (arc_len_prev < cp.contour_not_taken_length_prev) {
                        extend_prev_idx = i;
                        dist_y_prev     = dist_y;
                    }
                }
            }
        }

        if (extend_prev_idx >= 0 && extend_next_idx >= 0)
            // Which side to move the point?
            dist_y_prev < dist_y_next ? extend_prev_idx : extend_next_idx = -1;

        assert(cp.prev_trimmed == cp.prev_on_contour->next_trimmed);
        assert(cp.next_trimmed == cp.next_on_contour->prev_trimmed);
        Polyline &infill_line = infill[(&cp - graph.map_infill_end_point_to_boundary.data()) / 2];
        if (extend_prev_idx >= 0) {
            if (first)
                infill_line.reverse();
            take_cw_full(infill_line, contour, cp.point_idx, extend_prev_idx);
            if (first)
                infill_line.reverse();
            cp.point_idx = extend_prev_idx;
            if (cp.prev_trimmed)
                cp.contour_not_taken_length_prev -= arc_len_prev;
            else
                cp.contour_not_taken_length_prev = cp.prev_on_contour->contour_not_taken_length_next =
                    closed_contour_distance_ccw(contour_param[cp.prev_on_contour->point_idx], contour_param[cp.point_idx], contour_param.back());
            cp.trim_next(0);
            cp.next_on_contour->prev_trimmed = true;
        } else if (extend_next_idx >= 0) {
            if (first)
                infill_line.reverse();
            take_ccw_full(infill_line, contour, cp.point_idx, extend_next_idx);
            if (first)
                infill_line.reverse();
            cp.point_idx = extend_next_idx;
            cp.trim_prev(0);
            cp.prev_on_contour->next_trimmed = true;
            if (cp.next_trimmed)
                cp.contour_not_taken_length_next -= arc_len_next;
            else
                cp.contour_not_taken_length_next = cp.next_on_contour->contour_not_taken_length_prev =
                    closed_contour_distance_ccw(contour_param[cp.point_idx], contour_param[cp.next_on_contour->point_idx], contour_param.back());
        }
    }
}

// Called by Fill::connect_base_support() as part of the sparse support infill generator.
// Emit contour loops tracing the contour from tbegin to tend inside a band of (left, right).
// The contour is supposed to enter the "forbidden" zone outside of the (left, right) band at tbegin and also at tend.
static inline void emit_loops_in_band(
    // Vertical band, which will trim the contour between tbegin and tend.
    coord_t                      left,
    coord_t                      right,
    // Contour and its parametrization.
    const Points                &contour,
    const std::vector<double>   &contour_params,
    // Span of the parameters of an arch to trim with the vertical band.
    double                       tbegin,
    double                       tend,
    // Minimum arch length to put into polylines_out. Shorter arches are not necessary to support a dense support infill.
    double                       min_length,
    Polylines                   &polylines_out)
{
    assert(left < right);
    assert(contour.size() + 1 == contour_params.size());
    assert(contour.size() >= 3);
#ifndef NDEBUG
    double contour_length = contour_params.back();
    assert(tbegin >= 0 && tbegin < contour_length);
    assert(tend   >= 0 && tend   < contour_length);
    assert(min_length > 0);
#endif // NDEBUG

    // Find iterators of the range of segments, where the first and last segment contains tbegin and tend.
    size_t ibegin, iend;
    {
        auto it_begin = std::lower_bound(contour_params.begin(), contour_params.end(), tbegin);
        auto it_end   = std::lower_bound(contour_params.begin(), contour_params.end(), tend);
        assert(it_begin != contour_params.end());
        assert(it_end   != contour_params.end());
        if (*it_begin != tbegin) {
            assert(it_begin != contour_params.begin());
            -- it_begin;
        }
        ibegin = it_begin - contour_params.begin();
        iend   = it_end   - contour_params.begin();
    }

    if (ibegin == contour.size())
        ibegin = 0;
    if (iend == contour.size())
        iend = 0;
    assert(ibegin != iend);

    // Trim the start and end segment to calculate start and end points.
    Point pbegin, pend;
    {
        double t1 = contour_params[ibegin];
        double t2 = next_value_modulo(ibegin, contour_params);
        pbegin = lerp(contour[ibegin], next_value_modulo(ibegin, contour), (tbegin - t1) / (t2 - t1));
        t1 = contour_params[iend];
        t2 = prev_value_modulo(iend, contour_params);
        pend = lerp(contour[iend], prev_value_modulo(iend, contour), (tend - t1) / (t2 - t1));
    }

    // Trace the contour from ibegin to iend.
    enum Side {
        Left,
        Right,
        Mid,
        Unknown
    };

    enum InOutBand {
        Entering,
        Leaving,
    };

    class State {
    public:
        State(coord_t left, coord_t right, double min_length, Polylines &polylines_out) :
            m_left(left), m_right(right), m_min_length(min_length), m_polylines_out(polylines_out) {}

        void add_inner_point(const Point* p)
        {
            m_polyline.points.emplace_back(*p);
        }

        void add_outer_point(const Point* p)
        {
            if (m_polyline_end > 0)
                m_polyline.points.emplace_back(*p);
        }

        void add_interpolated_point(const Point* p1, const Point* p2, Side side, InOutBand inout)
        {
            assert(side == Left || side == Right);

            coord_t x = side == Left ? m_left : m_right;
            coord_t y = p1->y() + coord_t(double(x - p1->x()) * double(p2->y() - p1->y()) / double(p2->x() - p1->x()));

            if (inout == Leaving) {
                assert(m_polyline_end == 0);
                m_polyline_end = m_polyline.size();
                m_polyline.points.emplace_back(x, y);
            } else {
                assert(inout == Entering);
                if (m_polyline_end > 0) {
                    if ((this->side1 == Left) == (y - m_polyline.points[m_polyline_end].y() < 0)) {
                        // Emit the vertical segment. Remove the point, where the source contour was split the last time at m_left / m_right.
                        m_polyline.points.erase(m_polyline.points.begin() + m_polyline_end);
                    } else {
                        // Don't emit the vertical segment, split the contour.
                        this->finalize();
                        m_polyline.points.emplace_back(x, y);
                    }
                    m_polyline_end = 0;
                } else
                    m_polyline.points.emplace_back(x, y);
            }
        };

        void finalize()
        {
            m_polyline.points.erase(m_polyline.points.begin() + m_polyline_end, m_polyline.points.end());
            if (! m_polyline.empty()) {
                if (! m_polylines_out.empty() && (m_polylines_out.back().points.back() - m_polyline.points.front()).cast<int64_t>().squaredNorm() < SCALED_EPSILON)
                    m_polylines_out.back().points.insert(m_polylines_out.back().points.end(), m_polyline.points.begin() + 1, m_polyline.points.end());
                else if (m_polyline.length() > m_min_length)
                    m_polylines_out.emplace_back(std::move(m_polyline));
                m_polyline.clear();
            }
        };

    private:
        coord_t      m_left;
        coord_t      m_right;
        double       m_min_length;
        Polylines   &m_polylines_out;

        Polyline     m_polyline;
        size_t       m_polyline_end { 0 };
        Polyline     m_overlapping;

    public:
        Side         side1 { Unknown };
        Side         side2 { Unknown };
    };

    State state { left, right, min_length, polylines_out };

    const Point *p1 = &pbegin;
    auto side = [left, right](const Point* p) {
        coord_t x = p->x();
        return x < left ? Left : x > right ? Right : Mid;
    };
    state.side1 = side(p1);
    if (state.side1 == Mid)
        state.add_inner_point(p1);

    for (size_t i = ibegin; i != iend; ) {
        size_t inext = i + 1;
        if (inext == contour.size())
            inext = 0;
        const Point *p2 = inext == iend ? &pend : &contour[inext];
        state.side2 = side(p2);
        if (state.side1 == Mid) {
            if (state.side2 == Mid) {
                // Inside the band.
                state.add_inner_point(p2);
            } else {
                // From intisde the band to the outside of the band.
                state.add_interpolated_point(p1, p2, state.side2, Leaving);
                state.add_outer_point(p2);
            }
        } else if (state.side2 == Mid) {
            // From outside the band into the band.
            state.add_interpolated_point(p1, p2, state.side1, Entering);
            state.add_inner_point(p2);
        } else if (state.side1 != state.side2) {
            // Both points outside the band.
            state.add_interpolated_point(p1, p2, state.side1, Entering);
            state.add_interpolated_point(p1, p2, state.side2, Leaving);
        } else {
            // Complete segment is outside.
            assert((state.side1 == Left && state.side2 == Left) || (state.side1 == Right && state.side2 == Right));
            state.add_outer_point(p2);
        }
        state.side1 = state.side2;
        p1 = p2;
        i  = inext;
    }
    state.finalize();
}

#ifdef INFILL_DEBUG_OUTPUT
static void export_partial_infill_to_svg(const std::string &path, const BoundaryInfillGraph &graph, const Polylines &infill, const Polylines &emitted)
{
    Polygons polygons;
    for (const Points &pts : graph.boundary)
        polygons.emplace_back(pts);
    BoundingBox bbox = get_extents(polygons);
    bbox.merge(get_extents(infill));
    ::Slic3r::SVG svg(path, bbox);
    svg.draw(union_ex(polygons));
    svg.draw(infill, "blue");
    svg.draw(emitted, "darkblue");
    for (const ContourIntersectionPoint &cp : graph.map_infill_end_point_to_boundary)
        svg.draw(graph.point(cp), cp.consumed ? "red" : "green", scaled(0.2));
    for (const ContourIntersectionPoint &cp : graph.map_infill_end_point_to_boundary) {
        assert(cp.next_trimmed == cp.next_on_contour->prev_trimmed);
        assert(cp.prev_trimmed == cp.prev_on_contour->next_trimmed);
        if (cp.contour_not_taken_length_next > SCALED_EPSILON) {
            Polyline pl { graph.point(cp) };
            take_ccw_limited(pl, graph.boundary[cp.contour_idx], graph.boundary_params[cp.contour_idx], cp.point_idx, cp.next_on_contour->point_idx, cp.contour_not_taken_length_next);
            svg.draw(pl, cp.could_take_next() ? "lime" : "magenta", scaled(0.1));
        }
        if (cp.contour_not_taken_length_prev > SCALED_EPSILON) {
            Polyline pl { graph.point(cp) };
            take_cw_limited(pl, graph.boundary[cp.contour_idx], graph.boundary_params[cp.contour_idx], cp.point_idx, cp.prev_on_contour->point_idx, cp.contour_not_taken_length_prev);
            svg.draw(pl, cp.could_take_prev() ? "lime" : "magenta", scaled(0.1));
        }
    }
}
#endif // INFILL_DEBUG_OUTPUT

// To classify perimeter segments connecting infill lines, whether they are required for structural stability of the supports.
struct SupportArcCost
{
    // Connecting one end of an infill line to the other end of the same infill line.
    bool    self_loop { false };
    // Some of the arc touches some infill line.
    bool    open { false };
    // How needed is this arch for support structural stability.
    // Zero means don't take. The higher number, the more likely it is to take the arc.
    double  cost { 0 };
};

static double evaluate_support_arch_cost(const Polyline &pl)
{
    Point front = pl.points.front();
    Point back  = pl.points.back();

    coord_t ymin = front.y();
    coord_t ymax = back.y();
    if (ymin > ymax)
        std::swap(ymin, ymax);

    double dmax = 0;
    // Maximum distance in Y axis out of the (ymin, ymax) band and from the (front, back) line.
    Linef line { front.cast<double>(), back.cast<double>() };
    for (const Point &pt : pl.points)
        dmax = std::max<double>(std::max(dmax, line_alg::distance_to(line, Vec2d(pt.cast<double>()))), std::max(pt.y() - ymax, ymin - pt.y()));
    return dmax;
}

// Costs for prev / next arch of each infill line end point.
static inline std::vector<SupportArcCost> evaluate_support_arches(Polylines &infill, BoundaryInfillGraph &graph, const double spacing, const FillParams &params)
{
    std::vector<SupportArcCost> arches(graph.map_infill_end_point_to_boundary.size() * 2);

    Polyline pl;
    for (ContourIntersectionPoint &cp : graph.map_infill_end_point_to_boundary) {
        // Not a losed loop, such loops should already be consumed.
        assert(cp.next_on_contour != &cp);
        const size_t                    infill_line_idx = &cp - graph.map_infill_end_point_to_boundary.data();
        const bool                      first           = (infill_line_idx & 1) == 0;
        const ContourIntersectionPoint *other_end       = first ? &cp + 1 : &cp - 1;

        SupportArcCost &out_prev = arches[infill_line_idx * 2];
        SupportArcCost &out_next = *(&out_prev + 1);
        out_prev.self_loop = cp.prev_on_contour == other_end;
        out_prev.open      = cp.prev_trimmed;
        out_next.self_loop = cp.next_on_contour == other_end;
        out_next.open      = cp.next_trimmed;

        if (cp.contour_not_taken_length_next > SCALED_EPSILON) {
            pl.clear();
            pl.points.emplace_back(graph.point(cp));
            if (cp.next_trimmed)
                take_ccw_limited(pl, graph.boundary[cp.contour_idx], graph.boundary_params[cp.contour_idx], cp.point_idx, cp.next_on_contour->point_idx, cp.contour_not_taken_length_next);
            else
                take_ccw_full(pl, graph.boundary[cp.contour_idx], cp.point_idx, cp.next_on_contour->point_idx);
            out_next.cost = evaluate_support_arch_cost(pl);
        }

        if (cp.contour_not_taken_length_prev > SCALED_EPSILON) {
            pl.clear();
            pl.points.emplace_back(graph.point(cp));
            if (cp.prev_trimmed)
                take_cw_limited(pl, graph.boundary[cp.contour_idx], graph.boundary_params[cp.contour_idx], cp.point_idx, cp.prev_on_contour->point_idx, cp.contour_not_taken_length_prev);
            else
                take_cw_full(pl, graph.boundary[cp.contour_idx], cp.point_idx, cp.prev_on_contour->point_idx);
            out_prev.cost = evaluate_support_arch_cost(pl);
        }
    }

    return arches;
}

// Both the poly_with_offset and polylines_out are rotated, so the infill lines are strictly vertical.
void Fill::connect_base_support(Polylines &&infill_ordered, const std::vector<const Polygon*> &boundary_src, const BoundingBox &bbox, Polylines &polylines_out, const double spacing, const FillParams &params)
{
//    assert(! infill_ordered.empty());
    assert(params.anchor_length     >= 0.);
    assert(params.anchor_length_max >= 0.01f);
    assert(params.anchor_length_max >= params.anchor_length);

    BoundaryInfillGraph graph = create_boundary_infill_graph(infill_ordered, boundary_src, bbox, spacing);

#ifdef INFILL_DEBUG_OUTPUT
    static int iRun = 0;
    ++ iRun;
    export_partial_infill_to_svg(debug_out_path("connect_base_support-initial-%03d.svg", iRun), graph, infill_ordered, polylines_out);
#endif // INFILL_DEBUG_OUTPUT

    const double        line_half_width = 0.5 * scale_(spacing);
    const double        line_spacing    = scale_(spacing) / params.density;
    const double        min_arch_length = 1.3 * line_spacing;
    const double        trim_length     = line_half_width * 0.3;

// After mark_boundary_segments_touching_infill() marks boundary segments overlapping trimmed infill lines,
// there are possibly some very short boundary segments unmarked, but overlapping the untrimmed infill lines fully.
// Mark those short boundary segments.
    mark_boundary_segments_overlapping_infill(graph, infill_ordered, scale_(spacing));

#ifdef INFILL_DEBUG_OUTPUT
    export_partial_infill_to_svg(debug_out_path("connect_base_support-marked-%03d.svg", iRun), graph, infill_ordered, polylines_out);
#endif // INFILL_DEBUG_OUTPUT

    // Detect loops with zero infill end points connected.
    // Extrude these loops as perimeters.
    {
        std::vector<size_t> num_boundary_contour_infill_points(graph.boundary.size(), 0);
        for (ContourIntersectionPoint &cp : graph.map_infill_end_point_to_boundary)
            ++ num_boundary_contour_infill_points[cp.contour_idx];
        for (size_t i = 0; i < num_boundary_contour_infill_points.size(); ++ i)
            if (num_boundary_contour_infill_points[i] == 0 && graph.boundary_params[i].back() > trim_length + 0.5 * line_spacing) {
                // Emit a perimeter.
                Polyline pl(graph.boundary[i]);
                pl.points.emplace_back(pl.points.front());
                pl.clip_end(trim_length);
                if (pl.size() > 1)
                    polylines_out.emplace_back(std::move(pl));
            }
    }

    // Before processing the boundary arches, emit those arches, which were trimmed by the infill lines at both sides, but which
    // depart from the infill line at least once after touching the infill line.
    for (ContourIntersectionPoint &cp : graph.map_infill_end_point_to_boundary) {
        if (cp.next_on_contour && cp.next_trimmed && cp.next_on_contour->prev_trimmed) {
            // The arch is leaving one infill line to end up at the same infill line or at the neighbouring one.
            // The arch is touching one of those infill lines at least once.
            // Trace those arches and emit their parts, which are not attached to the end points and they are not overlapping with the two infill lines mentioned.
            bool    first    = graph.first(cp);
            coord_t left     = graph.point(cp).x();
            coord_t right    = left;
            if (first) {
                left  += line_half_width;
                right += line_spacing - line_half_width;
            } else {
                left  -= line_spacing - line_half_width;
                right -= line_half_width;
            }
            double param_start    = cp.param + cp.contour_not_taken_length_next;
            double param_end      = cp.next_on_contour->param - cp.next_on_contour->contour_not_taken_length_prev;
            double contour_length = graph.boundary_params[cp.contour_idx].back();
            if (param_start >= contour_length)
                param_start -= contour_length;
            if (param_end < 0)
                param_end += contour_length;
            // Verify that the interval (param_overlap1, param_overlap2) is inside the interval (ip_low->param, ip_high->param).
            assert(cyclic_interval_inside_interval(cp.param, cp.next_on_contour->param, param_start, param_end, contour_length));
            emit_loops_in_band(left, right, graph.boundary[cp.contour_idx], graph.boundary_params[cp.contour_idx], param_start, param_end, 0.5 * line_spacing, polylines_out);
        }
    }
#ifdef INFILL_DEBUG_OUTPUT
    export_partial_infill_to_svg(debug_out_path("connect_base_support-excess-%03d.svg", iRun), graph, infill_ordered, polylines_out);
#endif // INFILL_DEBUG_OUTPUT

    base_support_extend_infill_lines(infill_ordered, graph, spacing, params);

#ifdef INFILL_DEBUG_OUTPUT
    export_partial_infill_to_svg(debug_out_path("connect_base_support-extended-%03d.svg", iRun), graph, infill_ordered, polylines_out);
#endif // INFILL_DEBUG_OUTPUT

    std::vector<size_t> merged_with(infill_ordered.size());
    std::iota(merged_with.begin(), merged_with.end(), 0);
    auto get_and_update_merged_with = [&graph, &merged_with](const ContourIntersectionPoint *cp) -> size_t {
        size_t polyline_idx = (cp - graph.map_infill_end_point_to_boundary.data()) / 2;
        for (size_t last = polyline_idx;;) {
            size_t lower = merged_with[last];
            assert(lower <= last);
            if (lower == last) {
                merged_with[polyline_idx] = last;
                return last;
            }
            last = lower;
        }
        assert(false);
        return std::numeric_limits<size_t>::max();
    };

    auto vertical = [](BoundaryInfillGraph::Direction dir) {
        return dir == BoundaryInfillGraph::Up || dir == BoundaryInfillGraph::Down;
    };
    // When both left / right arch connected to cp is vertical (ends up at the same vertical infill line), which one to take?
    auto take_vertical_prev = [](const ContourIntersectionPoint &cp) {
        return cp.prev_trimmed == cp.next_trimmed ?
            // Both are either trimmed or not trimmed. Take the longer contour.
            cp.contour_not_taken_length_prev > cp.contour_not_taken_length_next :
            // One is trimmed, the other is not trimmed. Take the not trimmed.
            ! cp.prev_trimmed && cp.next_trimmed;
    };

    // Connect infill lines at cp and cpo_next_on_contour.
    // If the complete arch cannot be taken, then
    // if (take_first)
    //    take the infill line at cp and an arc from cp towards cp.next_on_contour.
    // else
    //    take the infill line at cp_next_on_contour and an arc from cp.next_on_contour towards cp.
    // If cp1 == next_on_contour (a single infill line is connected to a contour, this is a valid case for contours with holes),
    // then extrude the full circle.
    // Nothing is done if the arch could no more be taken (one of it end points were consumed already).
    auto take_next = [&graph, &infill_ordered, &merged_with, get_and_update_merged_with, line_half_width, trim_length](ContourIntersectionPoint &cp, bool take_first) {
        // Indices of the polylines to be connected by a perimeter segment.
        ContourIntersectionPoint  *cp1            = &cp;
        ContourIntersectionPoint  *cp2            = cp.next_on_contour;
        assert(cp1->next_trimmed == cp2->prev_trimmed);
        //assert(cp1->next_trimmed || cp1->consumed == cp2->consumed);
        if (take_first ? cp1->consumed : cp2->consumed)
            return;
        size_t                     polyline_idx1  = get_and_update_merged_with(cp1);
        size_t                     polyline_idx2  = get_and_update_merged_with(cp2);
        Polyline                  &polyline1      = infill_ordered[polyline_idx1];
        Polyline                  &polyline2      = infill_ordered[polyline_idx2];
        const Points              &contour        = graph.boundary[cp1->contour_idx];
        const std::vector<double> &contour_params = graph.boundary_params[cp1->contour_idx];
        assert(cp1->consumed || contour[cp1->point_idx] == polyline1.points.front() || contour[cp1->point_idx] == polyline1.points.back());
        assert(cp2->consumed || contour[cp2->point_idx] == polyline2.points.front() || contour[cp2->point_idx] == polyline2.points.back());
        bool trimmed = take_first ? cp1->next_trimmed : cp2->prev_trimmed;
        if (! trimmed) {
            // Trim the end if closing a loop or making a T-joint.
            trimmed = cp1 == cp2 || polyline_idx1 == polyline_idx2 || (take_first ? cp2->consumed : cp1->consumed);
            if (! trimmed) {
                const bool                      cp1_first = ((cp1 - graph.map_infill_end_point_to_boundary.data()) & 1) == 0;
                const ContourIntersectionPoint* cp1_other = cp1_first ? cp1 + 1 : cp1 - 1;
                // Self loop, connecting the end points of the same infill line.
                trimmed = cp2 == cp1_other;
            }
            if (trimmed) /* [[unlikely]] */ {
                // Single end point on a contour. This may happen on contours with holes. Extrude a loop.
                // Or a self loop, connecting the end points of the same infill line.
                // Or closing a chain of infill lines. This may happen if infilling a contour with a hole.
                double len = cp1 == cp2 ? contour_params.back() : path_length_along_contour_ccw(cp1, cp2, contour_params.back());
                if (take_first) {
                    cp1->trim_next(std::max(0., len - trim_length - SCALED_EPSILON));
                    cp2->trim_prev(0);
                } else {
                    cp1->trim_next(0);
                    cp2->trim_prev(std::max(0., len - trim_length - SCALED_EPSILON));
                }
            }
        }
        if (trimmed) {
            if (take_first)
                take_limited(polyline1, contour, contour_params, cp1, cp2, false, 1e10, line_half_width);
            else
                take_limited(polyline2, contour, contour_params, cp2, cp1, true, 1e10, line_half_width);
        } else if (! cp1->consumed && ! cp2->consumed) {
            if (contour[cp1->point_idx] == polyline1.points.front())
                polyline1.reverse();
            if (contour[cp2->point_idx] == polyline2.points.back())
                polyline2.reverse();
            take(polyline1, polyline2, contour, cp1, cp2, false);
            // Mark the second polygon as merged with the first one.
            if (polyline_idx2 < polyline_idx1) {
                polyline2 = std::move(polyline1);
                polyline1.points.clear();
                merged_with[polyline_idx1] = merged_with[polyline_idx2];
            } else {
                polyline2.points.clear();
                merged_with[polyline_idx2] = merged_with[polyline_idx1];
            }
        }
    };

    // Consume all vertical arches. If a vertical arch is touching a neighboring vertical infill line, thus the vertical arch is trimmed,
    // only consume the trimmed part if it is longer than min_arch_length.
    for (ContourIntersectionPoint &cp : graph.map_infill_end_point_to_boundary) {
        assert(cp.contour_idx != boundary_idx_unconnected);
        if (cp.consumed)
            continue;
        const ContourIntersectionPoint &cp_other = graph.other(cp);
        assert((cp.next_on_contour == &cp_other) == (cp_other.prev_on_contour == &cp));
        assert((cp.prev_on_contour == &cp_other) == (cp_other.next_on_contour == &cp));
        BoundaryInfillGraph::Direction dir_prev = graph.dir_prev(cp);
        BoundaryInfillGraph::Direction dir_next = graph.dir_next(cp);
        // Following code will also consume contours with just a single infill line attached. (cp1->next_on_contour == cp1).
        assert((cp.next_on_contour == &cp) == (cp.prev_on_contour == &cp));
        bool can_take_prev = vertical(dir_prev) && ! cp.prev_on_contour->consumed && cp.prev_on_contour != &cp_other;
        bool can_take_next = vertical(dir_next) && ! cp.next_on_contour->consumed && cp.next_on_contour != &cp_other;
        if (can_take_prev && (! can_take_next || take_vertical_prev(cp))) {
            if (! cp.prev_trimmed || cp.contour_not_taken_length_prev > min_arch_length)
                // take previous
                take_next(*cp.prev_on_contour, false);
        } else if (can_take_next) {
            if (! cp.next_trimmed || cp.contour_not_taken_length_next > min_arch_length)
                // take next
                take_next(cp, true);
        }
    }

#ifdef INFILL_DEBUG_OUTPUT
    export_partial_infill_to_svg(debug_out_path("connect_base_support-vertical-%03d.svg", iRun), graph, infill_ordered, polylines_out);
#endif // INFILL_DEBUG_OUTPUT

    const std::vector<SupportArcCost> arches = evaluate_support_arches(infill_ordered, graph, spacing, params);
    static const double cost_low      = line_spacing * 1.3;
    static const double cost_high     = line_spacing * 2.;
    static const double cost_veryhigh = line_spacing * 3.;

    {
        std::vector<const SupportArcCost*> selected;
        selected.reserve(graph.map_infill_end_point_to_boundary.size());
        for (ContourIntersectionPoint &cp : graph.map_infill_end_point_to_boundary) {
            if (cp.consumed)
                continue;
            const SupportArcCost &cost_prev = arches[(&cp - graph.map_infill_end_point_to_boundary.data()) * 2];
            const SupportArcCost &cost_next = *(&cost_prev + 1);
            double                cost_min = cost_prev.cost;
            double                cost_max = cost_next.cost;
            if (cost_min > cost_max)
                std::swap(cost_min, cost_max);
            if (cost_max < cost_low || cost_min > cost_high)
                // Don't take any of the prev / next arches now, take zig-zag instead. It does not matter which one will be taken.
                continue;
            const double           cost_diff_relative = (cost_max - cost_min) / cost_max;
            if (cost_diff_relative < 0.25)
                // Don't take any of the prev / next arches now, take zig-zag instead. It does not matter which one will be taken.
                continue;
            if (cost_prev.cost > cost_low)
                selected.emplace_back(&cost_prev);
            if (cost_next.cost > cost_low)
                selected.emplace_back(&cost_next);
        }
        // Take the longest arch first.
        std::sort(selected.begin(), selected.end(), [](const auto *l, const auto *r) { return l->cost > r->cost; });
        // And connect along the arches.
        for (const SupportArcCost *arc : selected) {
            ContourIntersectionPoint &cp = graph.map_infill_end_point_to_boundary[(arc - arches.data()) / 2];
            if (! cp.consumed) {
                bool prev = ((arc - arches.data()) & 1) == 0;
                if (prev)
                    take_next(*cp.prev_on_contour, false);
                else
                    take_next(cp, true);
            }
        }
    }

#if 0
    {
        // Connect infill lines with long horizontal arches. Only take a horizontal arch, if it will not block
        // the end caps (vertical arches) at the other side of the infill line.
        struct Arc {
            ContourIntersectionPoint    *intersection;
            double                       arc_length;
            bool                         take_next;
        };
        std::vector<Arc> arches;
        arches.reserve(graph.map_infill_end_point_to_boundary.size());
        for (ContourIntersectionPoint &cp : graph.map_infill_end_point_to_boundary) {
            if (cp.consumed)
                continue;
            // Not a losed loop, such loops should already be consumed.
            assert(cp.next_on_contour != &cp);
            const bool                      first     = ((&cp - graph.map_infill_end_point_to_boundary.data()) & 1) == 0;
            const ContourIntersectionPoint *other_end = first ? &cp + 1 : &cp - 1;
            const bool                      loop_next = cp.next_on_contour == other_end;
            if (! loop_next && cp.could_connect_next()) {
                if (cp.contour_not_taken_length_next > min_arch_length) {
                    // Try both directions. This is useful to be able to close a loop back to the same line to take a long arch.
                    arches.push_back({ &cp, cp.contour_not_taken_length_next, true });
                    arches.push_back({ cp.next_on_contour, cp.contour_not_taken_length_next, false });
                }
            } else {
                //bool    first     = ((&cp - graph.map_infill_end_point_to_boundary) & 1) == 0;
                if (cp.prev_trimmed && cp.could_take_prev()) {
                    //FIXME trace the trimmed line to decide what priority to assign to it.
                    // Is the end point close to the current vertical line or to the other vertical line?
                    const Point &pt   = graph.point(cp);
                    const Point &prev = graph.point(*cp.prev_on_contour);
                    if (std::abs(pt.x() - prev.x()) < coord_t(0.5 * line_spacing)) {
                        // End point on the same line.
                        // Measure maximum distance from the current vertical line.
                        if (cp.contour_not_taken_length_prev > 0.5 * line_spacing)
                            arches.push_back({ &cp, cp.contour_not_taken_length_prev, false });
                    } else {
                        // End point on the other line.
                        if (cp.contour_not_taken_length_prev > min_arch_length)
                            arches.push_back({ &cp, cp.contour_not_taken_length_prev, false });
                    }
                }
                if (cp.next_trimmed && cp.could_take_next()) {
                    //FIXME trace the trimmed line to decide what priority to assign to it.
                    const Point &pt   = graph.point(cp);
                    const Point &next = graph.point(*cp.next_on_contour);
                    if (std::abs(pt.x() - next.x()) < coord_t(0.5 * line_spacing)) {
                        // End point on the same line.
                        // Measure maximum distance from the current vertical line.
                        if (cp.contour_not_taken_length_next > 0.5 * line_spacing)
                            arches.push_back({ &cp, cp.contour_not_taken_length_next, true });
                    } else {
                        // End point on the other line.
                        if (cp.contour_not_taken_length_next > min_arch_length)
                            arches.push_back({ &cp, cp.contour_not_taken_length_next, true });
                    }
                }
            }
        }
        // Take the longest arch first.
        std::sort(arches.begin(), arches.end(), [](const auto &l, const auto &r) { return l.arc_length > r.arc_length; });
        // And connect along the arches.
        for (Arc &arc : arches)
            if (arc.take_next)
                take_next(*arc.intersection, true);
            else
                take_next(*arc.intersection->prev_on_contour, false);
    }
#endif

#ifdef INFILL_DEBUG_OUTPUT
    export_partial_infill_to_svg(debug_out_path("connect_base_support-arches-%03d.svg", iRun), graph, infill_ordered, polylines_out);
#endif // INFILL_DEBUG_OUTPUT

    // Traverse the unconnected lines in a zig-zag fashion, left to right only.
    for (ContourIntersectionPoint &cp : graph.map_infill_end_point_to_boundary) {
        assert(cp.contour_idx != boundary_idx_unconnected);
        if (cp.consumed)
            continue;
        bool first = ((&cp - graph.map_infill_end_point_to_boundary.data()) & 1) == 0;
        if (first) {
            // Only connect if the two lines are not connected by the same line already.
            if (get_and_update_merged_with(&cp) != get_and_update_merged_with(cp.next_on_contour))
                take_next(cp, true);
        } else {
            if (get_and_update_merged_with(&cp) != get_and_update_merged_with(cp.prev_on_contour))
                take_next(*cp.prev_on_contour, false);
        }
    }

#ifdef INFILL_DEBUG_OUTPUT
    export_partial_infill_to_svg(debug_out_path("connect_base_support-zigzag-%03d.svg", iRun), graph, infill_ordered, polylines_out);
#endif // INFILL_DEBUG_OUTPUT

    // Add the left caps.
    for (ContourIntersectionPoint &cp : graph.map_infill_end_point_to_boundary) {
        const bool                      first = ((&cp - graph.map_infill_end_point_to_boundary.data()) & 1) == 0;
        const ContourIntersectionPoint *other_end = first ? &cp + 1 : &cp - 1;
        const bool                      loop_next = cp.next_on_contour == other_end;
        const bool                      loop_prev = other_end->next_on_contour == &cp;
#ifndef NDEBUG
        const SupportArcCost           &cost_prev = arches[(&cp - graph.map_infill_end_point_to_boundary.data()) * 2];
        const SupportArcCost           &cost_next = *(&cost_prev + 1);
        assert(cost_prev.self_loop == loop_prev);
        assert(cost_next.self_loop == loop_next);
#endif // NDEBUG
        if (loop_prev && cp.could_take_prev())
            take_next(*cp.prev_on_contour, false);
        if (loop_next && cp.could_take_next())
            take_next(cp, true);
    }

#ifdef INFILL_DEBUG_OUTPUT
    export_partial_infill_to_svg(debug_out_path("connect_base_support-caps-%03d.svg", iRun), graph, infill_ordered, polylines_out);
#endif // INFILL_DEBUG_OUTPUT

    // Connect with T joints using long arches. Loops could be created only if a very long arc has to be added.
    {
        std::vector<const SupportArcCost*> candidates;
        for (ContourIntersectionPoint &cp : graph.map_infill_end_point_to_boundary) {
            if (cp.could_take_prev())
                candidates.emplace_back(&arches[(&cp - graph.map_infill_end_point_to_boundary.data()) * 2]);
            if (cp.could_take_next())
                candidates.emplace_back(&arches[(&cp - graph.map_infill_end_point_to_boundary.data()) * 2 + 1]);
        }
        std::sort(candidates.begin(), candidates.end(), [](auto *c1, auto *c2) { return c1->cost > c2->cost; });
        for (const SupportArcCost *candidate : candidates) {
            ContourIntersectionPoint &cp   = graph.map_infill_end_point_to_boundary[(candidate - arches.data()) / 2];
            bool                      prev = ((candidate - arches.data()) & 1) == 0;
            if (prev) {
                if (cp.could_take_prev() && (get_and_update_merged_with(&cp) != get_and_update_merged_with(cp.prev_on_contour) || candidate->cost > cost_high))
                    take_next(*cp.prev_on_contour, false);
            } else {
                if (cp.could_take_next() && (get_and_update_merged_with(&cp) != get_and_update_merged_with(cp.next_on_contour) || candidate->cost > cost_high))
                    take_next(cp, true);
            }
        }
    }

#ifdef INFILL_DEBUG_OUTPUT
    export_partial_infill_to_svg(debug_out_path("connect_base_support-Tjoints-%03d.svg", iRun), graph, infill_ordered, polylines_out);
#endif // INFILL_DEBUG_OUTPUT

    // Add very long arches and reasonably long caps even if both of its end points were already consumed.
    const double cap_cost = 0.5 * line_spacing;
    for (ContourIntersectionPoint &cp : graph.map_infill_end_point_to_boundary) {
        const SupportArcCost &cost_prev = arches[(&cp - graph.map_infill_end_point_to_boundary.data()) * 2];
        const SupportArcCost &cost_next = *(&cost_prev + 1);
        if (cp.contour_not_taken_length_prev > SCALED_EPSILON &&
            (cost_prev.self_loop ?
                cost_prev.cost > cap_cost :
                cost_prev.cost > cost_veryhigh)) {
            assert(cp.consumed && (cp.prev_on_contour->consumed || cp.prev_trimmed));
            Polyline pl { graph.point(cp) };
            if (! cp.prev_trimmed) {
                cp.trim_prev(cp.contour_not_taken_length_prev - line_half_width);
                cp.prev_on_contour->trim_next(0);
            }
            if (cp.contour_not_taken_length_prev > SCALED_EPSILON) {
                take_cw_limited(pl, graph.boundary[cp.contour_idx], graph.boundary_params[cp.contour_idx], cp.point_idx, cp.prev_on_contour->point_idx, cp.contour_not_taken_length_prev);
                cp.trim_prev(0);
                pl.clip_start(line_half_width);
                polylines_out.emplace_back(std::move(pl));
            }
        }
        if (cp.contour_not_taken_length_next > SCALED_EPSILON &&
            (cost_next.self_loop ?
                cost_next.cost > cap_cost :
                cost_next.cost > cost_veryhigh)) {
            assert(cp.consumed && (cp.next_on_contour->consumed || cp.next_trimmed));
            Polyline pl { graph.point(cp) };
            if (! cp.next_trimmed) {
                cp.trim_next(cp.contour_not_taken_length_next - line_half_width);
                cp.next_on_contour->trim_prev(0);
            }
            if (cp.contour_not_taken_length_next > SCALED_EPSILON) {
                take_ccw_limited(pl, graph.boundary[cp.contour_idx], graph.boundary_params[cp.contour_idx], cp.point_idx, cp.next_on_contour->point_idx, cp.contour_not_taken_length_next); // line_half_width);
                cp.trim_next(0);
                pl.clip_start(line_half_width);
                polylines_out.emplace_back(std::move(pl));
            }
        }
    }

#ifdef INFILL_DEBUG_OUTPUT
    export_partial_infill_to_svg(debug_out_path("connect_base_support-final-%03d.svg", iRun), graph, infill_ordered, polylines_out);
#endif // INFILL_DEBUG_OUTPUT

    polylines_out.reserve(polylines_out.size() + std::count_if(infill_ordered.begin(), infill_ordered.end(), [](const Polyline &pl) { return ! pl.empty(); }));
    for (Polyline &pl : infill_ordered)
        if (! pl.empty())
            polylines_out.emplace_back(std::move(pl));
}

void Fill::connect_base_support(Polylines &&infill_ordered, const Polygons &boundary_src, const BoundingBox &bbox, Polylines &polylines_out, const double spacing, const FillParams &params)
{
    auto polygons_src = reserve_vector<const Polygon*>(boundary_src.size());
    for (const Polygon &polygon : boundary_src)
        polygons_src.emplace_back(&polygon);

    connect_base_support(std::move(infill_ordered), polygons_src, bbox, polylines_out, spacing, params);
}

// Fill Multiline -Clipper2 version
void multiline_fill(Polylines& polylines, const FillParams& params, float spacing)
{
    if (params.multiline <= 1)
        return;

    const int n_lines     = params.multiline;
    const int n_polylines = static_cast<int>(polylines.size());
    Polylines all_polylines;
    all_polylines.reserve(n_lines * n_polylines);

    BOOST_LOG_TRIVIAL(debug) << "[multiline_fill] pattern=" << int(params.pattern)
                             << " multiline=" << n_lines
                             << " spacing=" << spacing
                             << " density=" << params.density
                             << " input_polylines=" << n_polylines;

    // Remove invalid polylines
    polylines.erase(std::remove_if(polylines.begin(), polylines.end(),
                              [](const Polyline& p) { return p.size() < 2; }),
               polylines.end());

    if (polylines.empty()) {
        BOOST_LOG_TRIVIAL(debug) << "[multiline_fill] no valid input polylines, skipping";
        return;
    }
    // Convert source polylines to Clipper2 paths
    Clipper2Lib::Paths64 subject_paths = Slic3rPolylines_to_Paths64(polylines);

    const double miter_limit = 2.0;
    const int    rings       = n_lines / 2;

    // Compute offsets (in units of spacing)
    std::vector<double> offsets;
    offsets.reserve(n_lines);

    if (n_lines % 2 != 0) {
        // Odd: center line at offset = 0
        offsets.push_back(0.0);

        for (int i = 1; i <= rings; ++i)
            offsets.push_back(i * spacing);
    } else {
        // Even: no center, start at 0.5 * spacing
        double start = 0.5 * spacing;
        for (int i = 0; i < rings; ++i)
            offsets.push_back(start + i * spacing);
    }

    // Process each offset
    Clipper2Lib::ClipperOffset offsetter(miter_limit);
    offsetter.AddPaths(subject_paths, Clipper2Lib::JoinType::Round, Clipper2Lib::EndType::Round);

    for (double t : offsets) {
        if (t == 0.0) {
            // Center line (only applies when n_lines is odd)
            all_polylines.insert(all_polylines.end(), polylines.begin(), polylines.end());
            continue;
        }

        // ClipperOffset with current offset distance (union is not needed here)
        Clipper2Lib::Paths64 offset_paths;
        offsetter.Execute(scale_(t), offset_paths);
        BOOST_LOG_TRIVIAL(trace) << "[multiline_fill]   offset=" << t
                                 << " -> offset_paths=" << offset_paths.size();
        if (offset_paths.empty())
            continue;

        // Convert back to polylines
        Polylines new_polylines = Paths64_to_polylines(offset_paths);

        for (Polyline& pl : new_polylines) {
            if (pl.points.size() < 3)
                continue;
            if (pl.points.front() != pl.points.back())
                pl.points.push_back(pl.points.front());
            all_polylines.emplace_back(std::move(pl));
        }
    }

    BOOST_LOG_TRIVIAL(debug) << "[multiline_fill] output_polylines=" << all_polylines.size();

    polylines = std::move(all_polylines);
}

} // namespace Slic3r
