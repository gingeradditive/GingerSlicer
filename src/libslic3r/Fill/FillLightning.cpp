#include "../ClipperUtils.hpp"
#include "../Print.hpp"
#include "../ShortestPath.hpp"
#include "FillBase.hpp"
#include "FillLightning.hpp"
#include "Lightning/Generator.hpp"

namespace Slic3r::FillLightning {

bool Filler::surface_in_fused_island(const ExPolygon &surface) const
{
    if (fused_islands == nullptr || fused_islands->empty() || surface.contour.points.empty())
        return false;
    // A sparse surface lies strictly inside the wall centerline of its own island (the fill
    // boundary is pulled in by half a spacing, less the wall overlap) and never straddles two
    // islands, so a single boundary point decides.
    const Point &probe = surface.contour.points.front();
    for (const Polygon &wall : *fused_islands)
        if (wall.contains(probe))
            return true;
    return false;
}

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
    //
    // EXCEPT where single_path_infill_as_wall already turned the wall into that ring. There the
    // surface contour is no longer the island outline: the fusion carved a gorge out of it for
    // every branch it took over, so "maximum wall coverage" makes the lining trace the outline of
    // each gorge - a second bead 0.75 spacings from a flank that is itself a wall bead. That is
    // where the fusion's material was going (stool: wall +6.0 m on layer 4 while the fill gave
    // back only 1.2 m). The wall IS the ring here, which is exactly what the option's tooltip
    // promises, so the lining preference is dropped for those islands only.
    const bool fused = this->surface_in_fused_island(expolygon);

    // single_path_infill_ring_always: with no fill line at all the connector has nothing to connect
    // and emits nothing - that is the band of layers with no second wall (stool: 366 layers of 527
    // print no sparse whatsoever, in runs of up to 100). The ring is then laid down by itself, on
    // the very boundary the lining walks when a tree is there, so the two are the same bead in the
    // same place; a lone closed loop needs no connector and costs no travel. Not under the fusion:
    // there the wall already is that ring.
    if (fill_lines.empty()) {
        // Under the fusion the surface only survives when the ring was asked for on every layer,
        // and then its contour already goes around each carved gorge: walking it IS that ring.
        if (params.ring_always) {
            polylines_out.emplace_back(expolygon.contour.split_at_first_point());
            for (const Polygon &hole : expolygon.holes)
                polylines_out.emplace_back(hole.split_at_first_point());
        }
        return;
    }

    FillParams lining_params = params;
    // In a fused island the ring is normally dropped (the wall is the ring), unless the user asks
    // for one on every layer: then it is wanted here too, and it is the connector's job to walk it
    // around the gorges the fusion carved out of the boundary.
    lining_params.sparse_wall_lining = ! fused || params.ring_always;
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
