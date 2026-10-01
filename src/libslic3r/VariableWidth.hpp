#ifndef slic3r_VariableWidth_hpp_
#define slic3r_VariableWidth_hpp_

#include "Polygon.hpp"
#include "ExtrusionEntity.hpp"
#include "Flow.hpp"

namespace Slic3r {
    ExtrusionMultiPath thick_polyline_to_multi_path(const ThickPolyline& thick_polyline, ExtrusionRole role, const Flow& flow, const float tolerance, const float merge_tolerance);
    void variable_width(const ThickPolylines& polylines, ExtrusionRole role, const Flow& flow, std::vector<ExtrusionEntity*>& out);
    // Ginger (2026-10-01): un cordone Arachne = UN ExtrusionPath con larghezza per segmento (ExtrusionPath::widths),
    // invece dei tratti a larghezza costante. width_from_max: larghezza del segmento = massimo dei suoi estremi
    // (la regola dei muri in thick_polyline_to_multi_path), altrimenti la media (quella del riempimento).
    ExtrusionPath thick_polyline_to_variable_width_path(const ThickPolyline& thick_polyline, ExtrusionRole role, const Flow& flow, bool width_from_max);
    // GINGER_ARACHNE_SPLIT=1: torna ai tratti separati (per il confronto A/B).
    bool arachne_split_legacy();
}

#endif
