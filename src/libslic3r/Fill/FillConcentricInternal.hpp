#ifndef slic3r_FillConcentricInternal_hpp_
#define slic3r_FillConcentricInternal_hpp_

#include "FillBase.hpp"

namespace Slic3r {

class FillConcentricInternal : public Fill
{
public:
    ~FillConcentricInternal() override = default;
    void fill_surface_extrusion(const Surface *surface, const FillParams &params, ExtrusionEntitiesPtr &out) override;
    bool is_self_crossing() override { return false; }

protected:
    Fill* clone() const override { return new FillConcentricInternal(*this); };
    bool no_sort() const override { return true; }

    // Ginger continuous_path: same reason as FillConcentric - under connect_polygons the rings are
    // closed loops with a free seam, so let the G-code router order them by proximity instead of
    // keeping the stored outer-to-inner sequence. Kept atomic (no_sort) the collection is entered
    // at its first point only, and the loops inside lose the free seam that makes exit == entry.
    bool reversible_when_connected() const override { return true; }

    friend class Layer;
};

} // namespace Slic3r

#endif // slic3r_FillConcentricInternal_hpp_
