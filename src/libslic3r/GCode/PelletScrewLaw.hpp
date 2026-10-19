// Pellet screw start/stop law (Ginger), MVP stage.
//
// Post-processing stage that runs AFTER the CoolingBuffer and replaces the pellet ERS of the
// PressureEqualizer with the tuned screw law (restart injection + END taper/coast) of the
// ginger-screw-dynamics study. Activated ONLY by the environment variable
//     GINGER_SCREW_LAW=<path of the law JSON>   (e.g. studies/2026-10-15_review/law_tuned_dry.json)
// GINGER_SCREW_LAW_SELFTEST=1 prints the self-test to stderr (reference: selftest_ref_general.txt for a law_general JSON,
// selftest_ref_tuned_dry.txt for law_tuned_dry.json).
//
// The namespace ScrewLaw is a 1:1 port of the reference Python implementation
//     studies/2026-10-14_tuning/law_general.py         (= law_tuned.py: model, END law, restart law, check
//                                                         + gen/law_low.py capped restart / END floor, see LawCoef)
//     studies/2026-10-15_review/make_law_json.py       (trunc, short-run policy B, self-test cases)
// In case of doubt the Python code is the source of truth (see MVP_DESIGN.md v2, PORT_GENERAL.md). A law JSON without the
// law_general keys of LawCoef (law_tuned_dry.json) gives exactly the law_tuned behaviour of C0-C4.
//
// Units: lengths mm, times s, speeds mm/s, volumes mm3, flows mm3/s. Commanded E = mm of E = commanded
// mm3 (pellet profiles: filament_diameter 1.12838 -> 1 mm2 section); "delivered" = eta * commanded.
//
// Checkpoints: C0 model + laws + policy B + self-test. C1 G-code parser (byte-identical pass-through).
// C2 Klipper-like timing, model integration along the G-code, stop/run detection, plan_run per restart.
// C3/C4 EMIT: the law is written into the G-code (restart + policy B + END + pause + screw reverse), the model
// integrated on the EMITTED moves; guardrails are errors.
//
// Environment (read once per export in create_from_env):
//   GINGER_SCREW_LAW=<json>        activates the stage (the PressureEqualizer is not created). Default mode EMIT.
//   GINGER_SCREW_LAW_SELFTEST=1    self-test to stderr at the stage creation
//   GINGER_SCREW_LAW_DRY=1         diagnostic: parse + analyse, output rebuilt from the parsed line slices: byte-identical
//   GINGER_SCREW_LAW_ANNOTATE=1    diagnostic: as DRY + only comment lines ";_ERS_LAW restart ..." / ";_ERS_LAW end ..."
//   GINGER_SCREW_LAW_DUMP=1        every annotation / header line also to the log ([SCREWLAW] ...)
//
// EMIT output (MVP_DESIGN sec. 2.1; every line that is not written by the law is copied verbatim):
//   ;_ERS_LAW restart ...                                 header of every restart (inputs of the plan, full precision;
//                                                         law_general: + x_s x_s_by e_c fit; the END header + v_floor coast_stop)
//   [SET_VELOCITY_LIMIT ACCEL=<a> ;_ERS_ACC]              before a law move whose design ACCEL differs (a2 on acc)
//   G1 E<E> F<F> ;_ERS_RAMPUP anchor i=<k>                standing pieces (merged as merge_standing)
//   G1 X Y [Z] E F ;_ERS_RAMPUP creep|down|acc|tail i=<k> e0=<A L>      along the ORIGINAL path, split at its vertices
//   G1 X Y [Z] E F<orig> ;_ERS_SPLIT                      part of an original line outside the law (E prop. to L)
//   ;_ERS_LAW end ...                                     header of every END
//   G1 X Y [Z] [E] F ;_ERS_RAMPDOWN taper|coast i=<k> e0=<A L>          (coast: no E)
//   [G1 E-<V> F ;_ERS_REV ...]                            screw reverse, layers >= 2 (stop.reverse_mm3 > 0)
//   G4 P<ms> ;_ERS_PAUSE                                  rest of the machine.pause_s pause (300 ms without reverse)
//   [G1 Z<z + hop> F ;_ERS_EXIT hop                       exit after every run end (stop.exit_mm > 0): lift, then a short
//    G1 X Y F ;_ERS_EXIT forward                          move along the last bead direction, so that the strand that does
//    [G1 Z<z> F ;_ERS_EXIT down]]                         not cut pulls the bead lengthwise (as the v4x-v6x generators);
//                                                         "down" only if the next input motion does not set Z itself
//   G1 F<orig> ;_ERS_F / SET_VELOCITY_LIMIT ACCEL=<orig> ;_ERS_ACC      stream state restored for the verbatim lines
// i = index of the law segment after merge_standing (pieces of one segment split at vertices share it).
// XY(Z) 4 decimals (more only for sub-0.0014 mm pieces), E 5 decimals (per-segment error diffusion), F 2 decimals.
#pragma once

