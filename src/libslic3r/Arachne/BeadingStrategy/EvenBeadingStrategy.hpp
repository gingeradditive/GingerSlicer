#ifndef EVEN_BEADING_STRATEGY_H
#define EVEN_BEADING_STRATEGY_H

#include "BeadingStrategy.hpp"

namespace Slic3r::Arachne
{

/*!
 * Ginger (2026-10-09, poppot top, Davide: "dare piu' margine di spessore per fittare con anelli pari"). Beading for
 * a CONNECTED concentric fill: a closed path crosses every section an even number of times, so an odd bead count
 * leaves an open middle line (a stub or a blob in the top). Here the count is always EVEN, the even count whose
 * variable beads are nearest to the optimal width inside [min, max]; one bead only on a true sliver (two hairpin
 * beads do not fit). All widths are SPACINGS, like the optimal width Arachne works with.
 *
 * The leftover goes to a CENTRAL BAND of `band` beads (all the beads when there are fewer): the outer rings stay at
 * the optimal spacing, i.e. exact offsets of each other, and a change of count moves only the band. Spreading it over
 * every bead (first prototype) moved every ring at every transition: each ring folded out and back (re-extrusion)
 * and the rings stopped being offsets of each other (lens-shaped overlaps).
 * Arachne still steps one bead at a time in its transitions, so an odd count only lives inside a transition region.
 */
class EvenBeadingStrategy : public BeadingStrategy
{
public:
    EvenBeadingStrategy(coord_t optimal_width, coord_t min_width, coord_t min_pair_width, coord_t max_width, coord_t band,
                        coord_t default_transition_length, double transitioning_angle);
    ~EvenBeadingStrategy() override = default;

    Beading compute(coord_t thickness, coord_t bead_count) const override;
    coord_t getOptimalThickness(coord_t bead_count) const override;
    coord_t getTransitionThickness(coord_t lower_bead_count) const override;
    coord_t getOptimalBeadCount(coord_t thickness) const override;
    float   getTransitionAnchorPos(coord_t lower_bead_count) const override;

private:
    coord_t min_width;      // variable bead, n >= 4
    coord_t min_pair_width; // the two beads of n = 2 (a hairpin across a narrow strip)
    coord_t max_width;
    coord_t band;           // even, >= 2
    // thickness at which the count leaves `count` for the next one: 0 -> 1, 1 -> 2, even n -> n + 2
    coord_t boundary(coord_t count) const;
};

} // namespace Slic3r::Arachne
#endif // EVEN_BEADING_STRATEGY_H
