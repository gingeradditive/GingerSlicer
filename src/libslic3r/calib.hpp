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
    // step <= 0 means automatic: the step is derived from the layer count of the sweep
    // target (the object for per-object sweeps, the plate for the global one) so the
    // value reaches `end` exactly at the last layer.
    double      start { 0. }, end { 0. }, step { 0. };
    // Config key of the parameter varied layer by layer.
    std::string sweep_param;
    CalibMode   mode;
    // -1 = global sweep (all objects); >= 0 = apply only to the ModelObject with this
    // ObjectID. Several per-object sweeps can be active at once (one per object), so
    // different objects on the same plate can sweep different parameters/ranges.
    int         object_id { -1 };
};

// Step actually used by the sweep: the configured one, or - when step <= 0 (auto) -
// the span divided over the target's layers, reaching `end` exactly at the last one.
inline double calib_sweep_effective_step(const Calib_Params &params, size_t layer_count)
{
    if (params.step > 0.)
        return params.step;
    return std::abs(params.end - params.start) / double(std::max<size_t>(2, layer_count) - 1);
}

// Parameters swept by the PressureEqualizer post-processor (per-object switching is
// done through ;_ERS_SWEEP tags emitted by GCode at each object change).
inline bool calib_is_ers_param(const std::string &key)
{
    return key == "max_volumetric_extrusion_rate_slope" || key == "pellet_ers_deceleration_slope" ||
           key == "pellet_ers_min_rate" || key == "pellet_ers_ramp_profile" ||
           key == "pellet_ers_rampup_flow" || key == "pellet_ers_rampdown_flow" ||
           key == "pellet_ers_pressure_tau";
}

// Parameters applied through the G-code writer config (at each layer change for a
// global sweep, at each object change for per-object sweeps).
inline bool calib_is_writer_param(const std::string &key)
{
    return key == "retraction_length" || key == "retraction_speed" ||
           key == "deretraction_speed" || key == "retract_restart_extra";
}

// Calib_Param_Sweep: value of the swept parameter at a given layer index.
// Starts at `start`, changes by the (possibly automatic, see above) step at every
// layer towards `end`, then holds. `layer_count` is the target's total layer count,
// only used to resolve an automatic step.
inline double calib_sweep_value_for_layer(const Calib_Params &params, int layer_idx, size_t layer_count)
{
    const double step  = calib_sweep_effective_step(params, layer_count);
    const double dir   = (params.end >= params.start) ? 1.0 : -1.0;
    const double value = params.start + dir * std::abs(step) * std::max(0, layer_idx);
    return std::clamp(value, std::min(params.start, params.end), std::max(params.start, params.end));
}

} // namespace Slic3r