#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

namespace Slic3r {
class PrintConfig;

namespace ScrewLaw {

// Fast/slow store model (law_tuned.MODEL). Volumes mm3 DELIVERED, flows mm3/s DELIVERED.
struct ModelParams { double k100 = 0., n = 0., r100 = 0., tau_f = 0., lam = 0., k2 = 0., n2 = 0., tau2 = 0.; };

// law_tuned.COEF without the knobs (restart formulas + END constants).
struct LawCoef {
    double c_f = 0., c_s = 0., x_e = 0., c_v = 0., L_B = 0., a_0 = 0., a_e = 0., c_b = 0., q_0 = 0., c_L = 0., c_g = 0.; // restart
    double T_t = 0., v_floor = 0., T_cmax = 0.;                                                                     // end
    // law_general (2026-10-17, law_low.py as printed in v64/v65). All optional in the JSON; 0 / false (missing) = off =
    // law_tuned. Restart: anchor x_s = min(formula, anchor_cap_A A, f G1) (delivered; the cap is NOT scaled by k_r), then
    // x_s <= anchor_max_cmd eta (ONE extrude-only move of <= anchor_max_cmd commanded mm3); creep cross-section
    // e_c = max(A, min(Q_MAX / (eta v_c), creep_cap_A A)); fit_total: the step-down starts lower when f G1 - x_s does not
    // cover it, so that the fast injection is exactly f G1. END: the coast stops below min(v_floor, coast_floor_rel v);
    // below end_coarse_v the taper / coast steps are coarser so that every END move is >= end_seg_min.
    double anchor_cap_A = 0., anchor_max_cmd = 0., creep_cap_A = 0.;
    bool   fit_total = false;
    double coast_floor_rel = 0., end_coarse_v = 0., end_seg_min = 0.;
    bool   general = false;    // at least one law_general key was given in the JSON (self-test and log format)
};

// Calibration knobs: k(Q) = clamp(k0 + k1 * ln(max(Q,1) / q_ref), clamp_min, clamp_max), Q = eta A v (delivered).
// The knobs come ONLY from the JSON `knobs` section (review2: the law_coef defaults 1.05 / 1.00 would be wrong).
struct Knobs {
    double kr_0 = 0.74, kr_1 = 0., ke_0 = 0.76, ke_1 = 0., q_ref = 200., clamp_min = 0.6, clamp_max = 1.6;
    // END knob on layers >= 2 (v61-v62). Optional in the JSON: when missing (ke_wall_set = false) the wall
    // knob is the same as k_e, as MVP_DESIGN sec. 2.5 prescribes; the defaults below are only the review-3 values
    // (v61 + v62 refit, valid 157-315 mm3/s; outside the curve is extrapolated and clamped like every knob).
    double ke_wall_0 = 1.062, ke_wall_1 = -0.231;
    bool   ke_wall_set = false;
    double k_r(double v, double A, double eta) const;
    double k_e(double v, double A, double eta, bool wall = false) const;
};

// Stop extras (v62-v63): small screw reverse during the pause, layers >= 2 only; 0 = off. Optional in the JSON.
// Exit (stampa A sA1, 2026-10-18: the slicer travel right after the pause - 3 mm z-hop, then straight to the next
// object, often back over the bead - drags the strand that does not cut and carries the strips / walls away): after
// the pause the head lifts exit_hop_mm and moves exit_mm forward along the last bead direction at exit_speed_mm_s
// (the research generators: hop 1 mm, then ~10 mm forward), then the slicer travel follows. exit_mm 0 = off.
struct StopParams {
    double reverse_mm3 = 0.; double reverse_time_s = 0.2;
    double exit_mm = 0.; double exit_hop_mm = 1.; double exit_speed_mm_s = 500.;
};

enum class ShortPolicy { B };   // only B in the MVP (A = "END only" rejected by v54)

struct ShortRuns {
    ShortPolicy policy = ShortPolicy::B;
    // Restart knob on TRUNCATED short runs; < 0 = same as Knobs::k_r (v58: keep -1, lowering it does not fix fat ends).
    double k_r_trunc = -1.;
};

struct MachineParams {
    double eta = 0.984, q_max = 520.0, icv = 30.0, icv_frac = 0.6, l_stand = 0.05;
    double print_acc = 500.0, e_acc = 3000.0;
    double end_dt = 0.001, dt_coast = 0.025, pause_s = 0.3;
    bool   end_from_state = false;        // false = design_end from steady state (as v51-v57)
    double stop_travel_mm = 3.0;          // a gap is a STOP if XY travel >= this, or it has a Z move / G4
    double first_stop_s = 5.0;            // assumed stop after START_PRINT
    double sim_dt = 0.002;
    double z_max_v = 7.0, z_max_a = 100.0, travel_acc_default = 2500.0;
    double max_extrude_only_e = 100.0, max_cross_section = 150.0;   // required in the JSON (100: v64/v65 anchors of 99 mm3)
    double tail_speed_tol = 0.01;
    double end_taper_n0_margin = 0.95;
    std::vector<double> end_taper_mults { 1.0, 1.25, 1.5, 2.0, 2.5, 3.0, 4.0 };
};

struct LawParams {
    ModelParams   model;
    LawCoef       coef;
    Knobs         knobs;
    ShortRuns     short_runs;
    StopParams    stop;
    MachineParams mach;
    std::string   version;
    std::string   path;     // file it was loaded from (informative)
    // Throws Slic3r::RuntimeError on a missing file, a parse error, a missing REQUIRED key or an invalid value.
    // Optional: knobs.ke_wall_0/1, the whole `stop` section, short_runs.k_r_trunc (-1), version.
    static LawParams from_json_file(const std::string &path);
};

struct ModelState { double V = 0., V2 = 0., q = 0.; };

enum class SegKind { Taper, Coast, Anchor, Creep, Down, Acc, Tail };
const char *seg_kind_name(SegKind k);

// One law segment along the ORIGINAL path: L mm of head travel (0 = standing), E commanded mm (= mm3),
// dt design duration (F = 60 L/dt), acc = SET_VELOCITY_LIMIT ACCEL for this move.
struct Segment { double L, E, dt; SegKind kind; double acc; };

struct RestartParams { double Q, x_s, v_c, f, L_B, a2, b, L_b, g; };

// What design_restart did (law_general.design_restart info): the anchor actually laid and the bound that set it
// ("formula" / "cap_A" / "fG1" / "eonly"), the creep cross-section, the fit_total step-down, the creep length.
struct RestartInfo {
    double x_s = 0., x_s_law = 0., e_c = 0., L_c = 0., inj = 0.;   // inj = fast injection (delivered mm3, tail excluded)
    const char *x_s_by = "formula";
    bool   fit = false;
};

// What design_end did: the coast floor used and why the coast stopped ("T_cmax" / "floor_abs" / "floor_rel").
struct EndInfo {
    double v_floor = 0., k_c = 0., coast_len = 0., coast_t = 0., dt_coast = 0.;
    const char *reason = "floor_abs";
};

// Sum as Python 3.12+ sum() does (Neumaier compensated), to reproduce the reference bit for bit.
double py_sum(const std::vector<double> &xs);
double seg_length(const std::vector<Segment> &segs);   // = sum(s["L"]) (compensated)

// ---- model (port 1:1 of law_tuned.step / steady_state / state_after_stop / deficits)
ModelState steady_state(const ModelParams &p, double Q);
double     step(const ModelParams &p, ModelState &st, double qe, double dt);   // returns nozzle outflow q
// END taper from steady Q_prev (screw linear to 0 in coef.T_t), then the screw stopped for t_stop s.
ModelState state_after_stop(const LawParams &lp, double Q_prev, double t_stop, double dt = 0.002);
void       deficits(const ModelParams &p, double Q, const ModelState &st, double &G1, double &D2);

// ---- laws (port 1:1 of law_general.design_end / restart_params / design_restart / check; = law_tuned without the
// law_general keys). k_c < 0 -> lp.knobs.k_e(v, A); k_r < 0 -> lp.knobs.k_r(v, A).
// design_end throws Slic3r::RuntimeError below coef.end_coarse_v when no taper discretisation has moves >= end_seg_min
// (law_low raises ValueError there; only below ~0.45 mm/s).
std::vector<Segment> design_end(double v, double A, const LawParams &lp, const ModelState *st = nullptr, double k_c = -1.,
                                EndInfo *info = nullptr);
double               v_floor_eff(double v, const LawParams &lp);   // min(v_floor, coast_floor_rel v) (or v_floor)
const char          *end_stop_reason(const std::vector<Segment> &segs, double v, const LawParams &lp);
RestartParams        restart_params(double v, double A, const LawParams &lp, double k_r = -1.);
std::vector<Segment> design_restart(double v, double A, double G1, double D2, const LawParams &lp,
                                    bool tail = true, double tail_max_L = -1. /* <0 = none */,
                                    const RestartParams *prm = nullptr, RestartInfo *info = nullptr);
std::vector<std::string> check(const std::vector<Segment> &segs, double v, const LawParams &lp);
// Consecutive standing segments (L < l_stand) merged into one (as make_screw_id5x.emit()).
std::vector<Segment> merge_standing(const std::vector<Segment> &segs, double l_stand);
// First L_max mm of head travel, the last segment cut proportionally (make_law_json.trunc).
std::vector<Segment> truncate(const std::vector<Segment> &segs, double L_max, double l_stand);

// ---- short-run policy B (make_law_json.policy_B)
enum class RunPlan { Full, FastEndTrunc, FastTrunc };
const char *run_plan_name(RunPlan p);
struct PlannedRun { RunPlan plan; std::vector<Segment> restart, end; double L_fast, L_end; RestartInfo rinfo; };
// L = run length (mm) between two stops, G1/D2 = deficits at the restart. wall = layer >= 2 (END knob ke_wall).
// end_in: optional precomputed design_end(v, A, k_e(v, A, wall)) (the stage caches it); nullptr = computed here.
PlannedRun plan_run(double v, double A, double L, double G1, double D2, const LawParams &lp, bool wall = false,
                    const std::vector<Segment> *end_in = nullptr);

// Prints the lines of selftest_ref_general.txt (lp.coef.general) or selftest_ref_tuned_dry.txt (same cases as
// make_law_json.py build_general / build_tuned); returns 0 = OK.
// A non-empty check() on any END or RESTART design (and on the policy-B plans) is a failure.
int self_test(const LawParams &lp, FILE *out);

} // namespace ScrewLaw


// ===================================================================================== the stage (C1-C2)
namespace ScrewLaw {

// Machine / G-code values the stage needs besides the law JSON. Kept independent of PrintConfig so that the
// standalone harness can run PelletScrewLaw::process_layer on real G-code files; create_from_env fills it.
// The planner limits are those of tools/model/kplanner.LIMITS (the reference of the timing check); Z limits,
// extruder ICV / acceleration and the initial ACCEL come from the JSON `machine` section.
struct StageConfig {
    bool   relative_e   = true;     // use_relative_e_distances: M83 is in the start G-code, never seen by the stage
    double max_velocity = 500.;     // mm/s, cap of every move (Klipper max_velocity)
    double max_accel    = 3000.;    // mm/s2, cap of SET_VELOCITY_LIMIT ACCEL= / M204
    double scv          = 5.;       // initial square_corner_velocity (Klipper default)
    double e_max_v      = 2000.;    // extruder max velocity (mm/s of E)
    double init_f       = 600.;     // initial modal F (mm/min)
    double bed_min[2] { -1e9, -1e9 }, bed_max[2] { 1e9, 1e9 };   // printable area bounding box (stop exit kept inside)
};

// Emit (default): the law is written into the G-code (C3/C4); guardrails are errors.
// Dry: analysis, output rebuilt from the parsed line slices (C1 identity check); guardrails are warnings.
// Annotate: as Dry + ";_ERS_LAW" comment lines before each restart and after each run end.
enum class StageMode { Emit, Dry, Annotate };
const char *stage_mode_name(StageMode m);

enum class MoveType : unsigned char { Extrude, Travel, ZMove, EOnly, Retract, Dwell, Accel, Other };
const char *move_type_name(MoveType t);

// One G-code line of a chunk (MVP sec. 2.1 Move). Every line of the chunk has exactly one Move (comments, empty
// lines and macros are Other), so the output can be rebuilt byte for byte from the slices.
//   Extrude  XY(Z) displacement with E > 0          Travel  XY displacement, E = 0, no Z change
//   ZMove    Z change without positive E             EOnly   no displacement, E > 0
//   Retract  E < 0 (with or without displacement)    Dwell   G4
//   Accel    SET_VELOCITY_LIMIT ACCEL= / SQUARE_CORNER_VELOCITY= or M204
struct Move {
    size_t   begin = 0, len = 0;   // content slice of the chunk (without the line end)
    unsigned char eol = 0;         // line end bytes after the content: 0, 1 ("\n") or 2 ("\r\n")
    MoveType type = MoveType::Other;
    bool     plan  = false;        // takes part in the planner (displacement or E, or G4)
    bool     eonly = false;        // planner: no XYZ displacement
    bool     arc   = false;        // G2 / G3 (timed with the arc length, junctions 0)
    bool     motion = false;       // G0-G3 line (also F only)
    bool     has_f = false;        // G0-G3 line with an F word
    bool     sets_accel = false;   // SET_VELOCITY_LIMIT ACCEL= / M204 changing the acceleration
    bool     rel_xyz = false;      // G91 active
    bool     run_end = false;      // EMIT: last extrusion move of a run (origin of the stop time)
    bool     origin = false;       // EMIT: START_PRINT / *PURGE* marker line (first-stop origin)
    int      role  = -1;           // index in PelletScrewLaw::roles() (last ;TYPE:)
    int      layer = 0;            // 1 = first layer
    size_t   line  = 0;            // global line index: input lines (Dry / Annotate), output lines (Emit)
    double   p0[3] { 0., 0., 0. }, p1[3] { 0., 0., 0. };   // p1 = position after the line (also G92 / G28)
    double   e = 0., f = 0.;       // relative E (mm = commanded mm3), modal F (mm/min)
    double   accel = 0., scv = 0.; // ACCEL (capped by max_accel) / SQUARE_CORNER_VELOCITY active for this move
    double   accel_raw = 0.;       // ACCEL as written in the G-code (restored verbatim by the emitter)
    double   dwell = 0.;           // G4 (s)
    // planner (kplanner / kp2, klipper=True)
    double   L = 0., L_xy = 0., v = 0., a = 0., epm = 0., dir[3] { 0., 0., 0. };
    double   v0 = 0., vc = 0., v1 = 0., la = 0., ld = 0., ta = 0., tc = 0., td = 0.;
    double   t0 = 0., t1 = 0.;     // absolute stage time (s)
};

// Run = consecutive extrusion moves between two stops (MVP sec. 6 decision 5). Gaps without a stop (XY travel
// < stop_travel_mm, no Z move, no G4, no retraction) stay inside the run.
struct Run {
    size_t first = 0, last = 0;    // indices (in the chunk) of the first and the last extrusion move
    double L = 0.;                 // extrusion path length (in-run short travels excluded)
    double v_start = 0., A_start = 0., v_end = 0., A_end = 0.;   // first / last mm: v = commanded F, A = E / L
    int    layer = 0;
    bool   arc = false;            // contains G2/G3 -> unsupported
};

} // namespace ScrewLaw

// The post-processing stage. Created per G-code export (GCode::process_layers), runs in the serial
// cooling filter after CoolingBuffer::process_layer. All the state (position, F, ACCEL, model) is kept
// across the chunks (layers).
class PelletScrewLaw {
public:
    // Guardrail (MVP_DESIGN sec. 2.3): runs with delivered flow above this fraction of q_max are rejected (C4).
    static constexpr double FLOW_GUARD_FACTOR = 0.95;

