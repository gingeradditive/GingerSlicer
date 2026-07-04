#ifndef slic3r_GCode_PressureEqualizer_hpp_
#define slic3r_GCode_PressureEqualizer_hpp_

#include "../libslic3r.h"
#include "../PrintConfig.hpp"

#include <queue>

namespace Slic3r {

struct LayerResult;
struct Calib_Params;

class GCodeG1Formatter;

//#define PRESSURE_EQUALIZER_STATISTIC
//#define PRESSURE_EQUALIZER_DEBUG

// Processes a G-code. Finds changes in the volumetric extrusion speed and adjusts the transitions
// between these paths to limit fast changes in the volumetric extrusion speed.
class PressureEqualizer
{
public:
    PressureEqualizer() = delete;
    explicit PressureEqualizer(const Slic3r::GCodeConfig &config, const Calib_Params *calib_params = nullptr);
    ~PressureEqualizer() = default;

    // Process a next batch of G-code lines.
    // The last LayerResult must be LayerResult::make_nop_layer_result() because it always returns GCode for the previous layer.
    // When process_layer is called for the first layer, then LayerResult::make_nop_layer_result() is returned.
    LayerResult process_layer(LayerResult &&input);
private:

    void process_layer(const std::string &gcode);

#ifdef PRESSURE_EQUALIZER_STATISTIC
    struct Statistics
    {
        void reset()
        {
            volumetric_extrusion_rate_min = std::numeric_limits<float>::max();
            volumetric_extrusion_rate_max = 0.f;
            volumetric_extrusion_rate_avg = 0.f;
            extrusion_length              = 0.f;
        }
        void update(float volumetric_extrusion_rate, float length)
        {
            volumetric_extrusion_rate_min  = std::min(volumetric_extrusion_rate_min, volumetric_extrusion_rate);
            volumetric_extrusion_rate_max  = std::max(volumetric_extrusion_rate_max, volumetric_extrusion_rate);
            volumetric_extrusion_rate_avg += volumetric_extrusion_rate * length;
            extrusion_length              += length;
        }
        float volumetric_extrusion_rate_min;
        float volumetric_extrusion_rate_max;
        float volumetric_extrusion_rate_avg;
        float extrusion_length;
    };

    struct Statistics m_stat;
#endif

    // Private configuration values
    // How fast could the volumetric extrusion rate increase / decrase? mm^3/sec^2
    struct ExtrusionRateSlope {
        float positive;
        float negative;
    };
    ExtrusionRateSlope              m_max_volumetric_extrusion_rate_slopes[size_t(ExtrusionRole::erCount)];
    // Effective slopes (mm³/min²) actually used by the passes: derived from the raw
    // configured values by apply_effective_slopes() (deceleration substitution on the
    // negative side and ramp-profile peak correction, both pellet mode only).
    float                           m_max_volumetric_extrusion_rate_slope_positive;
    float                           m_max_volumetric_extrusion_rate_slope_negative;
    // Raw configured slopes (mm³/min²), kept for the sweep and for re-deriving the
    // effective values when a swept parameter changes.
    float                           m_slope_positive_raw { 0.f };
    float                           m_slope_negative_raw { 0.f };
    // Derive the effective slopes + per-role table from the raw values, the deceleration
    // slope and the ramp profile.
    void                            apply_effective_slopes();

    // Configuration extracted from config.
    // Area of the crossestion of each filament. Necessary to calculate the volumetric flow rate.
    std::vector<float>              m_filament_crossections;

    // Internal data.
    // X,Y,Z,E,F
    float                           m_current_pos[5];
    size_t                          m_current_extruder;
    ExtrusionRole     m_current_extrusion_role;
    bool                            m_retracted;
    bool                            m_use_relative_e_distances;

	// Maximum segment length to split a long segment if the initial and the final flow rate differ.
	// Smaller value means a smoother transition between two different flow rates.
    float                           m_max_segment_length;
    
    // Apply ERS only on external perimeters and overhangs
    bool                           m_extrusion_rate_smoothing_external_perimeter_only;

