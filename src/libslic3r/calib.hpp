#pragma once
// Ginger: the layer-by-layer Parameter Sweep is the ONLY calibration in GingerSlicer -
// the stock filament-oriented calibrations (PA/temp/flow/VFA towers...) were removed
// together with their pattern generators. The sweep varies one process parameter layer
// by layer on the current plate: the pellet screw law knobs (restart / coasting scale) by the
// PelletScrewLaw stage through ;_ERS_SWEEP tags, G-code writer parameters (retraction...) by
// GCode at each layer change.
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

// Pellet screw law knobs (filament "Screw law"), swept by the PelletScrewLaw stage: GCode emits
// ";_ERS_SWEEP <key>=<value>" at each layer change (global sweep) or object change (per-object,
// ";_ERS_SWEEP default" restores the filament values), the stage applies it to the runs that follow.
inline bool calib_is_law_param(const std::string &key)
{
    return key == "screw_law_restart_scale" || key == "screw_law_coast_scale";
}

// Parameters applied through the G-code writer config (at each layer change for a
// global sweep, at each object change for per-object sweeps).
inline bool calib_is_writer_param(const std::string &key)
{
    return key == "retraction_length" || key == "retraction_speed" ||
           key == "deretraction_speed" || key == "retract_restart_extra";
}

// Parameters applied to GCode's own config (m_config) with the same cadence as the
// writer ones: the wipe move reads them from there at every retraction. wipe_speed is
// swept as an absolute mm/s value (the profile may express it as % of travel speed).
inline bool calib_is_gcode_param(const std::string &key)
{
    return key == "wipe_distance" || key == "wipe_speed";
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
