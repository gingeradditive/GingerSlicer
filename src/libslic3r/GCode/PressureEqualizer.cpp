#include <iostream>
#include <memory.h>
#include <cstring>
#include <cfloat>
#include <algorithm>

#include "../libslic3r.h"
#include "../PrintConfig.hpp"
#include "../LocalesUtils.hpp"
#include "../GCode.hpp"

#include "PressureEqualizer.hpp"
#include "fast_float/fast_float.h"
#include "GCodeWriter.hpp"

namespace Slic3r {

static const std::string EXTRUSION_ROLE_TAG = ";_EXTRUSION_ROLE:";
static const std::string EXTRUDE_END_TAG = ";_EXTRUDE_END";
static const std::string EXTRUDE_SET_SPEED_TAG = ";_EXTRUDE_SET_SPEED";
static const std::string EXTERNAL_PERIMETER_TAG = ";_EXTERNAL_PERIMETER";

// For how many GCode lines back will adjust a flow rate from the latest line.
// Bigger values affect the GCode export speed a lot, and smaller values could
// affect how distant will be propagated a flow rate adjustment.
static constexpr int max_look_back_limit = 128;

// Max non-extruding XY distance (travel move) in mm between two continous extrusions where we pretend
// its all one continous extruded line. Above this distance we assume extruder pressure hits 0
// This exists because often there's tiny travel moves between stuff like infill 
// lines where some extruder pressure will remain (so we should equalize between these small travels)
static constexpr long max_ignored_gap_between_extruding_segments = 3;

PressureEqualizer::PressureEqualizer(const Slic3r::GCodeConfig &config) : m_use_relative_e_distances(config.use_relative_e_distances.value)
{
    // Preallocate some data, so that output_buffer.data() will return an empty string.
    output_buffer.assign(32, 0);
    output_buffer_length      = 0;
    output_buffer_prev_length = 0;

    m_current_extruder = 0;
    // Zero the position of the XYZE axes + the current feed
    memset(m_current_pos, 0, sizeof(float) * 5);
    m_current_extrusion_role = ExtrusionRole::erNone;
    // Expect the first command to fill the nozzle (deretract).
    m_retracted = true;
    
    m_max_segment_length = 2.f;

    // Calculate filamet crossections for the multiple extruders.
    m_filament_crossections.clear();
    for (double r : config.filament_diameter.values) {
        double a = 0.25f * M_PI * r * r;
        m_filament_crossections.push_back(float(a));
    }

    // Volumetric rate of a 0.45mm x 0.2mm extrusion at 60mm/s XY movement: 0.45*0.2*60*60=5.4*60 = 324 mm^3/min
    // Volumetric rate of a 0.45mm x 0.2mm extrusion at 20mm/s XY movement: 0.45*0.2*20*60=1.8*60 = 108 mm^3/min
    // Slope of the volumetric rate, changing from 20mm/s to 60mm/s over 2 seconds: (5.4-1.8)*60*60/2=60*60*1.8 = 6480 mm^3/min^2 = 1.8 mm^3/s^2
    
    if(config.max_volumetric_extrusion_rate_slope.value > 0){
		m_max_volumetric_extrusion_rate_slope_positive = float(config.max_volumetric_extrusion_rate_slope.value) * 60.f * 60.f;
    	m_max_volumetric_extrusion_rate_slope_negative = float(config.max_volumetric_extrusion_rate_slope.value) * 60.f * 60.f;
    	m_max_segment_length = float(config.max_volumetric_extrusion_rate_slope_segment_length.value);
        m_extrusion_rate_smoothing_external_perimeter_only = bool(config.extrusion_rate_smoothing_external_perimeter_only.value);
        m_pellet_ers_mode = bool(config.pellet_ers_mode.value);
        m_pellet_ers_travel_threshold = float(config.pellet_ers_travel_threshold_mm.value);
        m_pellet_ers_ramp_profile = config.pellet_ers_ramp_profile.value;
        m_pellet_ers_deceleration_slope = float(config.pellet_ers_deceleration_slope.value) * 60.f * 60.f; // mm³/s² → mm³/min²
        m_pellet_ers_min_rate = float(config.pellet_ers_min_rate.value) * 60.f; // mm³/s → mm³/min
    }

    for (ExtrusionRateSlope &extrusion_rate_slope : m_max_volumetric_extrusion_rate_slopes) {
        extrusion_rate_slope.negative = m_max_volumetric_extrusion_rate_slope_negative;
        extrusion_rate_slope.positive = m_max_volumetric_extrusion_rate_slope_positive;
    }
    
	// Don't regulate the pressure before and after ironing.
    for (const ExtrusionRole er : {ExtrusionRole::erIroning}) {
        m_max_volumetric_extrusion_rate_slopes[size_t(er)].negative = 0;
        m_max_volumetric_extrusion_rate_slopes[size_t(er)].positive = 0;
    }

    opened_extrude_set_speed_block = false;

#ifdef PRESSURE_EQUALIZER_STATISTIC
    m_stat.reset();
#endif

#ifdef PRESSURE_EQUALIZER_DEBUG
    line_idx = 0;
#endif
}

void PressureEqualizer::process_layer(const std::string &gcode)
{
    if (!gcode.empty()) {
        const char *gcode_begin = gcode.c_str();
        while (*gcode_begin != 0) {
            // Find end of the line.
            const char *gcode_end = gcode_begin;
            // Slic3r always generates end of lines in a Unix style.
            for (; *gcode_end != 0 && *gcode_end != '\n'; ++gcode_end);

            m_gcode_lines.emplace_back();
            if (!this->process_line(gcode_begin, gcode_end, m_gcode_lines.back())) {
                // The line has to be forgotten. It contains comment marks, which shall be filtered out of the target g-code.
                m_gcode_lines.pop_back();
            }
            gcode_begin = gcode_end;
            if (*gcode_begin == '\n')
                ++gcode_begin;
        }
        assert(!this->opened_extrude_set_speed_block);
    }
    
    // at this point, we have an entire layer of gcode lines loaded into m_gcode_lines
    // now we will split the mix of travels and extrudes into segments of continous extrusion and process those
    // We skip over large travels, and pretend small ones are part of a continous extrusion segment

    if (m_pellet_ers_mode) {
        // Pellet mode: use POLYLINE_START/END markers to identify continuous extrusion segments
        // These markers are generated by GCode::_extrude() when pellet_ers_mode is enabled
        //
        // TWO-PASS approach:
        //   Pass 1: Collect all segments and assign travel_before/travel_after distances
        //   Pass 2: Process ERS for each segment (ramp-up/down decisions use travel distances)
        // This ensures travel_after is set BEFORE adjust_volumetric_rate reads it.
        struct PolylineSegment {
            size_t seg_start;
            size_t seg_end;
            size_t first_e_idx;
            size_t last_e_idx;
            float  travel_distance;
        };
        std::vector<PolylineSegment> segments;
        // --- Pass 1: Parse all POLYLINE_START/END markers and collect segments ---
        long idx = 0;
        while (idx < (long)m_gcode_lines.size()) {
            // Find next POLYLINE_START marker
            long seg_start = -1;
            float travel_distance = 0.f;
            while (idx < (long)m_gcode_lines.size()) {
                std::string_view line_view(m_gcode_lines[idx].raw.data(), m_gcode_lines[idx].raw.size());
                if (line_view.find(";POLYLINE_START") != std::string_view::npos) {
                    seg_start = idx;
                    // Extract travel_mm from the comment: ";POLYLINE_START id=N travel_mm=X"
                    size_t travel_pos = line_view.find("travel_mm=");
                    if (travel_pos != std::string_view::npos) {
                        std::string_view travel_substr = line_view.substr(travel_pos + 10);
                        size_t space_pos = travel_substr.find(' ');
                        if (space_pos == std::string_view::npos) space_pos = travel_substr.find('\n');
                        if (space_pos == std::string_view::npos) space_pos = travel_substr.size();
                        try {
                            travel_distance = std::stof(std::string(travel_substr.substr(0, space_pos)));
                        } catch (...) {
                            travel_distance = 0.f;
                        }
                    }
                    ++idx;
                    break;
                }
                ++idx;
            }
            if (seg_start < 0 || idx >= (long)m_gcode_lines.size())
                break;
            // Find corresponding POLYLINE_END marker
            long seg_end = -1;
            while (idx < (long)m_gcode_lines.size()) {
                std::string_view line_view(m_gcode_lines[idx].raw.data(), m_gcode_lines[idx].raw.size());
                if (line_view.find(";POLYLINE_END") != std::string_view::npos) {
                    seg_end = idx;
                    ++idx;
                    break;
                }
                ++idx;
            }
            if (seg_end < 0)
                break;
            // Find first and last actual extruding lines within the segment
            size_t first_e_idx = seg_start;
            while (first_e_idx < (size_t)seg_end && !m_gcode_lines[first_e_idx].extruding())
                ++first_e_idx;
            size_t last_e_idx = seg_end;
            while (last_e_idx > (size_t)seg_start && !m_gcode_lines[last_e_idx].extruding())
                --last_e_idx;
            // Safety check: ensure we found valid extruding lines
            if (first_e_idx > (size_t)seg_end || last_e_idx < (size_t)seg_start || first_e_idx > last_e_idx)
                continue;
            segments.push_back({(size_t)seg_start, (size_t)seg_end, first_e_idx, last_e_idx, travel_distance});
        }
        // --- Merge consecutive segments separated by below-threshold travel ---
        // When travel between two polylines is below threshold, they form a continuous
        // extrusion stream.  Merging ensures the ramp-up from the first polyline
        // continues into the next one instead of restarting at steady state.
        {
            std::vector<PolylineSegment> merged;
            for (size_t i = 0; i < segments.size(); ++i) {
                if (merged.empty()) {
                    merged.push_back(segments[i]);
                } else {
                    // The travel *before* this segment equals the gap after the previous one
                    if (segments[i].travel_distance < m_pellet_ers_travel_threshold) {
                        // Merge: extend the current merged segment to cover the new one
                        merged.back().seg_end    = segments[i].seg_end;
                        merged.back().last_e_idx = segments[i].last_e_idx;
                    } else {
                        merged.push_back(segments[i]);
                    }
                }
            }
            segments = std::move(merged);
        }
        // --- Assign travel_before and travel_after to GCodeLines ---
        for (size_t i = 0; i < segments.size(); ++i) {
            auto &seg = segments[i];
            // travel_before: distance from previous position to this segment's start
            m_gcode_lines[seg.first_e_idx].travel_before_polyline = seg.travel_distance;
            // travel_after: the NEXT segment's travel_distance is this segment's travel_after
            if (i + 1 < segments.size()) {
                m_gcode_lines[seg.last_e_idx].travel_after_polyline = segments[i + 1].travel_distance;
            }
            // Last segment: travel_after stays 0 (end of layer, assume full pressure loss)
        }
        // --- Pass 2: Process ERS for each segment ---
        for (const auto &seg : segments) {
            adjust_volumetric_rate(seg.seg_start, seg.seg_end, true, true);
        }
    } else {
        // Standard filament mode: break at large travel gaps (> 3mm)
        long idx_end_current_extrusion = 0;
        while (idx_end_current_extrusion < m_gcode_lines.size()) {
            // find beginning of next extrusion segment from current pos
            const long idx_begin_current_extrusion   = find_if(m_gcode_lines.begin() + idx_end_current_extrusion, m_gcode_lines.end(),
                                                              [](GCodeLine line) { return line.extruding(); }) - m_gcode_lines.begin();
            // (extrusion begin idx = extrusion end idx) here because we start with extrusion length of zero
            idx_end_current_extrusion = idx_begin_current_extrusion;

            // inner loop extends the extrusion segment over small travel moves
            while (idx_end_current_extrusion < m_gcode_lines.size()) {
                // find end of the current extrusion segment
                const auto just_after_end_extrusion = find_if(m_gcode_lines.begin() + idx_end_current_extrusion, m_gcode_lines.end(),
                                                              [](GCodeLine line) { return !line.extruding(); });
                idx_end_current_extrusion = std::max<long>(0,(just_after_end_extrusion - m_gcode_lines.begin()) - 1);
                const long idx_begin_segment_continuation = advance_segment_beyond_small_gap(idx_end_current_extrusion);
                if (idx_begin_segment_continuation > idx_end_current_extrusion) {
                    // extend the continous line over the small gap
                    idx_end_current_extrusion = idx_begin_segment_continuation;
                    continue; // keep going, loop again to find new end of extrusion segment
                } else {
                    // gap to next extrude is too big, stop looking forward. We've found end of this segment
                    break;
                }
            }

            // now run the pressure equalizer across the segment like a streamroller
            // it operates on a sliding window that moves forward across gcode line by line
            for (int i = idx_begin_current_extrusion; i < idx_end_current_extrusion; ++i) {
                // feed pressure equalizer past lines, going back to max_look_back_limit (or start of segment)
                const auto start_idx = std::max<long>(idx_begin_current_extrusion, i - max_look_back_limit);
                adjust_volumetric_rate(start_idx, i);
            }
            // current extrusion is all done processing so advance beyond it for next loop
            idx_end_current_extrusion++;
        }
    }
}

long PressureEqualizer::advance_segment_beyond_small_gap(const long idx_orig)
{
    // this should only be run on the last extruding line before a gap
    assert(m_gcode_lines[idx_orig].extruding());
    double distance_traveled = 0.0;
    // start at beginning of gap, advance till extrusion found or gap too big
    for (auto idx_cur_pos = idx_orig + 1; idx_cur_pos < m_gcode_lines.size(); idx_cur_pos++) {
        // started extruding again! return segment extension
        if (m_gcode_lines[idx_cur_pos].extruding()) {
            return idx_cur_pos;
        }

        distance_traveled += m_gcode_lines[idx_cur_pos].dist_xy();
        // gap too big, dont extend segment
        if (distance_traveled > max_ignored_gap_between_extruding_segments) {
            return idx_orig;
        }
    }
    // looped until end of layer and couldn't extend extrusion
     return idx_orig;
}

LayerResult PressureEqualizer::process_layer(LayerResult &&input)
{
    const bool   is_first_layer       = m_layer_results.empty();
    const size_t next_layer_first_idx = m_gcode_lines.size();

    if (!input.nop_layer_result) {
        this->process_layer(input.gcode);
        input.gcode.clear(); // GCode is already processed, so it isn't needed to store it.
        m_layer_results.emplace(new LayerResult(input));
    }

    if (is_first_layer) // Buffer previous input result and output NOP.
        return LayerResult::make_nop_layer_result();

    // Export previous layer.
    LayerResult *prev_layer_result = m_layer_results.front();
    m_layer_results.pop();

    output_buffer_length      = 0;
    output_buffer_prev_length = 0;
    for (size_t line_idx = 0; line_idx < next_layer_first_idx; ++line_idx)
        output_gcode_line(line_idx);
    m_gcode_lines.erase(m_gcode_lines.begin(), m_gcode_lines.begin() + int(next_layer_first_idx));

    if (output_buffer_length > 0)
        prev_layer_result->gcode = std::string(output_buffer.data());

    assert(!input.nop_layer_result || m_layer_results.empty());
    LayerResult out = *prev_layer_result;
    delete prev_layer_result;
    return out;
}

// Is a white space?
static inline bool is_ws(const char c) { return c == ' ' || c == '\t'; }
// Is it an end of line? Consider a comment to be an end of line as well.
static inline bool is_eol(const char c) { return c == 0 || c == '\r' || c == '\n' || c == ';'; }
// Is it a white space or end of line?
static inline bool is_ws_or_eol(const char c) { return is_ws(c) || is_eol(c); }

// Eat whitespaces.
static void eatws(const char *&line)
{
    while (is_ws(*line)) 
        ++ line;
}

// Parse an int starting at the current position of a line.
// If succeeded, the line pointer is advanced.
static inline int parse_int(const char *&line)
{
    char *endptr = nullptr;
    long result = strtol(line, &endptr, 10);
    if (endptr == nullptr || !is_ws_or_eol(*endptr))
        throw Slic3r::InvalidArgument("PressureEqualizer: Error parsing an int");
    line = endptr;
    return int(result);
}

float string_to_float_decimal_point(const char *line, const size_t str_len, size_t* pos)
{
    float out;
    size_t p = fast_float::from_chars(line, line + str_len, out).ptr - line;
    if (pos)
        *pos = p;
    return out;
}

// Parse an int starting at the current position of a line.
// If succeeded, the line pointer is advanced.
static inline float parse_float(const char *&line, const size_t line_length)
{
    size_t endptr = 0;
    auto   result = string_to_float_decimal_point(line, line_length, &endptr);
    if (endptr == 0 || !is_ws_or_eol(*(line + endptr)))
        throw Slic3r::RuntimeError("PressureEqualizer: Error parsing a float");
    line = line + endptr;
    return result;
}

bool PressureEqualizer::process_line(const char *line, const char *line_end, GCodeLine &buf)
{
    const size_t len = line_end - line;
    if (strncmp(line, EXTRUSION_ROLE_TAG.data(), EXTRUSION_ROLE_TAG.length()) == 0) {
        line += EXTRUSION_ROLE_TAG.length();
        int role = atoi(line);
        m_current_extrusion_role = ExtrusionRole(role);
#ifdef PRESSURE_EQUALIZER_DEBUG
        ++line_idx;
#endif
        return false;
    }

    // Set the type, copy the line to the buffer.
    buf.type = GCODELINETYPE_OTHER;
    buf.modified = false;
    if (buf.raw.size() < len + 1)
        buf.raw.assign(line, line + len + 1);
    else
        memcpy(buf.raw.data(), line, len);
    buf.raw[len] = 0;
    buf.raw_length = len;

    memcpy(buf.pos_start, m_current_pos, sizeof(float)*5);
    memcpy(buf.pos_end, m_current_pos, sizeof(float)*5);
    memset(buf.pos_provided, 0, 5);

    buf.volumetric_extrusion_rate = 0.f;
    buf.volumetric_extrusion_rate_start = 0.f;
    buf.volumetric_extrusion_rate_end = 0.f;
    buf.max_volumetric_extrusion_rate_slope_positive = 0.f;
    buf.max_volumetric_extrusion_rate_slope_negative = 0.f;
	buf.extrusion_role = m_current_extrusion_role;

    std::string str_line(line, line_end);
    const bool found_extrude_set_speed_tag = boost::contains(str_line, EXTRUDE_SET_SPEED_TAG);
    const bool found_extrude_end_tag = boost::contains(str_line, EXTRUDE_END_TAG);
    assert(!found_extrude_set_speed_tag || !found_extrude_end_tag);

    if (found_extrude_set_speed_tag)
        this->opened_extrude_set_speed_block = true;
    else if (found_extrude_end_tag)
        this->opened_extrude_set_speed_block = false;

    // Parse the G-code line, store the result into the buf.
    switch (toupper(*line ++)) {
    case 'G': {
        int gcode = -1;
        try {
            gcode = parse_int(line);
        } catch (Slic3r::InvalidArgument &) {
            // Ignore invalid GCodes.
            eatws(line);
            break;
        }

        assert(gcode != -1);
        eatws(line);
        switch (gcode) {
        case 0:
        case 1:
        {
            // G0, G1: A FFF 3D printer does not make a difference between the two.
            buf.adjustable_flow = this->opened_extrude_set_speed_block;
            buf.extrude_set_speed_tag = found_extrude_set_speed_tag;
            buf.extrude_end_tag = found_extrude_end_tag;
            float new_pos[5];
            memcpy(new_pos, m_current_pos, sizeof(float)*5);
            bool  changed[5] = { false, false, false, false, false };
            while (!is_eol(*line)) {
                const char axis = toupper(*line++);
                int  i = -1;
                switch (axis) {
                case 'X':
                case 'Y':
                case 'Z':
                    i = axis - 'X';
                    break;
                case 'E':
                    i = 3;
                    break;
                case 'F':
                    i = 4;
                    break;
                default:
                    break;
                }
                if (i != -1) {
                    buf.pos_provided[i] = true;
                    new_pos[i] = parse_float(line, line_end - line);
                    if (i == 3 && m_use_relative_e_distances)
                        new_pos[i] += m_current_pos[i];
                    changed[i] = new_pos[i] != m_current_pos[i];
                    eatws(line);
                }
            }
            if (changed[3]) {
                // Extrusion, retract or unretract.
                float diff = new_pos[3] - m_current_pos[3];
                if (diff < 0) {
                    buf.type = GCODELINETYPE_RETRACT;
                    m_retracted = true;
                } else if (! changed[0] && ! changed[1] && ! changed[2]) {
                    // assert(m_retracted);
                    buf.type = GCODELINETYPE_UNRETRACT;
                    m_retracted = false;
                } else {
                    assert(changed[0] || changed[1]);
                    // Moving in XY plane.
                    buf.type = GCODELINETYPE_EXTRUDE;
                    // Calculate the volumetric extrusion rate.
                    float diff[4];
                    for (size_t i = 0; i < 4; ++ i)
                        diff[i] = new_pos[i] - m_current_pos[i];
                    // volumetric extrusion rate = A_filament * F_xyz * L_e / L_xyz [mm^3/min]
                    float len2 = diff[0]*diff[0]+diff[1]*diff[1]+diff[2]*diff[2];
                    float rate = m_filament_crossections[m_current_extruder] * new_pos[4] * sqrt((diff[3]*diff[3])/len2);
                    buf.volumetric_extrusion_rate       = rate;
                    buf.volumetric_extrusion_rate_start = rate;
                    buf.volumetric_extrusion_rate_end   = rate;

#ifdef PRESSURE_EQUALIZER_STATISTIC
                    m_stat.update(rate, sqrt(len2));
#endif
#ifdef PRESSURE_EQUALIZER_DEBUG
                    if (rate < 40.f) {
                        printf("Extremely low flow rate: %f. Line %d, Length: %f, extrusion: %f Old position: (%f, %f, %f), new position: (%f, %f, %f)\n",
                               rate, int(line_idx), sqrt(len2), sqrt((diff[3] * diff[3]) / len2), m_current_pos[0], m_current_pos[1], m_current_pos[2],
                               new_pos[0], new_pos[1], new_pos[2]);
                    }
#endif
                }
            } else if (changed[0] || changed[1] || changed[2]) {
                // Moving without extrusion.
                buf.type = GCODELINETYPE_MOVE;
            }
            memcpy(m_current_pos, new_pos, sizeof(float) * 5);
            break;
        }
        case 92: 
        {
            // G92 : Set Position
            // Set a logical coordinate position to a new value without actually moving the machine motors.
            // Which axes to set?
            while (!is_eol(*line)) {
                const char axis = toupper(*line++);
                switch (axis) {
                case 'X':
                case 'Y':
                case 'Z':
                    m_current_pos[axis - 'X'] = (!is_ws_or_eol(*line)) ? parse_float(line, line_end - line) : 0.f;
                    break;
                case 'E':
                    m_current_pos[3] = (!is_ws_or_eol(*line)) ? parse_float(line, line_end - line) : 0.f;
                    break;
                default:
                    break;
                }
                eatws(line);
            }
            break;
        }
        case 10:
        case 22:
            // Firmware retract.
            buf.type = GCODELINETYPE_RETRACT;
            m_retracted = true;
            break;
        case 11:
        case 23:
            // Firmware unretract.
            buf.type = GCODELINETYPE_UNRETRACT;
            m_retracted = false;
            break;
        default:
            // Ignore the rest.
        break;
        }
        break;
    }
    case 'M': {
        eatws(line);
        // Ignore the rest of the M-codes.
        break;
    }
    case 'T':
    {
        // Activate an extruder head.
        int new_extruder = -1;
        try {
            new_extruder = parse_int(line);
        } catch (Slic3r::InvalidArgument &) {
            // Ignore invalid GCodes starting with T.
            eatws(line);
            break;
        }
        assert(new_extruder != -1);

        if (new_extruder != int(m_current_extruder)) {
            m_current_extruder = new_extruder;
            m_retracted = true;
            buf.type = GCODELINETYPE_TOOL_CHANGE;
        } else {
            buf.type = GCODELINETYPE_NOOP;
        }
        break;
    }
    }

    buf.extruder_id = m_current_extruder;
    memcpy(buf.pos_end, m_current_pos, sizeof(float)*5);
#ifdef PRESSURE_EQUALIZER_DEBUG
    ++line_idx;
#endif
    return true;
}

/// Interpolates feedrate at parametric position t ∈ [0,1] within a ramp zone.
///
/// @param f_start  Feedrate at zone start (mm/min)
/// @param f_end    Feedrate at zone end   (mm/min)
/// @param t        Parametric position within the zone, 0 = start, 1 = end
/// @param profile  Curve shape selector
/// @return         Interpolated feedrate (mm/min)
///
/// Profile shapes (ramp-up, f_start < f_end):
///   Linear:      constant acceleration — f(t) = f_start + (f_end - f_start) * t
///   Sqrt:        kinematic v² = v₀²+2as — fast initial ramp, gentle approach to target
///   Exponential: first-order response   — fastest initial ramp, asymptotic approach (k=3 → ~95% at t=1)
///
/// For ramp-down (f_start > f_end) the same formulas apply; the curve is automatically mirrored.
static float interpolate_ramp(float f_start, float f_end, float t, PelletERSRampProfile profile)
{
    switch (profile) {
    case PelletERSRampProfile::Sqrt:
        return sqrtf(f_start * f_start + (f_end * f_end - f_start * f_start) * t);
    case PelletERSRampProfile::Exponential:
        return f_end - (f_end - f_start) * expf(-3.f * t);
    case PelletERSRampProfile::Linear:
    default:
        return f_start + (f_end - f_start) * t;
    }
}
void PressureEqualizer::output_gcode_line(const size_t line_idx)
{
    GCodeLine &line = m_gcode_lines[line_idx];
    if (!line.modified) {
        // In pellet mode, unmodified extruding lines must go through push_line_to_output
        // to ensure correct feedrate (a prior modified/split line may have changed the
        // effective F in the GCode stream).
        if (m_pellet_ers_mode && line.extruding()) {
            const char *comment = line.raw.data();
            while (*comment != ';' && *comment != 0) ++comment;
            if (*comment != ';') comment = nullptr;
            push_line_to_output(line_idx, line.feedrate(), comment, ";_ERS_STEADY");
            return;
        }
        push_to_output(line.raw.data(), line.raw_length, true);
        return;
    }

    // The line was modified.
    
    // Special handling for non-extruding F-only lines (feedrate changes)
    std::string_view raw_view(line.raw.data(), line.raw.size());
    if (!line.extruding() && raw_view.find("G1 F") != std::string_view::npos) {
        // Re-emit as simple F command with new feedrate
        GCodeG1Formatter formatter;
        formatter.emit_f(line.feedrate());
        if (m_pellet_ers_mode)
            formatter.emit_string(";_ERS");
        push_to_output(formatter);
        return;
    }
    
    // Find the comment.
    const char *comment = line.raw.data();
    while (*comment != ';' && *comment != 0) ++comment;
    if (*comment != ';')
        comment = nullptr;

    // get the gcode line length
    float l = line.dist_xyz();
    float vol_rate = line.volumetric_extrusion_rate;
    float rate_start = line.volumetric_extrusion_rate_start;
    float rate_end   = line.volumetric_extrusion_rate_end;
    float original_feedrate = line.feedrate(); // F from the original GCode

    // Pellet mode trapezoidal/triangular profile: when EITHER rate_start or rate_end
    // is below the target vol_rate, this line needs internal segmentation.
    // Try trapezoidal (ramp-up -> steady@vol_rate -> ramp-down) first.
    // If both ramps don't fit, use triangular (ramp-up meets ramp-down at reduced peak).
    if (m_pellet_ers_mode && (rate_start < vol_rate * 0.98f || rate_end < vol_rate * 0.98f) && l > 2.f * m_max_segment_length) {
        float slope_pos = line.max_volumetric_extrusion_rate_slope_positive;
        float slope_neg = line.max_volumetric_extrusion_rate_slope_negative;
        if (slope_pos <= 0.f) slope_pos = m_max_volumetric_extrusion_rate_slope_positive;
        if (slope_neg <= 0.f) slope_neg = m_max_volumetric_extrusion_rate_slope_negative;
        // Ramp distances to reach vol_rate from each end.
        // Clamp to 0: when rate_start/rate_end >= vol_rate, no ramp is needed on that side
        // and the formula would produce a negative distance, corrupting position interpolation.
        float l_rampup  = std::max(0.f, (vol_rate * vol_rate - rate_start * rate_start) * original_feedrate / (2.f * slope_pos * vol_rate));
        float l_rampdown = std::max(0.f, (vol_rate * vol_rate - rate_end * rate_end) * original_feedrate / (2.f * slope_neg * vol_rate));

        if (l_rampup + l_rampdown <= l) {
            // === TRAPEZOIDAL PROFILE: ramp-up + steady + ramp-down ===
            float l_steady = l - l_rampup - l_rampdown;
            float pos_orig_start[5], pos_orig_end[5];
            memcpy(pos_orig_start, line.pos_start, sizeof(float) * 5);
            memcpy(pos_orig_end, line.pos_end, sizeof(float) * 5);

            // --- RAMP-UP zone ---
            if (l_rampup >= 0.5f * m_max_segment_length) {
                size_t nSeg = size_t(ceil(l_rampup / m_max_segment_length));
                float t_rampup_end = l_rampup / l; // parametric position where ramp-up ends
                float f_start = rate_start * original_feedrate / vol_rate;
                float f_end   = original_feedrate; // full speed at vol_rate
                for (size_t i = 1; i <= nSeg; ++i) {
                    float t_local = float(i) / float(nSeg); // 0..1 within ramp-up
                    float t_global = t_local * t_rampup_end; // 0..t_rampup_end
                    for (int j = 0; j < 4; ++j) {
                        line.pos_end[j] = pos_orig_start[j] + (pos_orig_end[j] - pos_orig_start[j]) * t_global;
                        line.pos_provided[j] = true;
                    }
                    float t_mid = (float(i) - 0.5f) / float(nSeg);
                    float f_interp = interpolate_ramp(f_start, f_end, t_mid, m_pellet_ers_ramp_profile);
                    push_line_to_output(line_idx, f_interp, comment, ";_ERS_RAMPUP");
                    comment = nullptr;
                    memcpy(line.pos_start, line.pos_end, sizeof(float) * 5);
                }
            }

            // --- STEADY zone ---
            if (l_steady >= 0.5f * m_max_segment_length) {
                float t_steady_start = l_rampup / l;
                float t_steady_end   = (l_rampup + l_steady) / l;
                for (int j = 0; j < 4; ++j) {
                    line.pos_end[j] = pos_orig_start[j] + (pos_orig_end[j] - pos_orig_start[j]) * t_steady_end;
                    line.pos_provided[j] = true;
                }
                push_line_to_output(line_idx, original_feedrate, comment, ";_ERS_STEADY");
                comment = nullptr;
                memcpy(line.pos_start, line.pos_end, sizeof(float) * 5);
            }

            // --- RAMP-DOWN zone ---
            if (l_rampdown >= 0.5f * m_max_segment_length) {
                size_t nSeg = size_t(ceil(l_rampdown / m_max_segment_length));
                float f_start = original_feedrate; // full speed at vol_rate
                float f_end   = rate_end * original_feedrate / vol_rate;
                for (size_t i = 1; i <= nSeg; ++i) {
                    float t_local = float(i) / float(nSeg);
                    float t_global_start = (l_rampup + l_steady) / l;
                    float t_global = t_global_start + t_local * (l_rampdown / l);
                    for (int j = 0; j < 4; ++j) {
                        line.pos_end[j] = pos_orig_start[j] + (pos_orig_end[j] - pos_orig_start[j]) * t_global;
                        line.pos_provided[j] = true;
                    }
                    float t_mid = (float(i) - 0.5f) / float(nSeg);
                    float f_interp = interpolate_ramp(f_start, f_end, t_mid, m_pellet_ers_ramp_profile);
                    push_line_to_output(line_idx, f_interp, comment, ";_ERS_RAMPDOWN");
                    comment = nullptr;
                    memcpy(line.pos_start, line.pos_end, sizeof(float) * 5);
                }
            } else if (l_rampdown > 0.01f) {
                // Remaining ramp-down too short to segment, emit as final piece
                for (int j = 0; j < 4; ++j) {
                    line.pos_end[j] = pos_orig_end[j];
                    line.pos_provided[j] = true;
                }
                float f_avg = rate_end * original_feedrate / vol_rate;
                push_line_to_output(line_idx, f_avg, comment, ";_ERS_RAMPDOWN");
                memcpy(line.pos_start, line.pos_end, sizeof(float) * 5);
            }
            // Catch-all: ensure the entire original line is covered.
            // If short steady/ramp-down zones were skipped due to minimum length
            // filtering, emit one final segment to reach the original endpoint.
            {
                float dx = pos_orig_end[0] - line.pos_start[0];
                float dy = pos_orig_end[1] - line.pos_start[1];
                if (dx * dx + dy * dy > 0.0001f) {
                    for (int j = 0; j < 4; ++j) {
                        line.pos_end[j] = pos_orig_end[j];
                        line.pos_provided[j] = true;
                    }
                    push_line_to_output(line_idx, original_feedrate, comment, ";_ERS_STEADY");
                }
            }
            return;
        }
        // === TRIANGULAR PROFILE: ramp-up meets ramp-down, no steady zone ===
        // Both ramps can't reach vol_rate within this line — find where they meet.
        float k_pos = 2.f * slope_pos * vol_rate / original_feedrate;
        float k_neg = 2.f * slope_neg * vol_rate / original_feedrate;
        float x_meet = (rate_end * rate_end - rate_start * rate_start + k_neg * l) / (k_pos + k_neg);
        x_meet = std::clamp(x_meet, 0.f, l);
        float l_up   = x_meet;
        float l_down = l - x_meet;
        bool has_rampup   = l_up   >= 0.5f * m_max_segment_length;
        bool has_rampdown = l_down >= 0.5f * m_max_segment_length;
        if (has_rampup || has_rampdown) {
            float peak = sqrtf(rate_start * rate_start + k_pos * x_meet);
            peak = std::min(peak, vol_rate);
            float pos_orig_start[5], pos_orig_end[5];
            memcpy(pos_orig_start, line.pos_start, sizeof(float) * 5);
            memcpy(pos_orig_end, line.pos_end, sizeof(float) * 5);
            float f_peak = peak * original_feedrate / vol_rate;
            // --- RAMP-UP zone (rate_start -> peak) ---
            if (has_rampup) {
                size_t nSeg = std::max(size_t(1), size_t(ceil(l_up / m_max_segment_length)));
                float t_up_end = l_up / l;
                float f_start_up = rate_start * original_feedrate / vol_rate;
                for (size_t i = 1; i <= nSeg; ++i) {
                    float t_local = float(i) / float(nSeg);
                    float t_global = t_local * t_up_end;
                    for (int j = 0; j < 4; ++j) {
                        line.pos_end[j] = pos_orig_start[j] + (pos_orig_end[j] - pos_orig_start[j]) * t_global;
                        line.pos_provided[j] = true;
                    }
                    float t_mid = (float(i) - 0.5f) / float(nSeg);
                    float f_interp = interpolate_ramp(f_start_up, f_peak, t_mid, m_pellet_ers_ramp_profile);
                    push_line_to_output(line_idx, f_interp, comment, ";_ERS_RAMPUP");
                    comment = nullptr;
                    memcpy(line.pos_start, line.pos_end, sizeof(float) * 5);
                }
            }
            // --- RAMP-DOWN zone (peak -> rate_end) ---
            if (has_rampdown) {
                size_t nSeg = std::max(size_t(1), size_t(ceil(l_down / m_max_segment_length)));
                float f_end_down = rate_end * original_feedrate / vol_rate;
                for (size_t i = 1; i <= nSeg; ++i) {
                    float t_local = float(i) / float(nSeg);
                    float t_global = (l_up + t_local * l_down) / l;
                    for (int j = 0; j < 4; ++j) {
                        line.pos_end[j] = pos_orig_start[j] + (pos_orig_end[j] - pos_orig_start[j]) * t_global;
                        line.pos_provided[j] = true;
                    }
                    float t_mid = (float(i) - 0.5f) / float(nSeg);
                    float f_interp = interpolate_ramp(f_peak, f_end_down, t_mid, m_pellet_ers_ramp_profile);
                    push_line_to_output(line_idx, f_interp, comment, ";_ERS_RAMPDOWN");
                    comment = nullptr;
                    memcpy(line.pos_start, line.pos_end, sizeof(float) * 5);
                }
            }
            // Catch-all: ensure the entire original line is covered.
            {
                float dx = pos_orig_end[0] - line.pos_start[0];
                float dy = pos_orig_end[1] - line.pos_start[1];
                if (dx * dx + dy * dy > 0.0001f) {
                    for (int j = 0; j < 4; ++j) {
                        line.pos_end[j] = pos_orig_end[j];
                        line.pos_provided[j] = true;
                    }
                    float f_end = rate_end * original_feedrate / vol_rate;
                    push_line_to_output(line_idx, std::max(f_end, original_feedrate * 0.1f), comment, ";_ERS_STEADY");
                }
            }
            return;
        }
        // else: line too short for any meaningful segmentation — fall through to standard linear
    }

    // number of segments this line can be broken down to
    auto nSegments = size_t(ceil(l / m_max_segment_length));
    
    // Orca:
    // Calculate the absolute difference in volumetric extrusion rate between the start and end point of the line.
    // Quantize it to 1mm3/min (0.016mm3/sec).
    int delta_volumetric_rate = std::round(fabs(rate_end - rate_start));

    // Determine ERS tag for pellet mode
    const char *ers_tag = nullptr;
    if (m_pellet_ers_mode) {
        if (line.pellet_ramp) {
            // Line was modified by pellet boundary handler / mini passes
            if (rate_start < rate_end)
                ers_tag = ";_ERS_RAMPUP";
            else if (rate_start > rate_end)
                ers_tag = ";_ERS_RAMPDOWN";
            else
                ers_tag = ";_ERS_STEADY";
        } else {
            // Native ERS adjustment only
            ers_tag = ";_ERS_STEADY";
        }
    }
    
    // Emit the line with lowered extrusion rates.
    // Orca:
    // First, check if the change in volumetric extrusion rate is trivial (less than 10mm3/min -> 0.16mm3/sec (5mm/sec speed for a 0.25 mm nozzle).
    // Or if the line size is equal in length with the smallest segment.
    // If so, then emit the line as a single extrusion, i.e. dont split into segments.
    if ( nSegments == 1 || delta_volumetric_rate < 10) {
        push_line_to_output(line_idx, line.feedrate() * line.volumetric_correction_avg(), comment, ers_tag);
    } else // The line needs to be split the line into segments and apply extrusion rate smoothing
    {
        bool accelerating = rate_start < rate_end;
        // Update the initial and final feed rate values.
        line.pos_start[4] = rate_start * line.pos_end[4] / vol_rate;
        line.pos_end  [4] = rate_end   * line.pos_end[4] / vol_rate;
        float feed_avg = 0.5f * (line.pos_start[4] + line.pos_end[4]);
        // Limiting volumetric extrusion rate slope for this segment.
        float max_volumetric_extrusion_rate_slope = accelerating ? line.max_volumetric_extrusion_rate_slope_positive :
                                                                   line.max_volumetric_extrusion_rate_slope_negative;
        // Total time for the segment, corrected for the possibly lowered volumetric feed rate,
        // if accelerating / decelerating over the complete segment.
        float t_total = line.dist_xyz() / feed_avg;
        // Time of the acceleration / deceleration part of the segment, if accelerating / decelerating
        // with the maximum volumetric extrusion rate slope.
        float t_acc    = 0.5f * (rate_start + rate_end) / max_volumetric_extrusion_rate_slope;
        float l_acc    = l;
        float l_steady = 0.f;
        if (t_acc < t_total && !(m_pellet_ers_mode && line.pellet_ramp)) {
            // One may achieve higher print speeds if part of the segment is not speed limited.
            l_acc    = t_acc * feed_avg;
            l_steady = l - l_acc;
            if (l_steady < 0.5f * m_max_segment_length) {
                l_acc    = l;
                l_steady = 0.f;
            } else
                nSegments = size_t(ceil(l_acc / m_max_segment_length));
        }
        float pos_start[5];
        float pos_end[5];
        float pos_end2[4];
        memcpy(pos_start, line.pos_start, sizeof(float) * 5);
        memcpy(pos_end, line.pos_end, sizeof(float) * 5);
        if (l_steady > 0.f) {
            // There will be a steady feed segment emitted.
            if (accelerating) {
                // Prepare the final steady feed rate segment.
                memcpy(pos_end2, pos_end, sizeof(float)*4);
                float t = l_acc / l;
                for (int i = 0; i < 4; ++ i) {
                    pos_end[i] = pos_start[i] + (pos_end[i] - pos_start[i]) * t;
                    line.pos_provided[i] = true;
                }
            } else {
                // Emit the steady feed rate segment.
                float t = l_steady / l;
                for (int i = 0; i < 4; ++ i) {
                    line.pos_end[i] = pos_start[i] + (pos_end[i] - pos_start[i]) * t;
                    line.pos_provided[i] = true;
                }
                push_line_to_output(line_idx, pos_start[4], comment, ";_ERS_STEADY");
                comment = nullptr;

                float new_pos_start_feedrate = pos_start[4];

                memcpy(line.pos_start, line.pos_end, sizeof(float)*5);
                memcpy(pos_start, line.pos_end, sizeof(float)*5);

                line.pos_start[4] = new_pos_start_feedrate;
                pos_start[4] = new_pos_start_feedrate;
            }
        }
        // Split the segment into pieces.
        for (size_t i = 1; i < nSegments; ++ i) {
            float t = float(i) / float(nSegments);
            for (size_t j = 0; j < 4; ++ j) {
                line.pos_end[j] = pos_start[j] + (pos_end[j] - pos_start[j]) * t;
                line.pos_provided[j] = true;
            } 
            // Interpolate the feed rate at the center of the segment.
            push_line_to_output(line_idx, pos_start[4] + (pos_end[4] - pos_start[4]) * (float(i) - 0.5f) / float(nSegments), comment, ers_tag);
            comment = nullptr;
            memcpy(line.pos_start, line.pos_end, sizeof(float)*5);
        }
		if (l_steady > 0.f && accelerating) {
            for (int i = 0; i < 4; ++ i) {
                line.pos_end[i] = pos_end2[i];
                line.pos_provided[i] = true;
            }
            push_line_to_output(line_idx, pos_end[4], comment, ";_ERS_STEADY");
        } else {
            for (int i = 0; i < 4; ++ i) {
                line.pos_end[i] = pos_end[i];
                line.pos_provided[i] = true;
            }
            push_line_to_output(line_idx, pos_end[4], comment, ers_tag);
        }
    }
    
}

void PressureEqualizer::adjust_volumetric_rate(const size_t first_line_idx, const size_t last_line_idx, const bool is_segment_start, const bool is_segment_end)
{
    // don't bother adjusting volumetric rate if there's no gcode to adjust
    // In pellet boundary mode (ramp-up/ramp-down), we need to process even with 1-2 lines
    bool is_boundary_mode = m_pellet_ers_mode && (is_segment_start || is_segment_end);
    if (!is_boundary_mode && last_line_idx - first_line_idx < 2)
        return;
    if (last_line_idx < first_line_idx)
        return;

    // In pellet boundary mode, the range may include non-extruding lines at edges.
    // Find the actual first/last extruding lines within the range.
    size_t first_extruding_idx = first_line_idx;
    while (first_extruding_idx < last_line_idx && !m_gcode_lines[first_extruding_idx].extruding())
        ++first_extruding_idx;
    
    size_t last_extruding_idx = last_line_idx;
    while (last_extruding_idx > first_line_idx && !m_gcode_lines[last_extruding_idx].extruding())
        --last_extruding_idx;
    
    // In boundary mode (ramp-up/ramp-down), we allow single extruding line
    // In normal mode, we need at least 2 extruding lines
    bool has_extruding_lines = m_gcode_lines[first_extruding_idx].extruding() && m_gcode_lines[last_extruding_idx].extruding();
    if (!has_extruding_lines)
        return;  // No extruding lines in range
    if (!is_boundary_mode && first_extruding_idx >= last_extruding_idx)
        return;  // Need at least 2 extruding lines in normal mode

    size_t       line_idx      = last_extruding_idx;
    
    // Pellet mode boundary handling is applied AFTER the backward/forward passes below,
    // so that the passes don't overwrite the boundary values.
    std::array<float, size_t(ExtrusionRole::erCount)> feedrate_per_extrusion_role{};
    feedrate_per_extrusion_role.fill(std::numeric_limits<float>::max());
    feedrate_per_extrusion_role[int(m_gcode_lines[line_idx].extrusion_role)] = m_gcode_lines[line_idx].volumetric_extrusion_rate_start;

    while (line_idx != first_extruding_idx) {
        size_t idx_prev = line_idx - 1;
        for (; !m_gcode_lines[idx_prev].extruding() && idx_prev != first_extruding_idx; --idx_prev);
        if (!m_gcode_lines[idx_prev].extruding())
            break;
        // Don't decelerate before ironing.
        if (m_gcode_lines[line_idx].extrusion_role == ExtrusionRole::erIroning) {            line_idx = idx_prev;
            continue;
        }
        // Volumetric extrusion rate at the start of the succeeding segment.
        float rate_succ = m_gcode_lines[line_idx].volumetric_extrusion_rate_start;
        
        // What is the gradient of the extrusion rate between idx_prev and idx?
        line_idx        = idx_prev;
        GCodeLine &line = m_gcode_lines[line_idx];

        for (size_t iRole = 1; iRole < size_t(ExtrusionRole::erCount); ++ iRole) {
            const float &rate_slope = m_max_volumetric_extrusion_rate_slopes[iRole].negative;
            if (rate_slope == 0 || feedrate_per_extrusion_role[iRole] == std::numeric_limits<float>::max())
                continue; // The negative rate is unlimited or the rate for ExtrusionRole iRole is unlimited.

            float rate_end = feedrate_per_extrusion_role[iRole];
            if (iRole == size_t(line.extrusion_role) && rate_succ < rate_end)
                // Limit by the succeeding volumetric flow rate.
                rate_end = rate_succ;

            // don't alter the flow rate for these extrusion types
            // Orca: Limit ERS to external perimeters and overhangs if option selected by user
            if (!line.adjustable_flow || line.extrusion_role == ExtrusionRole::erBridgeInfill || line.extrusion_role == ExtrusionRole::erIroning ||
                (m_extrusion_rate_smoothing_external_perimeter_only && line.extrusion_role != ExtrusionRole::erOverhangPerimeter && line.extrusion_role != ExtrusionRole::erExternalPerimeter)) {
                rate_end = line.volumetric_extrusion_rate_end;
            } else if (line.volumetric_extrusion_rate_end > rate_end) {
                line.volumetric_extrusion_rate_end = rate_end;
                line.max_volumetric_extrusion_rate_slope_negative = rate_slope;
                line.modified = true;
            } else if (iRole == size_t(line.extrusion_role)) {
                rate_end = line.volumetric_extrusion_rate_end;
            } else {
                // Use the original, 'floating' extrusion rate as a starting point for the limiter.
            }

            if (line.adjustable_flow) {
                float rate_start = sqrt(rate_end * rate_end + 2 * line.volumetric_extrusion_rate * line.dist_xyz() * rate_slope / line.feedrate());
                if (rate_start < line.volumetric_extrusion_rate_start) {
                    // Limit the volumetric extrusion rate at the start of this segment due to a segment
                    // of ExtrusionType iRole, which will be extruded in the future.
                    line.volumetric_extrusion_rate_start = rate_start;
                    line.max_volumetric_extrusion_rate_slope_negative = rate_slope;
                    line.modified = true;
                }
            }
//            feedrate_per_extrusion_role[iRole] = (iRole == line.extrusion_role) ? line.volumetric_extrusion_rate_start : rate_start;
            // Don't store feed rate for ironing
            if (line.extrusion_role != ExtrusionRole::erIroning)
                feedrate_per_extrusion_role[iRole] = line.volumetric_extrusion_rate_start;
        }
    }

    feedrate_per_extrusion_role.fill(std::numeric_limits<float>::max());
    feedrate_per_extrusion_role[size_t(m_gcode_lines[line_idx].extrusion_role)] = m_gcode_lines[line_idx].volumetric_extrusion_rate_end;

    // Pellet mode: limit feedrate of F-only lines before segment start for proper ramp-up
    // Only when ramp-up was actually applied (travel_before >= threshold)
    if (m_pellet_ers_mode && is_segment_start &&
        m_gcode_lines[first_extruding_idx].travel_before_polyline >= m_pellet_ers_travel_threshold) {
        bool in_wipe = false;
        for (long i = (long)first_line_idx; i < (long)first_extruding_idx; ++i) {
            GCodeLine &line = m_gcode_lines[i];
            std::string_view line_view(line.raw.data(), line.raw.size());
            // Skip wipe blocks — wipe moves have their own independent feedrate
            if (line_view.find("WIPE_START") != std::string_view::npos) in_wipe = true;
            if (line_view.find("WIPE_END") != std::string_view::npos) { in_wipe = false; continue; }
            if (in_wipe) continue;
            if (line_view.find("G1 F") != std::string_view::npos && !line.extruding()) {
                // This is an F-only line - limit its feedrate for ramp-up
                float max_feedrate = 60.0f;  // Start from low speed (1mm/s = 60mm/min)
                if (line.feedrate() > max_feedrate) {
                    line.pos_end[4] = max_feedrate;
                    line.modified = true;
                }
            }
        }
    }

    assert(m_gcode_lines[line_idx].extruding());
    while (line_idx != last_extruding_idx) {
        size_t idx_next = line_idx + 1;
        for (; !m_gcode_lines[idx_next].extruding() && idx_next != last_extruding_idx; ++idx_next);
        if (!m_gcode_lines[idx_next].extruding())
            break;
        // Don't accelerate after ironing.
        if (m_gcode_lines[line_idx].extrusion_role == ExtrusionRole::erIroning) {
            line_idx = idx_next;
            continue;
        }
        // Volumetric extrusion rate at the end of the preceding segment.
        float rate_prec = m_gcode_lines[line_idx].volumetric_extrusion_rate_end;
        
        // What is the gradient of the extrusion rate between idx_prev and idx?
        line_idx = idx_next;
        GCodeLine &line = m_gcode_lines[line_idx];

        for (size_t iRole = 1; iRole < size_t(ExtrusionRole::erCount); ++ iRole) {
            const float &rate_slope = m_max_volumetric_extrusion_rate_slopes[iRole].positive;
            if (rate_slope == 0 || feedrate_per_extrusion_role[iRole] == std::numeric_limits<float>::max())
                continue; // The positive rate is unlimited or the rate for ExtrusionRole iRole is unlimited.

            float rate_start = feedrate_per_extrusion_role[iRole];
            // don't alter the flow rate for these extrusion types
            // Orca: Limit ERS to external perimeters and overhangs if option selected by user
            if (!line.adjustable_flow || line.extrusion_role == ExtrusionRole::erBridgeInfill || line.extrusion_role == ExtrusionRole::erIroning ||
                (m_extrusion_rate_smoothing_external_perimeter_only && line.extrusion_role != ExtrusionRole::erOverhangPerimeter && line.extrusion_role != ExtrusionRole::erExternalPerimeter)) {
                rate_start = line.volumetric_extrusion_rate_start;
            } else if (iRole == size_t(line.extrusion_role) && rate_prec < rate_start)
                rate_start = rate_prec;
            
            if (line.volumetric_extrusion_rate_start > rate_start) {
                line.volumetric_extrusion_rate_start = rate_start;
                line.max_volumetric_extrusion_rate_slope_positive = rate_slope;
                line.modified = true;
            } else if (iRole == size_t(line.extrusion_role)) {
                rate_start = line.volumetric_extrusion_rate_start;
            } else {
                // Use the original, 'floating' extrusion rate as a starting point for the limiter.
            }

            if (line.adjustable_flow) {
                float rate_end = sqrt(rate_start * rate_start + 2 * line.volumetric_extrusion_rate * line.dist_xyz() * rate_slope / line.feedrate());
                if (rate_end < line.volumetric_extrusion_rate_end) {
                    // Limit the volumetric extrusion rate at the start of this segment due to a segment
                    // of ExtrusionType iRole, which was extruded before.
                    line.volumetric_extrusion_rate_end                = rate_end;
                    line.max_volumetric_extrusion_rate_slope_positive = rate_slope;
                    line.modified                                     = true;
                }
            }
//            feedrate_per_extrusion_role[iRole] = (iRole == line.extrusion_role) ? line.volumetric_extrusion_rate_end : rate_end;
            // Don't store feed rate for ironing
            if (line.extrusion_role != ExtrusionRole::erIroning)
                feedrate_per_extrusion_role[iRole] = line.volumetric_extrusion_rate_end;
        }
    }
    
    // Pellet mode boundary handling: apply AFTER passes so values aren't overwritten.
    // The passes work on pairs of lines and may not correctly handle the first/last lines.
    if (m_pellet_ers_mode && is_segment_start) {
        GCodeLine &first_line = m_gcode_lines[first_extruding_idx];
        // Skip ramp-up for short travels - treated as continuous extrusion
        if (first_line.travel_before_polyline < m_pellet_ers_travel_threshold) {
            // No boundary ramp-up needed
        } else if (first_line.adjustable_flow) {
            // Find the positive slope for the first line's extrusion role
            float ramp_slope = 0.f;
            for (size_t iRole = 1; iRole < size_t(ExtrusionRole::erCount); ++iRole) {
                if (m_max_volumetric_extrusion_rate_slopes[iRole].positive > 0 &&
                    first_line.extrusion_role == ExtrusionRole(iRole)) {
                    ramp_slope = m_max_volumetric_extrusion_rate_slopes[iRole].positive;
                    break;
                }
            }
            if (ramp_slope > 0.f) {
                float ramp_target = first_line.volumetric_extrusion_rate;
                // Walk forward through extruding lines, distributing the ramp-up
                // across as many GCodeLines as needed until the target rate is reached.
                float rate_prec = m_pellet_ers_min_rate;
                for (size_t idx = first_extruding_idx; idx <= last_extruding_idx; ++idx) {
                    GCodeLine &line = m_gcode_lines[idx];
                    if (!line.extruding() || !line.adjustable_flow)
                        continue;
                    // Ramp completed — stop
                    if (rate_prec >= ramp_target)
                        break;
                    float dist = line.dist_xyz();
                    float feedrate = line.feedrate();
                    if (feedrate <= 0.f || dist <= 0.f)
                        continue;
                    line.volumetric_extrusion_rate_start = rate_prec;
                    // rate_end = sqrt(rate_start^2 + 2 * slope * vol * dist / F)
                    float rate_end = sqrtf(rate_prec * rate_prec
                        + 2.f * ramp_slope * line.volumetric_extrusion_rate * dist / feedrate);
                    rate_end = std::min(rate_end, ramp_target);
                    line.volumetric_extrusion_rate_end = rate_end;
                    line.max_volumetric_extrusion_rate_slope_positive = ramp_slope;
                    line.modified = true;
                    line.pellet_ramp = true;
                    rate_prec = rate_end;
                }
            }
        }
    }
    if (m_pellet_ers_mode && is_segment_end) {
        GCodeLine &last_line = m_gcode_lines[last_extruding_idx];
        // Skip ramp-down for short travels - treated as continuous extrusion
        if (last_line.travel_after_polyline < m_pellet_ers_travel_threshold) {
            // No boundary ramp-down needed
        } else if (last_line.adjustable_flow) {
            // Find the negative slope for the last line's extrusion role.
            // Use the dedicated deceleration slope if configured, otherwise the role-based slope.
            float ramp_slope = 0.f;
            if (m_pellet_ers_deceleration_slope > 0.f) {
                ramp_slope = m_pellet_ers_deceleration_slope;
            } else {
                for (size_t iRole = 1; iRole < size_t(ExtrusionRole::erCount); ++iRole) {
                    if (m_max_volumetric_extrusion_rate_slopes[iRole].negative > 0 &&
                        last_line.extrusion_role == ExtrusionRole(iRole)) {
                        ramp_slope = m_max_volumetric_extrusion_rate_slopes[iRole].negative;
                        break;
                    }
                }
            }
            if (ramp_slope > 0.f) {
                float ramp_target = last_line.volumetric_extrusion_rate;
                // Walk backward through extruding lines, distributing the ramp-down
                // across as many GCodeLines as needed until the target rate is reached.
                float rate_succ = m_pellet_ers_min_rate;
                size_t idx = last_extruding_idx;
                while (true) {
                    GCodeLine &line = m_gcode_lines[idx];
                    if (line.extruding() && line.adjustable_flow) {
                        // Ramp completed — stop
                        if (rate_succ >= ramp_target)
                            break;
                        float dist = line.dist_xyz();
                        float feedrate = line.feedrate();
                        if (feedrate > 0.f && dist > 0.f) {
                            // Use std::min to preserve ramp-up values already set
                            line.volumetric_extrusion_rate_end = std::min(rate_succ, line.volumetric_extrusion_rate_end);
                            // rate_start = sqrt(rate_end^2 + 2 * slope * vol * dist / F)
                            float rate_start = sqrtf(rate_succ * rate_succ
                                + 2.f * ramp_slope * line.volumetric_extrusion_rate * dist / feedrate);
                            rate_start = std::min(rate_start, ramp_target);
                            line.volumetric_extrusion_rate_start = std::min(rate_start, line.volumetric_extrusion_rate_start);
                            line.max_volumetric_extrusion_rate_slope_negative = ramp_slope;
                            line.modified = true;
                            line.pellet_ramp = true;
                            rate_succ = rate_start;
                        }
                    }
                    if (idx <= first_extruding_idx)
                        break;
                    --idx;
                }
            }
        }
    }

    // Also limit feedrate of non-extruding F-only lines immediately after segment (ramp-down)
    // Only when ramp-down was actually applied (travel_after >= threshold)
    if (m_pellet_ers_mode && is_segment_end &&
        m_gcode_lines[last_extruding_idx].travel_after_polyline >= m_pellet_ers_travel_threshold) {
        for (size_t i = last_extruding_idx + 1; i <= last_line_idx; ++i) {
            GCodeLine &line = m_gcode_lines[i];
            std::string_view line_view(line.raw.data(), line.raw.size());
            // Stop before wipe block — wipe has its own independent feedrate
            if (line_view.find("WIPE_START") != std::string_view::npos)
                break;
            if (line_view.find("G1 F") != std::string_view::npos && !line.extruding()) {
                // This is an F-only line - limit its feedrate for ramp-down
                float max_feedrate = 60.0f;  // End with low speed (1mm/s = 60mm/min)
                if (line.feedrate() > max_feedrate) {
                    line.pos_end[4] = max_feedrate;
                    line.modified = true;
                }
            }
        }
    }
}

inline void PressureEqualizer::push_to_output(GCodeG1Formatter &formatter)
{
    return this->push_to_output(formatter.string(), false);
}

inline void PressureEqualizer::push_to_output(const std::string &text, bool add_eol)
{
    return this->push_to_output(text.data(), text.size(), add_eol);
}

inline void PressureEqualizer::push_to_output(const char *text, const size_t len, bool add_eol)
{
    // New length of the output buffer content.
    size_t len_new = output_buffer_length + len + 1;
    if (add_eol)
        ++len_new;

    // Resize the output buffer to a power of 2 higher than the required memory.
    if (output_buffer.size() < len_new) {
        size_t v = len_new;
        // Compute the next highest power of 2 of 32-bit v
        // http://graphics.stanford.edu/~seander/bithacks.html
        v--;
        v |= v >> 1;
        v |= v >> 2;
        v |= v >> 4;
        v |= v >> 8;
        v |= v >> 16;
        v++;
        output_buffer.resize(v);
    }

    // Copy the text to the output.
    if (len != 0) {
        memcpy(output_buffer.data() + output_buffer_length, text, len);
        this->output_buffer_prev_length = this->output_buffer_length;
        output_buffer_length += len;
    }
    if (add_eol)
        output_buffer[output_buffer_length++] = '\n';
    output_buffer[output_buffer_length] = 0;
}

inline bool is_just_line_with_extrude_set_speed_tag(const std::string &line)
{
    if (line.empty() || !boost::starts_with(line, "G1 ") || !boost::ends_with(line, EXTRUDE_SET_SPEED_TAG))
        return false;

    const char       *p_line   = line.data() + 3;
    const char *const line_end = line.data() + line.length() - 1;
    while (!is_eol(*p_line)) {
        if (toupper(*p_line++) == 'F')
            break;
        else
            return false;
    }
    parse_float(p_line, line_end - p_line);
    eatws(p_line);
    p_line += EXTRUDE_SET_SPEED_TAG.length();
    return p_line <= line_end && is_eol(*p_line);
}

void PressureEqualizer::push_line_to_output(const size_t line_idx, float new_feedrate, const char *comment, const char *ers_tag)
{
    // Orca: sanity check, 1 mm/s is the minimum feedrate.
    if (new_feedrate < 60)
        new_feedrate = 60;
    // Quantize speed changes to a minimum of 1mm/sec, to reduce gcode volume for trivial speed changes.
    new_feedrate = std::round(new_feedrate / 60.0) * 60.0;
    const GCodeLine &line = m_gcode_lines[line_idx];
    // Add ERS debug comment when line was modified by pressure equalizer
    const bool add_ers_comment = m_pellet_ers_mode && (line.modified || ers_tag != nullptr);
    if (line_idx > 0 && output_buffer_length > 0) {
        const std::string prev_line_str = std::string(output_buffer.begin() + int(this->output_buffer_prev_length),
                                                      output_buffer.begin() + int(this->output_buffer_length) + 1);
        if (is_just_line_with_extrude_set_speed_tag(prev_line_str))
            this->output_buffer_length = this->output_buffer_prev_length; // Remove the last line because it only sets the speed for an empty block of g-code lines, so it is useless.
        else
            push_to_output(EXTRUDE_END_TAG.data(), EXTRUDE_END_TAG.length(), true);
    } else
        push_to_output(EXTRUDE_END_TAG.data(), EXTRUDE_END_TAG.length(), true);

    GCodeG1Formatter feedrate_formatter;
    feedrate_formatter.emit_f(new_feedrate);
    feedrate_formatter.emit_string(std::string(EXTRUDE_SET_SPEED_TAG.data(), EXTRUDE_SET_SPEED_TAG.length()));
    if (line.extrusion_role == ExtrusionRole::erExternalPerimeter)
        feedrate_formatter.emit_string(std::string(EXTERNAL_PERIMETER_TAG.data(), EXTERNAL_PERIMETER_TAG.length()));
    push_to_output(feedrate_formatter);

    GCodeG1Formatter extrusion_formatter;
    for (size_t axis_idx = 0; axis_idx < 3; ++axis_idx)
        if (line.pos_provided[axis_idx])
            extrusion_formatter.emit_axis(char('X' + axis_idx), line.pos_end[axis_idx], GCodeFormatter::XYZF_EXPORT_DIGITS);
    extrusion_formatter.emit_axis('E', m_use_relative_e_distances ? (line.pos_end[3] - line.pos_start[3]) : line.pos_end[3], GCodeFormatter::E_EXPORT_DIGITS);

    if (comment != nullptr)
        extrusion_formatter.emit_string(std::string(comment));
    
    if (add_ers_comment) {
        if (ers_tag != nullptr)
            extrusion_formatter.emit_string(std::string(ers_tag));
        else
            extrusion_formatter.emit_string(";_ERS");
    }

    push_to_output(extrusion_formatter);
}

} // namespace Slic3r