    struct Stats {
        // C1 parser
        size_t layers = 0, chunks = 0, lines = 0, comments = 0, empty = 0, macros = 0, known = 0;
        size_t unknown = 0;            // G/M/T commands not recognised (timed 0 s, passed through)
        size_t unsupported = 0;        // G2/G3, G28, G20, T<n>, G91 E-axis tricks...: lines the law cannot model
        size_t moves[8] {};            // by MoveType
        size_t no_motion_g1 = 0;       // G0/G1 without displacement and E (F only)
        bool   absolute_e = false;     // M82 / use_relative_e_distances off -> pass-through for the rest of the file
        // C2 runs and stops
        size_t runs = 0, ends = 0;
        size_t stops = 0, stop_travel = 0, stop_z = 0, stop_dwell = 0, stop_retract = 0, stop_chunk_end = 0;
        size_t chunk_splits = 0;       // run apparently continuing across a chunk boundary (no stop): split there
        size_t full = 0, fast_end_trunc = 0, fast_trunc = 0;   // short-run policy B outcomes (planned, not emitted)
        size_t unsupported_runs = 0;   // runs with arcs
        size_t over_flow = 0;          // runs with Q = eta A v > FLOW_GUARD_FACTOR * q_max (start or end)
        size_t bad_runs = 0;           // v or A not positive -> no plan
        size_t pre_first_e = 0;        // E-only / extrusion-less E moves before the first run (not integrated)
        double first_stop_s = -1.;     // first-stop estimate (assumed), < 0 = no run yet
        double first_stop_gcode_s = 0.;// part of it measured on the G-code
        double t_total = 0.;           // estimated print time of the chunks seen by the stage (s)
        size_t model_steps = 0;
        size_t carried = 0;            // moves carried to the next chunk (lookahead across the boundary), summed
        size_t provisional = 0;        // restarts whose first move was not settled at annotation time (chunk tail)
        size_t end_cache_hits = 0, end_cache_misses = 0;
        size_t rejected = 0;           // runs rejected by the guardrails (C4)
        // input lines the law cannot coexist with (EMIT: error; DRY / ANNOTATE: warning)
        size_t in_retracts = 0, in_fw_retracts = 0, in_wipes = 0;
        // C3/C4 emission
        size_t law_restarts = 0, law_ends = 0, law_lines = 0, standing_lines = 0, split_lines = 0, verbatim_run_moves = 0;
        size_t accel_lines = 0, f_restores = 0, pauses = 0, exits = 0, reverses = 0, pause_extended = 0, comp_applied = 0;
        size_t tail_speed_cuts = 0, tail_fits = 0, tail_fit_fallbacks = 0, tiny_skipped = 0, fine_xy = 0;
        // law_general: which bound set the anchor, fit_total step-downs, why the coast stopped (emitted plans)
        size_t xs_formula = 0, xs_cap_A = 0, xs_fG1 = 0, xs_eonly = 0, fits = 0, creep_capped = 0;
        size_t end_floor_abs = 0, end_floor_rel = 0, end_T_cmax = 0, end_coarse = 0;
        double comp_mm3 = 0.;          // delivered mm3 added to restarts by the reverse compensation
        double E_runs_in = 0., E_runs_out = 0.;   // commanded E of the runs before / after the law
        double law_time = 0.;          // seconds spent in the emitter (wall clock)
    };