    // Pellet extruder mode: apply ERS across all gaps (travel, retract, discontinuities)
    bool                           m_pellet_ers_mode { false };
    // Travel threshold below which ramp-up/ramp-down is skipped (treated as continuous)
    float                          m_pellet_ers_travel_threshold { 3.0f };
    // Feedrate interpolation curve shape during ramp-up/ramp-down
    PelletERSRampProfile           m_pellet_ers_ramp_profile { PelletERSRampProfile::Sqrt };
    // Separate deceleration slope (mm³/min²). 0 = use main slope for both directions.
    float                          m_pellet_ers_deceleration_slope { 0.f };
    // Minimum volumetric rate at ramp boundaries (mm³/min, converted from mm³/s at init)
    float                          m_pellet_ers_min_rate { 30.f }; // 0.5 mm³/s * 60
    // Flow compensation factors applied to the extruded amount (E) inside ramp zones.
    // The feedrate ramp alone only redistributes the pressure mismatch along the path:
    // during ramp-up part of the material charges the melt reservoir (under-extruded bead),
    // during ramp-down the reservoir discharges into the bead (over-extruded).
    // 1.0 = no compensation. Applied only with relative E distances.
    float                          m_pellet_ers_rampup_flow { 1.f };
    float                          m_pellet_ers_rampdown_flow { 1.f };
    // Time constant τ of the melt pressure reservoir (seconds, 0 = disabled).
    // Primary flow compensation: E_scale = 1 ± τ·slope/Q computed per segment;
    // the rampup/rampdown flow factors act as an additional trim on top of it.
    float                          m_pellet_ers_pressure_tau { 0.f };

    // Parameter sweep (CalibMode::Calib_Param_Sweep): which ERS parameter is varied
    // layer by layer from sweep_start towards sweep_end, changing by sweep_step at
    // every layer. Values are in user units (mm³/s² for slopes, mm³/s for min rate,
    // 0/1/2 = linear/sqrt/exponential for the ramp profile).
    enum class SweepParam {
        None,         // sweep disabled or handled elsewhere
        Slope,        // max_volumetric_extrusion_rate_slope
        DecelSlope,   // pellet_ers_deceleration_slope
        MinRate,      // pellet_ers_min_rate
        RampProfile,  // pellet_ers_ramp_profile
        RampupFlow,   // pellet_ers_rampup_flow (%)
        RampdownFlow, // pellet_ers_rampdown_flow (%)
        PressureTau,  // pellet_ers_pressure_tau (s)
    };
    SweepParam                     m_sweep_param { SweepParam::None };
    float                          m_sweep_start { 0.f };
    float                          m_sweep_end { 0.f };
    float                          m_sweep_step { 0.f };
    // Index of the next layer to be processed (counts non-nop layers).
    size_t                         m_sweep_layer_idx { 0 };
    // Sweep comment for the last processed layer, emitted when that layer is output.
    std::string                    m_sweep_comment_next;

    // Snapshot of the ERS parameters the sweep can modify. Needed because the output
    // of layer N-1 happens while layer N is being processed: the output pass must run
    // with the parameters that were active when N-1 was processed.
    struct SweepSnapshot {
        float                slope_positive;   // raw, mm³/min²
        float                slope_negative;   // raw, mm³/min²
        float                decel_slope;      // raw, mm³/min²
        float                min_rate;         // mm³/min
        PelletERSRampProfile ramp_profile;
        float                rampup_flow;      // factor, 1.0 = off
        float                rampdown_flow;    // factor, 1.0 = off
        float                pressure_tau;     // seconds, 0 = off
    };
    SweepSnapshot snapshot_sweep_params() const;
    void          restore_sweep_params(const SweepSnapshot &params);
    // Apply the sweep value for m_sweep_layer_idx and compose the G-code comment.
    void          apply_param_sweep();

    // Indicate if extrude set speed block was opened using the tag ";_EXTRUDE_SET_SPEED"
    // or not (not opened, or it was closed using the tag ";_EXTRUDE_END").
    bool                            opened_extrude_set_speed_block = false;

    enum GCodeLineType {
        GCODELINETYPE_INVALID,
        GCODELINETYPE_NOOP,
        GCODELINETYPE_OTHER,
        GCODELINETYPE_RETRACT,
        GCODELINETYPE_UNRETRACT,
        GCODELINETYPE_TOOL_CHANGE,
        GCODELINETYPE_MOVE,
        GCODELINETYPE_EXTRUDE,
    };

    struct GCodeLine
    {
        GCodeLine() : 
            type(GCODELINETYPE_INVALID),
            raw_length(0),
            modified(false),
            extruder_id(0), 
            volumetric_extrusion_rate(0.f), 
            volumetric_extrusion_rate_start(0.f), 
            volumetric_extrusion_rate_end(0.f) 
            {}

        bool        moving_xy()     const { return fabs(pos_end[0] - pos_start[0]) > 0.f || fabs(pos_end[1] - pos_start[1]) > 0.f; }
        bool        moving_z ()     const { return fabs(pos_end[2] - pos_start[2]) > 0.f; }
        bool        extruding()     const { return moving_xy() && pos_end[3] > pos_start[3]; }
        bool        retracting()    const { return pos_end[3] < pos_start[3]; }
        bool        deretracting()  const { return ! moving_xy() && pos_end[3] > pos_start[3]; }

