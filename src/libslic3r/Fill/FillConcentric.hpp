#ifndef slic3r_FillConcentric_hpp_
#define slic3r_FillConcentric_hpp_

#include "FillBase.hpp"

namespace Slic3r {

class FillConcentric : public Fill
{
public:
    ~FillConcentric() override = default;
    bool is_self_crossing() override { return false; }

protected:
    Fill* clone() const override { return new FillConcentric(*this); };
	void _fill_surface_single(
	    const FillParams                &params, 
	    unsigned int                     thickness_layers,
	    const std::pair<float, Point>   &direction, 
	    ExPolygon     		             expolygon,
	    Polylines                       &polylines_out) override;

	void _fill_surface_single(const FillParams& params,
		unsigned int                   thickness_layers,
		const std::pair<float, Point>& direction,
		ExPolygon                      expolygon,
		ThickPolylines& thick_polylines_out) override;

    bool no_sort() const override { return true; }

    // Ginger single-path: under connect_polygons let the G-code router order the rings by
    // proximity instead of the stored outer-to-inner sequence. The rings are closed loops with a
    // free seam (and the arcs of rings broken by a wavy edge are open paths enterable at either
    // end), so proximity routing chains them at ~one ring spacing; the fixed stored order instead
    // bounced across the surface (measured 15 hops of 20-100mm on one top layer of the real part).
    bool reversible_when_connected() const override { return true; }
};

} // namespace Slic3r

#endif // slic3r_FillConcentric_hpp_