    // GINGER_SCREW_LAW set and not empty (cached at the first call).
    static bool enabled_from_env();
#ifndef SCREWLAW_STANDALONE
    // nullptr when disabled; throws Slic3r::RuntimeError if the JSON cannot be loaded or the self-test fails.
    static std::unique_ptr<PelletScrewLaw> create_from_env(const PrintConfig &config);
#endif

    // Throws Slic3r::RuntimeError in Emit mode with absolute E (cfg.relative_e false).
    explicit PelletScrewLaw(ScrewLaw::LawParams lp, ScrewLaw::StageConfig cfg = ScrewLaw::StageConfig(),
                            ScrewLaw::StageMode mode = ScrewLaw::StageMode::Emit, bool dump = false);

    // One layer of G-code from the CoolingBuffer: with the law (Emit), rebuilt identical (Dry) or with
    // ";_ERS_LAW" comment lines only (Annotate). Emit throws Slic3r::RuntimeError on a guardrail.
    std::string process_layer(std::string &&gcode);
    void        log_stats() const;
    // End of the print (harness only, the output does not depend on it): times / integrates the carried tail.
    void        finish();

    const ScrewLaw::LawParams   &params() const { return m_lp; }
    const ScrewLaw::StageConfig &stage_config() const { return m_cfg; }
    ScrewLaw::StageMode          mode() const { return m_mode; }
    const Stats                 &stats() const { return m_stats; }
    const std::vector<std::string> &roles() const { return m_roles; }
    // Delivered flow Q (mm3/s) within the guardrail (Q <= FLOW_GUARD_FACTOR * q_max).
    bool flow_ok(double Q) const { return Q <= FLOW_GUARD_FACTOR * m_lp.mach.q_max; }

