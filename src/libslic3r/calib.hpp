#pragma once
// Ginger: the layer-by-layer Parameter Sweep is the ONLY calibration in GingerSlicer -
// the stock filament-oriented calibrations (PA/temp/flow/VFA towers...) were removed
// together with their pattern generators. The sweep varies one process parameter layer
// by layer on the current plate: ERS parameters are applied by the PressureEqualizer
// post-processor, G-code writer parameters (retraction...) by GCode at each layer change.
#include <string>
#include <algorithm>
#include <cmath>

namespace Slic3r {

enum class CalibMode : int {
    Calib_None = 0,
    Calib_Param_Sweep
};

struct Calib_Params
{
    Calib_Params() : mode(CalibMode::Calib_None) {}
    double      start { 0. }, end { 0. }, step { 0. };
    // Config key of the parameter varied layer by layer.
    std::string sweep_param;
    CalibMode   mode;
};

// Calib_Param_Sweep: value of the swept parameter at a given layer index.
// Starts at `start`, changes by `step` at every layer towards `end`, then holds.
inline double calib_sweep_value_for_layer(const Calib_Params &params, int layer_idx)
{
    const double dir   = (params.end >= params.start) ? 1.0 : -1.0;
    const double value = params.start + dir * std::abs(params.step) * std::max(0, layer_idx);
    return std::clamp(value, std::min(params.start, params.end), std::max(params.start, params.end));
}

} // namespace Slic3r
