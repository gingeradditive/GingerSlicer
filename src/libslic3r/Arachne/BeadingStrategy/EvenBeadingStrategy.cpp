#include "EvenBeadingStrategy.hpp"

#include <algorithm>
#include <cmath>

namespace Slic3r::Arachne
{

EvenBeadingStrategy::EvenBeadingStrategy(coord_t optimal_width, coord_t min_width, coord_t min_pair_width, coord_t max_width, coord_t band,
                                         coord_t default_transition_length, double transitioning_angle)
    // the parity thresholds of the base class are not used: the transitions come from boundary()
    : BeadingStrategy(optimal_width, 0.5, 0.5, default_transition_length, float(transitioning_angle))
    , min_width(std::min(min_width, optimal_width))
    , min_pair_width(std::min(min_pair_width, std::min(min_width, optimal_width)))
    , max_width(std::max(max_width, optimal_width))
    , band(std::max<coord_t>(2, band - band % 2))
{
    name = "EvenBeadingStrategy";
}

coord_t EvenBeadingStrategy::boundary(coord_t count) const
{
    const double w = double(optimal_width);
    if (count <= 0)
        // no bead at all under half the optimal width, like the default stack (RedistributeBeadingStrategy,
        // minimum_variable_line_ratio 0.5): a lower threshold printed slivers the base never filled
        return optimal_width / 2;
    if (count == 1)
        // two beads (a hairpin) as soon as two hairpin beads fit
        return 2 * min_pair_width;
    const double n = double(count);
    if (count + 2 <= band) {
        // n -> n + 2, both inside the band (every bead variable): equal ratio distance from w, inside the window
        // (n = 2 may go down to the hairpin minimum, n + 2 >= 4 not under min_width)
        double       t  = w * std::sqrt(n * (n + 2.));
        const double lo = (n + 2.) * double(min_width);
        const double hi = n * double(max_width);
        return coord_t(lo <= hi ? std::clamp(t, lo, hi) : hi);
    }
    // n -> n + 2 with n >= band: the band keeps its k beads, the outer beads at w grow by two. Variable width
    // a = (T - (n - k) w) / k for n, b = a - 2w/k for n + 2; equal ratio distance a/w = w/b gives
    // a = w (1/k + sqrt(1/k^2 + 1)), inside the window a <= max, b >= min.
    const double k     = double(band);
    const double fixed = (n - k) * w;
    double       a     = w * (1. / k + std::sqrt(1. / (k * k) + 1.));
    const double lo    = double(min_width) + 2. * w / k;
    const double hi    = double(max_width);
    a = lo <= hi ? std::clamp(a, lo, hi) : hi;
    const double v = k * a;
    return coord_t(fixed + v);
}

coord_t EvenBeadingStrategy::getOptimalBeadCount(coord_t thickness) const
{
    if (thickness < boundary(0))
        return 0;
    if (thickness < boundary(1))
        return 1;
    coord_t n = std::max<coord_t>(2, 2 * coord_t(std::llround(double(thickness) / (2. * double(optimal_width)))));
    while (n > 2 && thickness < boundary(n - 2))
        n -= 2;
    while (thickness >= boundary(n))
        n += 2;
    return n;
}

coord_t EvenBeadingStrategy::getTransitionThickness(coord_t lower_bead_count) const
{
    if (lower_bead_count <= 1)
        return boundary(lower_bead_count);
    // n -> n+1 and n+1 -> n+2 sit on the same thickness: the odd count only exists inside the transition region
    return boundary(lower_bead_count % 2 == 0 ? lower_bead_count : lower_bead_count - 1);
}

coord_t EvenBeadingStrategy::getOptimalThickness(coord_t bead_count) const
{
    return optimal_width * bead_count;
}

float EvenBeadingStrategy::getTransitionAnchorPos(coord_t lower_bead_count) const
{
    // The two steps of an even -> even change share one mid. With centred anchors their ends coincided, and at each
    // end Arachne put two nodes on the same point with different counts: the band rings jumped between beadings at
    // zero length (jogs out and back, verification 2026-10-09). Here n -> n+1 spans the transition length BEFORE the
    // mid and n+1 -> n+2 the one AFTER: both meet at the mid on the pure n+1 beading, the rings ramp over 2 lengths.
    if (lower_bead_count < 2)
        return 0.5f;
    return lower_bead_count % 2 == 0 ? 0.999f : 0.001f;
}

BeadingStrategy::Beading EvenBeadingStrategy::compute(coord_t thickness, coord_t bead_count) const
{
    Beading ret;
    ret.total_thickness = thickness;
    if (bead_count <= 0) {
        ret.left_over = thickness;
        return ret;
    }
    // outer beads at the optimal spacing on both sides, the band in the middle takes the rest (an odd count inside a
    // transition gets the odd bead in the band); never let the band go below zero
    coord_t k     = std::min<coord_t>(bead_count, band + bead_count % 2);
    coord_t outer = (bead_count - k) / 2;
    while (outer > 0 && thickness - 2 * outer * optimal_width < k * min_pair_width) {
        -- outer;
        k += 2;
    }
    const coord_t band_thickness = thickness - 2 * outer * optimal_width;
    coord_t accumulated = 0;
    for (coord_t i = 0; i < bead_count; ++ i) {
        const bool    is_outer = i < outer || i >= bead_count - outer;
        const coord_t bi       = i - outer; // index inside the band
        const coord_t width    = i + 1 == bead_count ? thickness - accumulated :
                                 is_outer             ? optimal_width :
                                 bi + 1 == k          ? band_thickness - (k - 1) * (band_thickness / k) :
                                                        band_thickness / k;
        ret.toolpath_locations.emplace_back(i == 0 ? width / 2 : ret.toolpath_locations.back() + (ret.bead_widths.back() + width) / 2);
        ret.bead_widths.emplace_back(width);
        accumulated += width;
    }
    ret.left_over = 0;
    return ret;
}

} // namespace Slic3r::Arachne
