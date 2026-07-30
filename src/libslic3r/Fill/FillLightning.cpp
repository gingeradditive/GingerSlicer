#include "../ClipperUtils.hpp"
#include "../Print.hpp"
#include "../ShortestPath.hpp"
#include "FillBase.hpp"
#include "FillLightning.hpp"
#include "Lightning/Generator.hpp"

namespace Slic3r::FillLightning {

void Filler::_fill_surface_single(
    const FillParams              &params,
    unsigned int                   thickness_layers,
    const std::pair<float, Point> &direction,
    ExPolygon                      expolygon,
    Polylines                     &polylines_out)
{
    const Layer &layer      = generator->getTreesForLayer(this->layer_id);
    Polylines    fill_lines = layer.convertToLines(to_polygons(expolygon), scaled<coord_t>(0.5 * this->spacing - this->overlap));

    // Apply multiline offset if needed
    multiline_fill(fill_lines, params, spacing);

    if (params.multiline > 1)
        fill_lines = intersection_pl(std::move(fill_lines), expolygon);

    // Ginger single-path: guarantee the wall-hugging "lining" loop. Lightning is demand-driven,
    // so on layers with little demand above only a lone tree survives and the welded walk
    // shrinks to a stub - the inner lining bead (the "second wall") that every other layer has
    // disappears for a band of layers (banding on the inner surface) and the walk loses the
    // rail that carries it to the wall seam (rib). The connector then prefers the contour
    // phase with maximum wall coverage whenever it costs no extra trail.
    FillParams lining_params = params;
    lining_params.sparse_wall_lining = true;
    chain_or_connect_infill(std::move(fill_lines), expolygon, polylines_out, this->spacing, lining_params);
}

void GeneratorDeleter::operator()(Generator *p) {
    delete p;
}

GeneratorPtr build_generator(const PrintObject &print_object, const std::function<void()> &throw_on_cancel_callback)
{
    return GeneratorPtr(new Generator(print_object, throw_on_cancel_callback));
}

} // namespace Slic3r::FillAdaptive