        float       dist_xy2()      const { return (pos_end[0] - pos_start[0]) * (pos_end[0] - pos_start[0]) + (pos_end[1] - pos_start[1]) * (pos_end[1] - pos_start[1]); }
        float       dist_xyz2()     const { return (pos_end[0] - pos_start[0]) * (pos_end[0] - pos_start[0]) + (pos_end[1] - pos_start[1]) * (pos_end[1] - pos_start[1]) + (pos_end[2] - pos_start[2]) * (pos_end[2] - pos_start[2]); }
        float       dist_xy()       const { return sqrt(dist_xy2()); }
        float       dist_xyz()      const { return sqrt(dist_xyz2()); }
        float       dist_e()        const { return fabs(pos_end[3] - pos_start[3]); }

        float       feedrate()      const { return pos_end[4]; }
        float       time()          const { return dist_xyz() / feedrate(); }
        float       time_inv()      const { return feedrate() / dist_xyz(); }
        float       volumetric_correction_avg() const { 
        // Orca: cap the correction to 0.05 - 1.00000001 to avoid zero feedrate
            float avg_correction = std::max(0.05f,0.5f * (volumetric_extrusion_rate_start + volumetric_extrusion_rate_end) / volumetric_extrusion_rate); 
            assert(avg_correction > 0.f);
            assert(avg_correction <= 1.00000001f);
            return avg_correction;
        }
        float       time_corrected()  const { return time() * volumetric_correction_avg(); }

        GCodeLineType type;

        // We try to keep the string buffer once it has been allocated, so it will not be reallocated over and over.
        std::vector<char>   raw;
        size_t              raw_length;
        // If modified, the raw text has to be adapted by the new extrusion rate,
        // or maybe the line needs to be split into multiple lines.
        bool                modified;

        // X,Y,Z,E,F. Storing the state of the currently active extruder only.
        float       pos_start[5];
        float       pos_end[5];
        // Was the axis found on the G-code line? X,Y,Z,E,F
        bool        pos_provided[5];

        // Index of the active extruder.
        size_t      extruder_id;
        // Extrusion role of this segment.
        ExtrusionRole extrusion_role;

        // Current volumetric extrusion rate.
        float       volumetric_extrusion_rate;
        // Volumetric extrusion rate at the start of this segment.
        float       volumetric_extrusion_rate_start;
        // Volumetric extrusion rate at the end of this segment.
        float       volumetric_extrusion_rate_end;

        // Volumetric extrusion rate slope limiting this segment.
        // If set to zero, the slope is unlimited.
        float       max_volumetric_extrusion_rate_slope_positive;
        float       max_volumetric_extrusion_rate_slope_negative;

        bool        adjustable_flow       = false;
        // Set by pellet boundary handler / mini passes to distinguish ramp segments from native ERS.
        bool        pellet_ramp           = false;

        bool        extrude_set_speed_tag = false;
        bool        extrude_end_tag       = false;
        
        // Pellet ERS: travel distance from previous position to this polyline start (mm)
        float       travel_before_polyline = 0.f;
        // Pellet ERS: travel distance from this polyline end to next polyline start (mm)
        float       travel_after_polyline = 0.f;
    };

    // Output buffer will only grow. It will not be reallocated over and over.
    std::vector<char>               output_buffer;
    size_t                          output_buffer_length;
    size_t                          output_buffer_prev_length;

#ifdef PRESSURE_EQUALIZER_DEBUG
    // For debugging purposes. Index of the G-code line processed.
    size_t                          line_idx;
#endif

    bool process_line(const char *line, const char *line_end, GCodeLine &buf);
    long advance_segment_beyond_small_gap(long idx_cur_pos);
    void output_gcode_line(size_t line_idx);

    // Go back from the current circular_buffer_pos and lower the feedtrate to decrease the slope of the extrusion rate changes.
    // Then go forward and adjust the feedrate to decrease the slope of the extrusion rate changes.
    // In pellet mode, is_segment_start/is_segment_end indicate transitions to/from zero flow (travel moves).
    void adjust_volumetric_rate(size_t first_line_idx, size_t last_line_idx, bool is_segment_start = false, bool is_segment_end = false);

    // Push the text to the end of the output_buffer.
    inline void push_to_output(GCodeG1Formatter &formatter);
    inline void push_to_output(const std::string &text, bool add_eol);
    inline void push_to_output(const char *text, size_t len, bool add_eol = true);
    // Push a G-code line to the output.
    void push_line_to_output(size_t line_idx, float new_feedrate, const char *comment, const char *ers_tag = nullptr);

public:
    std::queue<LayerResult*> m_layer_results;

    std::vector<GCodeLine> m_gcode_lines;
};

} // namespace Slic3r

#endif /* slic3r_GCode_PressureEqualizer_hpp_ */