    // Debug / harness: called for every planned move (G0-G3 with displacement or E, G4) with the global line
    // index (0-based, over all the chunks), its type and its absolute start / end time.
    using TimingSink = std::function<void(size_t line, ScrewLaw::MoveType type, double t0, double t1)>;
    void set_timing_sink(TimingSink sink) { m_timing_sink = std::move(sink); }

private:
    void parse(const std::string &g);          // C1: m_moves of the chunk, parser state kept across chunks
    void move_time();                          // C2: Klipper-like trapezoids with lookahead over the chunk
    void find_runs();                          // C2: m_runs of the chunk + stop causes
    // C2: model along a move on the fixed grid t = grid * sim_dt (as kp2.timeline)
    void integrate(const ScrewLaw::Move &m, ScrewLaw::ModelState &st, long long &grid);
    void analyse(std::vector<std::pair<size_t, std::string>> *notes);   // runs + model + plans (+ notes)
    const std::vector<ScrewLaw::Segment> &end_design(double v, double A, double k_c, double &L_end);
    void log_line(const std::string &s) const;
    void log_cli(const std::string &s) const;  // log + stderr (the CLI shows only stderr)
    // kp2.parse kinematics of a linear (or arc) move: m.e, m.f, m.accel set; fills plan, L, v, a, epm, dir, eonly
    void kinematics(ScrewLaw::Move &m, double dx, double dy, double dz, double L_xy, bool is_arc) const;
    void input_guardrails();                   // retractions / firmware retraction / wipes in the input

