#ifndef slic3r_FillLightning_hpp_
#define slic3r_FillLightning_hpp_

#include "FillBase.hpp"

namespace Slic3r {

class PrintObject;

namespace FillLightning {

class Generator;
// To keep the definition of Octree opaque, we have to define a custom deleter.
struct GeneratorDeleter { void operator()(Generator *p); };
using  GeneratorPtr = std::unique_ptr<Generator, GeneratorDeleter>;

GeneratorPtr build_generator(const PrintObject &print_object, const std::function<void()> &throw_on_cancel_callback);

class Filler : public Slic3r::Fill
{
public:
    ~Filler() override = default;
    bool is_self_crossing() override { return false; }

    Generator   *generator { nullptr };
    // Ginger single_path_infill_as_wall: the islands of this layer whose wall took over the tree
    // (Layer::wall_fused_islands, owned by the Layer). Inside them the lining is skipped - see
    // _fill_surface_single. nullptr / empty when the fusion is off or fused nothing here.
    const Polygons *fused_islands { nullptr };
protected:
    // True when this sparse surface sits in an island whose wall took over the tree.
    bool surface_in_fused_island(const ExPolygon &surface) const;

    Fill* clone() const override { return new Filler(*this); }

    void _fill_surface_single(const FillParams              &params,
                              unsigned int                   thickness_layers,
                              const std::pair<float, Point> &direction,
                              ExPolygon                      expolygon,
                              Polylines &polylines_out) override;

    // Let the G-code export reoder the infill lines.
	bool no_sort() const override { return false; }
};

} // namespace FillAdaptive
} // namespace Slic3r

#endif // slic3r_FillLightning_hpp_