    // ---- C3/C4 emitter
    struct Out;                                // output text of a chunk
    struct Path;                               // extrusion path of a run (arc length along the original moves)
    std::string emit_chunk(const std::string &g);
    void emit_run(Out &o, const std::vector<ScrewLaw::Move> &M, const ScrewLaw::Run &run);
    void time_piece();                         // times + integrates m_piece (lookahead carried as in move_time)
    void put_verbatim(Out &o, const ScrewLaw::Move &m, bool origin = false);
    void put_accel(Out &o, double acc_raw);
    void push_move(ScrewLaw::Move &&mv, const char *what);   // into m_piece + emitted-move guardrails
    void put_law_move(Out &o, const ScrewLaw::Move &ref, const double p1[3], bool z, int dec, long long e_units,
                      double F, double acc, const std::string &tag, bool split);
    void put_standing(Out &o, const ScrewLaw::Move &ref, double E, double dt, const std::string &tag);
    void put_exit(Out &o, const std::vector<ScrewLaw::Move> &M, const ScrewLaw::Run &run);
    void lay_block(Out &o, Path &path, const std::vector<ScrewLaw::Segment> &segs, double A_ref, bool rampup, double s_nominal);
    void lay_plain(Out &o, Path &path, double target);
    void advance(Out &o, Path &path, double target, const std::function<void(size_t, double, double)> &fn);
    [[noreturn]] void fail(const std::string &what, const ScrewLaw::Run *run) const;

    ScrewLaw::LawParams   m_lp;
    ScrewLaw::StageConfig m_cfg;
    ScrewLaw::StageMode   m_mode;
    bool                  m_dump;
    Stats                 m_stats;
    TimingSink            m_timing_sink;

    // per chunk
    std::vector<ScrewLaw::Move> m_moves;
    std::vector<ScrewLaw::Run>  m_runs;

    // parser state (across chunks)
    double m_pos[3] { 0., 0., 0. };
    double m_f = 600., m_accel = 0., m_accel_raw = 0., m_scv = 5.;
    bool   m_rel_xyz = false, m_rel_e = true;
    bool   m_passthrough = false;              // absolute E: nothing is analysed any more
    int    m_layer = 0;                        // ;LAYER_CHANGE counter
    bool   m_layer_tags = false;               // a ;LAYER_CHANGE was seen (otherwise layers from the Z of extrusions)
    double m_layer_z = -1e9;                   // Z of the current layer (fallback)
    int    m_role = -1;
    std::vector<std::string> m_roles;
    size_t m_line_base = 0;                    // global index of the first line of the chunk
    // timing / model state (across chunks)
    // planner lookahead across the chunk boundary: unsettled tail of the previous chunk (see move_time)
    struct Carry { ScrewLaw::Move m; size_t gline = 0; size_t chunk_idx = size_t(-1); bool last_ext = false; };
    std::vector<Carry> m_carry;                // unsettled tail, re-timed with the next chunk
    std::vector<Carry> m_pre;                  // carried moves settled by the current chunk (integrated first)
    std::vector<char>  m_settled;              // per move of the chunk: timing final
    size_t m_first_unsettled = 0;              // first unsettled move of the chunk (= size when none)
    double m_carry_v0 = 0.;                    // entry speed of the first carried move
    double m_t_settled = 0.;                   // stage clock at the end of the last settled move
    double m_t_origin = 0.;                    // first-stop origin: stage start or the last START_PRINT / *PURGE* line
    long   m_origin_idx = -1;                  // that line in the chunk being parsed (-1 = none)
    bool   m_origin_seen = false;
    bool   m_model_on = false;                 // model initialised at the first run (first-stop assumption)
    ScrewLaw::ModelState m_st;
    long long m_grid = 0;                      // next model sample: t = m_grid * sim_dt
    bool   m_have_prev_run = false;
    double m_prev_run_end_t = 0.;              // end time of the last extrusion of the previous run
    // gap after the last extrusion, carried over the chunk boundary (chunk_splits)
    bool   m_gap_open = false, m_gap_stop = false;
    double m_gap_xy = 0.;
    // END design cache: (v, A, k_c) -> segments (design_end from the steady state only depends on these)
    std::map<std::tuple<double, double, double>, std::pair<double, std::vector<ScrewLaw::Segment>>> m_end_cache;
    double m_t_last = 0.;                      // end time of the last timed move (provisional end of the piece)
    // input guardrails
    bool   m_warned_input = false;
    std::string m_first_bad_input;             // first retraction / wipe line (message)
    // emitter state (across chunks): the printer state after the EMITTED lines
    std::vector<ScrewLaw::Move> m_piece;       // emitted plan moves not timed yet
    double m_emit_f = 600., m_emit_accel = 0.; // modal F / ACCEL (as written) of the emitted stream
    double m_epos[3] { 0., 0., 0. };           // emitted position (as written, rounded)
    size_t m_out_line = 0;                     // emitted lines of the previous chunks
    double m_pending_comp = 0.;                // delivered mm3 of the screw reverses, added to the next restart
};

} // namespace Slic3r
