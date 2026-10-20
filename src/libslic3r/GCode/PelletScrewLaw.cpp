// Pellet screw start/stop law (Ginger), MVP stage. See PelletScrewLaw.hpp.
#include "PelletScrewLaw.hpp"
#include "../Exception.hpp"

#include <fast_float/fast_float.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <system_error>
#include <utility>

#ifndef SCREWLAW_STANDALONE
#include "../PrintConfig.hpp"
#include <boost/log/trivial.hpp>
#endif

namespace Slic3r {
namespace ScrewLaw {

// ===================================================================================== helpers

double py_sum(const std::vector<double> &xs)
{
    // CPython >= 3.12 builtin sum() over floats: Neumaier's improved Kahan-Babuska summation.
    double f = 0., c = 0.;
    for (double x : xs) {
        double t = f + x;
        if (std::fabs(f) >= std::fabs(x)) c += (f - t) + x;
        else                              c += (x - t) + f;
        f = t;
    }
    if (c != 0. && std::isfinite(c)) f += c;
    return f;
}

double seg_length(const std::vector<Segment> &segs)
{
    std::vector<double> xs; xs.reserve(segs.size());
    for (const Segment &s : segs) xs.push_back(s.L);
    return py_sum(xs);
}

const char *seg_kind_name(SegKind k)
{
    switch (k) {
    case SegKind::Taper:  return "taper";
    case SegKind::Coast:  return "coast";
    case SegKind::Anchor: return "anchor";
    case SegKind::Creep:  return "creep";
    case SegKind::Down:   return "down";
    case SegKind::Acc:    return "acc";
    case SegKind::Tail:   return "tail";
    }
    return "?";
}

const char *run_plan_name(RunPlan p)
{
    switch (p) {
    case RunPlan::Full:         return "full";
    case RunPlan::FastEndTrunc: return "fast_end_trunc";
    case RunPlan::FastTrunc:    return "fast_trunc";
    }
    return "?";
}

static std::string strfmt(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return std::string();
    if (size_t(n) < sizeof(buf)) return std::string(buf, size_t(n));
    std::string out(size_t(n) + 1, '\0');
    va_start(ap, fmt);
    vsnprintf(&out[0], out.size(), fmt, ap);
    va_end(ap);
    out.resize(size_t(n));
    return out;
}

// ===================================================================================== knobs

static double knob_curve(double k0, double k1, double Q, const Knobs &kn)
{
    return std::min(kn.clamp_max, std::max(kn.clamp_min, k0 + k1 * std::log(std::max(Q, 1.0) / kn.q_ref)));
}

double Knobs::k_r(double v, double A, double eta) const
{
    return knob_curve(kr_0, kr_1, eta * A * v, *this) * restart_scale;
}

double Knobs::k_e(double v, double A, double eta, bool wall) const
{
    if (wall && ke_wall_set) return knob_curve(ke_wall_0, ke_wall_1, eta * A * v, *this) * coast_scale;
    return knob_curve(ke_0, ke_1, eta * A * v, *this) * coast_scale;
}

// ===================================================================================== JSON

using nlohmann::json;

static const json &req_obj(const json &j, const char *key, const std::string &path)
{
    auto it = j.find(key);
    if (it == j.end() || !it->is_object())
        throw Slic3r::RuntimeError("Pellet screw law: section \"" + std::string(key) + "\" missing or not an object in " + path);
    return *it;
}

static double req_num(const json &sec, const char *sec_name, const char *key, const std::string &path)
{
    auto it = sec.find(key);
    if (it == sec.end())
        throw Slic3r::RuntimeError("Pellet screw law: required key \"" + std::string(sec_name) + "." + key + "\" missing in " + path);
    if (!it->is_number())
        throw Slic3r::RuntimeError("Pellet screw law: key \"" + std::string(sec_name) + "." + key + "\" is not a number in " + path);
    return it->get<double>();
}

static bool req_bool(const json &sec, const char *sec_name, const char *key, const std::string &path)
{
    auto it = sec.find(key);
    if (it == sec.end())
        throw Slic3r::RuntimeError("Pellet screw law: required key \"" + std::string(sec_name) + "." + key + "\" missing in " + path);
    if (!it->is_boolean())
        throw Slic3r::RuntimeError("Pellet screw law: key \"" + std::string(sec_name) + "." + key + "\" is not a boolean in " + path);
    return it->get<bool>();
}

static double opt_num(const json &sec, const char *sec_name, const char *key, double def, bool *found, const std::string &path)
{
    auto it = sec.find(key);
    if (found) *found = it != sec.end();
    if (it == sec.end()) return def;
    if (!it->is_number())
        throw Slic3r::RuntimeError("Pellet screw law: key \"" + std::string(sec_name) + "." + key + "\" is not a number in " + path);
    return it->get<double>();
}

static bool opt_bool(const json &sec, const char *sec_name, const char *key, bool def, bool *found, const std::string &path)
{
    auto it = sec.find(key);
    if (found) *found = it != sec.end();
    if (it == sec.end()) return def;
    if (!it->is_boolean())
        throw Slic3r::RuntimeError("Pellet screw law: key \"" + std::string(sec_name) + "." + key + "\" is not a boolean in " + path);
    return it->get<bool>();
}

static void require(bool ok, const std::string &what, const std::string &path)
{
    if (!ok) throw Slic3r::RuntimeError("Pellet screw law: invalid value: " + what + " in " + path);
}

LawParams LawParams::from_json_file(const std::string &path)
{
    std::ifstream f(path, std::ios::in | std::ios::binary);
    if (!f)
        throw Slic3r::RuntimeError("Pellet screw law: cannot open the law JSON \"" + path + "\" (GINGER_SCREW_LAW)");
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return from_json_text(text, path);
}

LawParams LawParams::from_json_text(const std::string &text, const std::string &path)
{
    json j;
    try {
        j = json::parse(text);
    } catch (const std::exception &e) {
        throw Slic3r::RuntimeError("Pellet screw law: JSON parse error in \"" + path + "\": " + e.what());
    }
    if (!j.is_object())
        throw Slic3r::RuntimeError("Pellet screw law: the root of \"" + path + "\" is not a JSON object");

    LawParams lp;
    lp.path = path;
    if (auto it = j.find("version"); it != j.end() && it->is_string()) lp.version = it->get<std::string>();

    // ---- machine
    const json &m = req_obj(j, "machine", path);
    MachineParams &mp = lp.mach;
    mp.eta                 = req_num(m, "machine", "eta", path);
    mp.q_max               = req_num(m, "machine", "q_max", path);
    mp.icv                 = req_num(m, "machine", "icv", path);
    mp.icv_frac            = req_num(m, "machine", "icv_frac", path);
    mp.l_stand             = req_num(m, "machine", "l_stand", path);
    mp.print_acc           = req_num(m, "machine", "print_acc", path);
    mp.e_acc               = req_num(m, "machine", "e_acc", path);
    mp.end_dt              = req_num(m, "machine", "end_dt", path);
    mp.dt_coast            = req_num(m, "machine", "dt_coast", path);
    mp.pause_s             = req_num(m, "machine", "pause_s", path);
    mp.end_from_state      = req_bool(m, "machine", "end_from_state", path);
    mp.stop_travel_mm      = req_num(m, "machine", "stop_travel_mm", path);
    mp.first_stop_s        = req_num(m, "machine", "first_stop_s", path);
    mp.pause_assumed_s     = opt_num(m, "machine", "pause_assumed_s", mp.pause_assumed_s, nullptr, path);
    mp.sim_dt              = req_num(m, "machine", "sim_dt", path);
    mp.z_max_v             = req_num(m, "machine", "z_max_v", path);
    mp.z_max_a             = req_num(m, "machine", "z_max_a", path);
    mp.travel_acc_default  = req_num(m, "machine", "travel_acc_default", path);
    mp.max_extrude_only_e  = req_num(m, "machine", "max_extrude_only_e", path);
    mp.max_cross_section   = req_num(m, "machine", "max_cross_section", path);
    mp.tail_speed_tol      = req_num(m, "machine", "tail_speed_tol", path);
    mp.end_taper_n0_margin = req_num(m, "machine", "end_taper_n0_margin", path);
    {
        auto it = m.find("end_taper_mults");
        if (it == m.end())
            throw Slic3r::RuntimeError("Pellet screw law: required key \"machine.end_taper_mults\" missing in " + path);
        if (!it->is_array() || it->empty())
            throw Slic3r::RuntimeError("Pellet screw law: \"machine.end_taper_mults\" must be a non-empty array in " + path);
        mp.end_taper_mults.clear();
        for (const json &x : *it) {
            if (!x.is_number())
                throw Slic3r::RuntimeError("Pellet screw law: \"machine.end_taper_mults\" must contain numbers in " + path);
            mp.end_taper_mults.push_back(x.get<double>());
        }
    }

    // ---- model
    const json &mo = req_obj(j, "model", path);
    ModelParams &p = lp.model;
    p.k100  = req_num(mo, "model", "k100", path);
    p.n     = req_num(mo, "model", "n", path);
    p.r100  = req_num(mo, "model", "r100", path);
    p.tau_f = req_num(mo, "model", "tau_f", path);
    p.k2    = req_num(mo, "model", "k2", path);
    p.n2    = req_num(mo, "model", "n2", path);
    p.tau2  = req_num(mo, "model", "tau2", path);
    p.lam   = req_num(mo, "model", "lam", path);

    // ---- coef
    const json &co = req_obj(j, "coef", path);
    LawCoef &c = lp.coef;
    c.c_f     = req_num(co, "coef", "c_f", path);
    c.c_s     = req_num(co, "coef", "c_s", path);
    c.x_e     = req_num(co, "coef", "x_e", path);
    c.c_v     = req_num(co, "coef", "c_v", path);
    c.L_B     = req_num(co, "coef", "L_B", path);
    c.a_0     = req_num(co, "coef", "a_0", path);
    c.a_e     = req_num(co, "coef", "a_e", path);
    c.c_b     = req_num(co, "coef", "c_b", path);
    c.q_0     = req_num(co, "coef", "q_0", path);
    c.c_L     = req_num(co, "coef", "c_L", path);
    c.c_g     = req_num(co, "coef", "c_g", path);
    c.T_t     = req_num(co, "coef", "T_t", path);
    c.v_floor = req_num(co, "coef", "v_floor", path);
    c.T_cmax  = req_num(co, "coef", "T_cmax", path);
    {   // law_general keys (optional: missing = off = law_tuned)
        bool fk[7] = { false, false, false, false, false, false, false };
        c.anchor_cap_A    = opt_num(co, "coef", "anchor_cap_A", 0., &fk[0], path);
        c.anchor_max_cmd  = opt_num(co, "coef", "anchor_max_cmd", 0., &fk[1], path);
        c.creep_cap_A     = opt_num(co, "coef", "creep_cap_A", 0., &fk[2], path);
        c.fit_total       = opt_bool(co, "coef", "fit_total", false, &fk[3], path);
        c.coast_floor_rel = opt_num(co, "coef", "coast_floor_rel", 0., &fk[4], path);
        c.end_coarse_v    = opt_num(co, "coef", "end_coarse_v", 0., &fk[5], path);
        c.end_seg_min     = opt_num(co, "coef", "end_seg_min", 0., &fk[6], path);
        c.general = fk[0] || fk[1] || fk[2] || fk[3] || fk[4] || fk[5] || fk[6];
    }

    // ---- knobs (the ONLY source of the knobs)
    const json &kn = req_obj(j, "knobs", path);
    Knobs &k = lp.knobs;
    k.kr_0      = req_num(kn, "knobs", "kr_0", path);
    k.kr_1      = req_num(kn, "knobs", "kr_1", path);
    k.ke_0      = req_num(kn, "knobs", "ke_0", path);
    k.ke_1      = req_num(kn, "knobs", "ke_1", path);
    k.q_ref     = req_num(kn, "knobs", "q_ref", path);
    k.clamp_min = req_num(kn, "knobs", "clamp_min", path);
    k.clamp_max = req_num(kn, "knobs", "clamp_max", path);
    {
        bool f0 = false, f1 = false;
        double w0 = opt_num(kn, "knobs", "ke_wall_0", k.ke_wall_0, &f0, path);
        double w1 = opt_num(kn, "knobs", "ke_wall_1", k.ke_wall_1, &f1, path);
        if (f0 != f1)
            throw Slic3r::RuntimeError("Pellet screw law: \"knobs.ke_wall_0\" and \"knobs.ke_wall_1\" must be given together in " + path);
        if (f0) { k.ke_wall_0 = w0; k.ke_wall_1 = w1; }
        k.ke_wall_set = f0;
    }

    // ---- short runs
    const json &sr = req_obj(j, "short_runs", path);
    {
        auto it = sr.find("policy");
        if (it == sr.end())
            throw Slic3r::RuntimeError("Pellet screw law: required key \"short_runs.policy\" missing in " + path);
        if (!it->is_string() || it->get<std::string>() != "B")
            throw Slic3r::RuntimeError("Pellet screw law: \"short_runs.policy\" must be \"B\" (the only policy of the MVP) in " + path);
        lp.short_runs.policy    = ShortPolicy::B;
        lp.short_runs.k_r_trunc = opt_num(sr, "short_runs", "k_r_trunc", -1., nullptr, path);
    }

    // ---- stop (optional section)
    if (auto it = j.find("stop"); it != j.end()) {
        if (!it->is_object())
            throw Slic3r::RuntimeError("Pellet screw law: section \"stop\" is not an object in " + path);
        lp.stop.reverse_mm3    = opt_num(*it, "stop", "reverse_mm3", lp.stop.reverse_mm3, nullptr, path);
        lp.stop.reverse_time_s = opt_num(*it, "stop", "reverse_time_s", lp.stop.reverse_time_s, nullptr, path);
        lp.stop.exit_mm         = opt_num(*it, "stop", "exit_mm", lp.stop.exit_mm, nullptr, path);
        lp.stop.exit_hop_mm     = opt_num(*it, "stop", "exit_hop_mm", lp.stop.exit_hop_mm, nullptr, path);
        lp.stop.exit_speed_mm_s = opt_num(*it, "stop", "exit_speed_mm_s", lp.stop.exit_speed_mm_s, nullptr, path);
    }

    // ---- sanity
    require(mp.eta > 0. && mp.eta <= 1.5, "machine.eta", path);
    require(mp.q_max > 0., "machine.q_max", path);
    require(mp.icv > 0. && mp.icv_frac > 0., "machine.icv / icv_frac", path);
    require(mp.l_stand > 0., "machine.l_stand", path);
    require(mp.print_acc > 0., "machine.print_acc", path);
    require(mp.end_dt > 0. && mp.dt_coast > 0. && mp.sim_dt > 0., "machine.end_dt / dt_coast / sim_dt", path);
    require(mp.end_taper_n0_margin > 0., "machine.end_taper_n0_margin", path);
    for (double x : mp.end_taper_mults) require(x > 0., "machine.end_taper_mults", path);
    require(p.k100 > 0. && p.n > 0. && p.r100 > 0. && p.k2 > 0. && p.n2 > 0. && p.tau2 > 0. && p.tau_f >= 0. && p.lam >= 0. && p.lam < 1.,
            "model", path);
    require(c.T_t > 0. && c.v_floor > 0. && c.T_cmax > 0., "coef.T_t / v_floor / T_cmax", path);
    require(c.anchor_cap_A >= 0. && c.creep_cap_A >= 0. && (c.creep_cap_A == 0. || c.creep_cap_A >= 1.),
            "coef.anchor_cap_A / creep_cap_A (>= 0; creep_cap_A 0 = off or >= 1)", path);
    require(c.anchor_max_cmd >= 0. && c.anchor_max_cmd <= mp.max_extrude_only_e,
            "coef.anchor_max_cmd (0 = off, <= machine.max_extrude_only_e)", path);
    require(c.coast_floor_rel >= 0. && c.coast_floor_rel <= 1., "coef.coast_floor_rel (0 = off, <= 1)", path);
    require(c.end_coarse_v >= 0. && (c.end_coarse_v == 0. || c.end_seg_min >= mp.l_stand),
            "coef.end_coarse_v / end_seg_min (end_seg_min >= machine.l_stand when end_coarse_v > 0)", path);
    require(k.q_ref > 0. && k.clamp_min > 0. && k.clamp_min <= k.clamp_max, "knobs.q_ref / clamp_min / clamp_max", path);
    require(lp.stop.reverse_mm3 >= 0. && lp.stop.reverse_time_s > 0., "stop.reverse_mm3 / reverse_time_s", path);
    require(lp.stop.exit_mm >= 0. && lp.stop.exit_hop_mm >= 0. && lp.stop.exit_speed_mm_s > 0.,
            "stop.exit_mm / exit_hop_mm (>= 0) / exit_speed_mm_s (> 0)", path);
    return lp;
}

// ===================================================================================== model

static double rho_of(const ModelParams &p)
{
    double n = p.n, r = p.r100, q0 = r * 100.0;
    return std::max(0.0, (std::pow(100.0, n) - std::pow(q0, n)) / q0);
}

ModelState steady_state(const ModelParams &p, double Q)
{
    double kp = p.k100 / std::pow(100.0, p.n), kp2 = p.k2 / std::pow(100.0, p.n2);
    ModelState st;
    st.V  = Q > 0 ? kp * std::pow(Q, p.n) : 0.0;
    st.V2 = Q > 0 ? kp2 * std::pow(Q, p.n2) : 0.0;
    st.q  = Q;
    return st;
}

double step(const ModelParams &p, ModelState &st, double qe, double dt)
{
    const double n = p.n, kp = p.k100 / std::pow(100.0, n), rho = rho_of(p), tf = p.tau_f, keep = 1.0 - p.lam;
    const double kp2 = p.k2 / std::pow(100.0, p.n2);
    double V2t = qe > 0 ? kp2 * std::pow(qe, p.n2) : 0.0;
    double V2n = V2t + (st.V2 - V2t) * std::exp(-dt / p.tau2);
    double d2  = std::min(qe, (V2n - st.V2) / dt);
    st.V2 += d2 * dt;
    double qe2 = qe - d2;
    double V   = st.V;
    double Vse = qe > 0 ? kp * std::pow(qe, n) : 0.0;
    if (tf > 0 && V < Vse) {   // FILL: the store is re-pressurised, lam leaks out
        double fill = std::max(0.0, std::min(qe2, (Vse - V) / tf));
        st.V = std::min(Vse, V + keep * fill * dt);
        st.q = qe2 - keep * fill;
        return st.q;
    }
    double S = rho * qe2 + V / kp;
    if (S <= 0) {
        st.q = 0.0;
        return 0.0;
    }
    double q = st.q > 0 ? st.q : (rho > 0 ? S / rho : std::pow(S, 1 / n));
    if (rho == 0)
        q = std::pow(S, 1 / n);
    else {
        for (int it = 0; it < 30; ++it) {
            double qn = std::pow(q, n), f = rho * q + qn - S, d = rho + n * qn / q;
            double q2 = q - f / d;
            if (q2 <= 0) q2 = q * 0.3;
            if (std::abs(q2 - q) < 1e-9 * (q + 1e-9)) { q = q2; break; }
            q = q2;
        }
    }
    st.V = std::max(0.0, V + (qe2 - q) * dt);
    st.q = q;
    return q;
}

ModelState state_after_stop(const LawParams &lp, double Q_prev, double t_stop, double dt)
{
    const ModelParams &p = lp.model;
    const double T_t = lp.coef.T_t;
    ModelState st = steady_state(p, Q_prev);
    double t = 0.0;
    while (t < T_t - 1e-12) {
        step(p, st, Q_prev * std::max(0.0, 1 - (t + 0.5 * dt) / T_t), dt);
        t += dt;
    }
    t = 0.0;
    while (t < t_stop - 1e-12) {
        step(p, st, 0.0, dt);
        t += dt;
    }
    return st;
}

void deficits(const ModelParams &p, double Q, const ModelState &st, double &G1, double &D2)
{
    ModelState ss = steady_state(p, Q);
    G1 = std::max(0.0, ss.V - st.V);
    D2 = std::max(0.0, ss.V2 - st.V2);
}

// ===================================================================================== END law

// law_tuned._end_segments; v_floor = the effective coast floor, dt_coast = the coast step (coarser below end_coarse_v)
static std::vector<Segment> end_segments(double v, double A, const LawParams &lp, const ModelState *st0, double k_c, int n_t,
                                         double v_floor, double dt_coast)
{
    const ModelParams &p = lp.model;
    const double eta = lp.mach.eta, dt = lp.mach.end_dt;
    const double T_t = lp.coef.T_t, T_cmax = lp.coef.T_cmax;
    const double dtt = T_t / n_t;
    const double Ad  = eta * A;
    double t = 0.0;
    ModelState st = st0 == nullptr ? steady_state(p, eta * A * v) : *st0;
    std::vector<Segment> segs;
    double acc_L = 0., acc_E = 0., acc_t = 0.;
    // Bounded by T_cmax (one coast segment every dt_coast); the guard only protects from absurd parameters.
    const double t_guard = T_t + T_cmax + 10. * dt_coast + 1.;
    while (true) {
        double qc = t < T_t ? A * v * std::max(0.0, 1 - (t + 0.5 * dt) / T_t) : 0.0;
        double q  = step(p, st, eta * qc, dt);
        double vh = (1.0 + (k_c - 1.0) * std::min(1.0, (t + 0.5 * dt) / T_t)) * q / Ad;   // k_c ramped in over the taper
        acc_L += vh * dt; acc_E += qc * dt; acc_t += dt; t += dt;
        if (acc_t >= (t <= T_t + 1e-9 ? dtt : dt_coast) - 1e-9) {
            segs.push_back({ acc_L, acc_E, acc_t, t <= T_t + 1e-9 ? SegKind::Taper : SegKind::Coast, lp.mach.print_acc });
            acc_L = acc_E = acc_t = 0.;
            if (t > T_t && (vh < v_floor || t > T_t + T_cmax)) break;
        }
        if (t > t_guard) break;
    }
    return segs;
}

double v_floor_eff(double v, const LawParams &lp)
{
    const LawCoef &c = lp.coef;
    return c.coast_floor_rel > 0. ? std::min(c.v_floor, c.coast_floor_rel * v) : c.v_floor;
}

const char *end_stop_reason(const std::vector<Segment> &segs, double v, const LawParams &lp)
{
    std::vector<double> ts;
    for (const Segment &s : segs) if (s.kind == SegKind::Coast) ts.push_back(s.dt);
    if (py_sum(ts) > lp.coef.T_cmax) return "T_cmax";
    return lp.coef.coast_floor_rel > 0. && lp.coef.coast_floor_rel * v < lp.coef.v_floor ? "floor_rel" : "floor_abs";
}

std::vector<Segment> design_end(double v, double A, const LawParams &lp, const ModelState *st, double k_c, EndInfo *info)
{
    const MachineParams &m = lp.mach;
    const LawCoef &c = lp.coef;
    if (k_c < 0) k_c = lp.knobs.k_e(v, A, m.eta);
    const double vf = v_floor_eff(v, lp);
    const double n0 = std::max(8.0, std::ceil(A / (m.end_taper_n0_margin * m.icv_frac * m.icv / v)));
    std::vector<Segment> segs;
    double dtc = m.dt_coast;
    if (!(c.end_coarse_v > 0. && v < c.end_coarse_v)) {
        for (double mult : m.end_taper_mults) {
            segs = end_segments(v, A, lp, st, k_c, int(std::ceil(n0 * mult)), vf, dtc);
            if (check(segs, v, lp).empty()) break;
        }
    } else {
        // law_low.design_end_arm below V_COARSE: the largest taper count whose moves are all >= end_seg_min (and pass
        // check()), coast steps x1.25 until every coast move is >= end_seg_min (otherwise the whole END would be laid standing)
        const double seg_min = c.end_seg_min;
        auto all_ge = [seg_min](const std::vector<Segment> &sg, SegKind kind) {
            for (const Segment &q : sg) if (q.kind == kind && !(q.L >= seg_min)) return false;
            return true;
        };
        for (int round = 0;; ++round) {
            if (round > 200)
                throw Slic3r::RuntimeError(strfmt("Pellet screw law: END %gx%g: no coast discretisation with moves >= %g mm", v, A, seg_min));
            bool found = false;
            for (int n_t = int(n0); n_t > 0; --n_t) {
                segs = end_segments(v, A, lp, st, k_c, n_t, vf, dtc);
                if (all_ge(segs, SegKind::Taper) && check(segs, v, lp).empty()) { found = true; break; }
            }
            if (!found)
                throw Slic3r::RuntimeError(strfmt("Pellet screw law: END %gx%g: no taper discretisation with moves >= %g mm "
                                                  "(bead speed too low for the END law)", v, A, seg_min));
            if (all_ge(segs, SegKind::Coast)) break;
            dtc *= 1.25;
        }
    }
    if (info != nullptr) {
        std::vector<double> Ls, ts;
        for (const Segment &s : segs) if (s.kind == SegKind::Coast) { Ls.push_back(s.L); ts.push_back(s.dt); }
        info->v_floor = vf; info->k_c = k_c; info->coast_len = py_sum(Ls); info->coast_t = py_sum(ts); info->dt_coast = dtc;
        info->reason = end_stop_reason(segs, v, lp);
    }
    return segs;
}

// ===================================================================================== RESTART law

RestartParams restart_params(double v, double A, const LawParams &lp, double k_r)
{
    const LawCoef &c = lp.coef;
    const double eta = lp.mach.eta, Q_MAX = lp.mach.q_max, PRINT_ACC = lp.mach.print_acc;
    RestartParams r;
    r.Q = eta * A * v;
    if (k_r < 0) k_r = lp.knobs.k_r(v, A, eta);
    r.v_c  = std::min(v, std::max(3.0, c.c_v * lp.model.lam * Q_MAX / (eta * A)));
    r.a2   = std::min(PRINT_ACC, std::max(100.0, c.a_0 * std::pow(100.0 / r.Q, c.a_e)));
    double B = k_r * c.c_b * std::max(0.0, r.Q - c.q_0);
    r.L_b  = std::max(1.0, c.c_L * v);
    r.b    = 2.0 * B / (eta * A * r.L_b);
    r.x_s  = k_r * c.c_s * A * std::pow(100.0 / v, c.x_e);
    r.f    = k_r * c.c_f;
    r.L_B  = c.L_B;
    r.g    = c.c_g;
    return r;
}

std::vector<Segment> design_restart(double v, double A, double G1, double D2, const LawParams &lp,
                                    bool tail, double tail_max_L, const RestartParams *prm_in, RestartInfo *info)
{
    const MachineParams &m = lp.mach;
    const LawCoef &co = lp.coef;
    const double eta = m.eta, Q_MAX = m.q_max, ICV = m.icv, ICV_FRAC = m.icv_frac, L_STAND = m.l_stand, PRINT_ACC = m.print_acc;
    const RestartParams prm = prm_in == nullptr ? restart_params(v, A, lp) : *prm_in;
    const double tau2 = lp.model.tau2;
    std::vector<Segment> segs;

    // anchor: law_tuned max(0, formula); law_general (law_low 'cap') min(formula, anchor_cap_A A, f G1), <= anchor_max_cmd eta
    double x_s = std::max(0.0, prm.x_s);
    const char *x_s_by = "formula";
    if (co.anchor_cap_A > 0.) {
        if (co.anchor_cap_A * A < x_s) { x_s = co.anchor_cap_A * A; x_s_by = "cap_A"; }
        if (prm.f * G1 < x_s)          { x_s = prm.f * G1;          x_s_by = "fG1"; }
    }
    if (co.anchor_max_cmd > 0. && co.anchor_max_cmd * eta < x_s) { x_s = co.anchor_max_cmd * eta; x_s_by = "eonly"; }
    if (x_s > 0.5) segs.push_back({ 0.0, x_s / eta, x_s / Q_MAX, SegKind::Anchor, PRINT_ACC });
    const double v_c = std::min(prm.v_c, v), a2 = prm.a2;
    double e_c = std::max(A, Q_MAX / (eta * v_c));           // E/mm with the screw at Q_MAX during the creep
    if (co.creep_cap_A > 0.) e_c = std::max(A, std::min(e_c, co.creep_cap_A * A));   // law_general: creep <= creep_cap_A A
    const double b = std::max(0.0, prm.b), L_b = std::max(0.5, prm.L_b);
    const double e_b0 = A * (1 + b);
    const double dmax = ICV_FRAC * ICV / v_c * 0.98;
    // step-down from the creep cross-section ec to e_b0 (law_low.design_restart_arm down())
    int nB = 1;
    double LB = 0., extra_B = 0.;
    std::vector<double> eB;
    auto down = [&](double ec, int &nB_, double &LB_, std::vector<double> &eB_) {
        nB_ = std::max(1, int(std::ceil(std::max(0.0, ec - e_b0) / dmax)));
        LB_ = std::max(prm.L_B, nB_ * L_STAND);
        eB_.assign(static_cast<size_t>(nB_), 0.);
        for (int k = 0; k < nB_; ++k) eB_[size_t(k)] = ec - (ec - e_b0) * (k + 1) / nB_;
        std::vector<double> xs; xs.reserve(eB_.size());
        for (double e : eB_) xs.push_back((e - A) * LB_ / nB_);
        return py_sum(xs) * eta;
    };
    extra_B = down(e_c, nB, LB, eB);
    const double La = std::max(0.0, (v * v - v_c * v_c) / (2 * a2)), Lr = std::max(La, L_b);
    const int    n  = std::max(1, int(std::ceil(Lr / 0.25)));
    const double dl = Lr / n;
    struct Rel { double L, e, vk; };
    std::vector<Rel> rel;
    double prev = eB.back();
    for (int k = 0; k < n; ++k) {
        double vk  = La > 0 ? std::sqrt(v_c * v_c + 2 * a2 * std::min(La, (k + 1) * dl)) : v;
        double tgt = A * (1 + b * std::max(0.0, 1 - (k + 0.5) * dl / L_b));
        double lim = ICV_FRAC * ICV / vk * 0.98;
        double e   = tgt < prev ? std::max(tgt, prev - lim) : std::min(tgt, prev + lim);
        e = std::min(e, Q_MAX / (eta * vk));
        rel.push_back({ dl, e, vk });
        prev = e;
    }
    while (std::abs(prev - A) > 1e-6) {
        prev = std::max(A, prev - ICV_FRAC * ICV / v * 0.98);
        rel.push_back({ 0.5, prev, v });
    }
    double extra_rel;
    {
        std::vector<double> xs; xs.reserve(rel.size());
        for (const Rel &r : rel) xs.push_back((r.e - A) * r.L);
        extra_rel = py_sum(xs) * eta;
    }
    const double X   = std::max(0.0, prm.f * G1 - x_s);
    bool fit = false;
    if (co.fit_total && X < extra_B + extra_rel - 1e-9) {
        // law_general fit_total: no room for a creep, the step-down starts lower so that its volume is what is left of f G1
        double lo_ = e_b0, hi_ = e_c;
        int nB_t; double LB_t; std::vector<double> eB_t;
        for (int it = 0; it < 60; ++it) {
            const double mid = 0.5 * (lo_ + hi_);
            if (down(mid, nB_t, LB_t, eB_t) + extra_rel > X) hi_ = mid;
            else                                              lo_ = mid;
        }
        e_c = lo_;
        extra_B = down(e_c, nB, LB, eB);
        fit = true;
    }
    const double L_c = e_c > A ? std::max(0.0, (X - extra_B - extra_rel) / ((e_c - A) * eta)) : 0.0;
    if (L_c >= L_STAND) segs.push_back({ L_c, e_c * L_c, L_c / v_c, SegKind::Creep, PRINT_ACC });
    for (double e : eB) segs.push_back({ LB / nB, e * LB / nB, LB / nB / v_c, SegKind::Down, PRINT_ACC });
    for (const Rel &r : rel) segs.push_back({ r.L, r.e * r.L, r.L / r.vk, SegKind::Acc, a2 });

    const double g = tail ? prm.g : 0.0;
    if (g * D2 > 0.5) {
        const double dts = 0.1;
        double t = 0.0;
        std::vector<double> want;
        while (t < 3 * tau2 - 1e-9) {
            want.push_back(g * D2 * (std::exp(-t / tau2) - std::exp(-(t + dts) / tau2)) / eta / (v * dts));
            t += dts;
        }
        const double dx = ICV_FRAC * ICV / v * 0.95;
        std::vector<double> x; x.reserve(want.size());
        double prv = 0.0;
        for (double w : want) {
            prv = w > prv ? std::min(w, prv + dx) : std::max(w, prv - dx);
            x.push_back(prv);
        }
        const double short_ = py_sum(want) - py_sum(x);
        std::vector<char> ok(want.size());
        std::vector<double> ok_w;
        for (size_t i = 0; i < want.size(); ++i) {
            ok[i] = x[i] >= want[i] - 1e-12;
            if (ok[i]) ok_w.push_back(want[i]);
        }
        const double tot_ok = py_sum(ok_w);
        if (short_ > 0 && tot_ok > 0)
            for (size_t i = 0; i < x.size(); ++i) x[i] = x[i] + (ok[i] ? short_ * want[i] / tot_ok : 0.0);
        const double cap = std::max(0.0, Q_MAX / (eta * v) - A);
        double carry = 0.0;
        std::vector<double> xs_;
        for (double xk : x) {
            xk += carry;
            carry = std::max(0.0, xk - cap);
            xs_.push_back(std::min(xk, cap));
        }
        while (carry > 1e-6) {
            double xk = std::min(carry, cap);
            xs_.push_back(xk);
            carry -= xk;
            if (cap <= 0.) break;   // cannot happen with Q_MAX > eta A v; avoids an endless loop otherwise
        }
        double L0 = seg_length(segs), prev_e = A;
        for (double xk : xs_) {
            if (tail_max_L >= 0 && L0 + v * dts > tail_max_L + 1e-9) break;
            segs.push_back({ v * dts, (A + xk) * v * dts, dts, SegKind::Tail, PRINT_ACC });
            L0 += v * dts;
            prev_e = A + xk;
        }
        double e = prev_e;
        while (e - A > ICV_FRAC * ICV / v * 0.95) {   // cut in the middle: step down within the ICV rule
            e = std::max(A, e - ICV_FRAC * ICV / v * 0.95);
            segs.push_back({ 0.5, e * 0.5, 0.5 / v, SegKind::Tail, PRINT_ACC });
        }
    }
    if (info != nullptr) {
        info->x_s = x_s; info->x_s_law = prm.x_s; info->x_s_by = x_s_by; info->e_c = e_c; info->L_c = L_c; info->fit = fit;
        std::vector<double> xs;
        for (const Segment &s : segs) if (s.kind != SegKind::Tail) xs.push_back(s.E - A * s.L);
        info->inj = py_sum(xs) * eta;
    }
    return segs;
}

// ===================================================================================== checks / utilities

static std::string py_repr(double x);

std::vector<std::string> check(const std::vector<Segment> &segs, double v, const LawParams &lp)
{
    const MachineParams &m = lp.mach;
    std::vector<std::string> out;
    std::vector<const Segment*> mv;
    for (const Segment &s : segs)
        if (s.L >= m.l_stand) mv.push_back(&s);
    for (size_t i = 0; i + 1 < mv.size(); ++i) {
        const Segment &a = *mv[i], &c = *mv[i + 1];
        double vj = std::min(std::min(a.L / a.dt, c.L / c.dt), v);
        double de = std::abs(c.E / c.L - a.E / a.L);
        if (de * vj > m.icv_frac * m.icv + 1e-6)
            out.push_back(strfmt("E/mm step %.3f at %.1f mm/s (%s->%s)", de, vj, seg_kind_name(a.kind), seg_kind_name(c.kind)));
    }
    for (const Segment &s : segs) {
        if (s.E < -1e-9 || s.dt <= 0)
            out.push_back("bad segment {'L': " + py_repr(s.L) + ", 'E': " + py_repr(s.E) + ", 'dt': " + py_repr(s.dt) +
                          ", 'kind': '" + seg_kind_name(s.kind) + "', 'acc': " + py_repr(s.acc) + "}");
        if (s.dt > 0 && s.E / s.dt * m.eta > m.q_max * 1.001)
            out.push_back(strfmt("flow %.0f > Q_MAX (%s)", s.E / s.dt * m.eta, seg_kind_name(s.kind)));
    }
    return out;
}

std::vector<Segment> merge_standing(const std::vector<Segment> &segs, double l_stand)
{
    std::vector<Segment> out;
    bool pend = false;
    for (const Segment &s : segs) {
        if (s.L < l_stand) {
            if (pend) {
                Segment &p = out.back();
                p.L += s.L; p.E += s.E; p.dt += s.dt;
            } else {
                out.push_back(s);
                pend = true;
            }
            continue;
        }
        pend = false;
        out.push_back(s);
    }
    return out;
}

std::vector<Segment> truncate(const std::vector<Segment> &segs, double L_max, double l_stand)
{
    std::vector<Segment> out;
    double acc = 0.0;
    for (const Segment &q : segs) {
        if (acc + q.L <= L_max + 1e-9) { out.push_back(q); acc += q.L; continue; }
        double f = q.L > 0 ? (L_max - acc) / q.L : 0.0;
        if (f * q.L >= l_stand) {
            Segment c = q;
            c.L = q.L * f; c.E = q.E * f; c.dt = q.dt * f;
            out.push_back(c);
        }
        break;
    }
    return out;
}

// ===================================================================================== short-run policy B

PlannedRun plan_run(double v, double A, double L, double G1, double D2, const LawParams &lp, bool wall,
                    const std::vector<Segment> *end_in)
{
    const double eta = lp.mach.eta;
    PlannedRun pr;
    pr.end   = end_in != nullptr ? *end_in : design_end(v, A, lp, nullptr, lp.knobs.k_e(v, A, eta, wall));
    pr.L_end = seg_length(pr.end);
    const RestartParams prm  = restart_params(v, A, lp);
    std::vector<Segment> fast = design_restart(v, A, G1, D2, lp, false, -1., &prm, &pr.rinfo);
    pr.L_fast = seg_length(fast);
    if (L >= pr.L_fast + pr.L_end) {
        pr.plan    = RunPlan::Full;
        pr.restart = design_restart(v, A, G1, D2, lp, true, L - pr.L_end, &prm);
        return pr;
    }
    // Truncated short runs: restart knob k_r_trunc (if >= 0); L_fast is computed with that knob.
    if (lp.short_runs.k_r_trunc >= 0) {
        const RestartParams prm_t = restart_params(v, A, lp, lp.short_runs.k_r_trunc);
        fast      = design_restart(v, A, G1, D2, lp, false, -1., &prm_t, &pr.rinfo);
        pr.L_fast = seg_length(fast);
    }
    if (L >= pr.L_fast) {
        pr.plan    = RunPlan::FastEndTrunc;
        pr.restart = std::move(fast);
        pr.end     = truncate(pr.end, L - pr.L_fast, lp.mach.l_stand);
        return pr;
    }
    pr.plan    = RunPlan::FastTrunc;
    pr.restart = truncate(fast, L, lp.mach.l_stand);
    pr.end.clear();
    return pr;
}

// ===================================================================================== self-test

// Python's round(x, 6): correctly rounded decimal rounding of the binary value.
static double round6(double x)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "%.6f", x);
    return std::strtod(buf, nullptr);
}

// Python's float repr (shortest round-trip, 'e' notation for exp < -4 or >= 16), as json.dumps prints floats.
static std::string py_repr(double x)
{
    if (std::isnan(x)) return "NaN";
    if (std::isinf(x)) return x > 0 ? "Infinity" : "-Infinity";
    if (x == 0.) return std::signbit(x) ? "-0.0" : "0.0";
    char buf[64];
    for (int p = 1; p <= 17; ++p) {
        snprintf(buf, sizeof(buf), "%.*e", p - 1, x);
        if (std::strtod(buf, nullptr) == x) break;
    }
    std::string s(buf);
    size_t epos = s.find('e');
    int exp10 = std::atoi(s.c_str() + epos + 1);
    std::string mant = s.substr(0, epos);
    bool neg = !mant.empty() && mant[0] == '-';
    std::string digits;
    for (char ch : mant)
        if (ch >= '0' && ch <= '9') digits += ch;
    while (digits.size() > 1 && digits.back() == '0') digits.pop_back();
    std::string r;
    if (exp10 < -4 || exp10 >= 16) {
        r = digits.substr(0, 1);
        if (digits.size() > 1) r += "." + digits.substr(1);
        r += strfmt("e%c%02d", exp10 < 0 ? '-' : '+', std::abs(exp10));
    } else if (exp10 < 0) {
        r = "0." + std::string(size_t(-exp10 - 1), '0') + digits;
    } else {
        size_t intlen = size_t(exp10 + 1);
        if (digits.size() <= intlen) r = digits + std::string(intlen - digits.size(), '0') + ".0";
        else                         r = digits.substr(0, intlen) + "." + digits.substr(intlen);
    }
    return neg ? "-" + r : r;
}

static std::string r6(double x) { return py_repr(round6(x)); }

static std::string json_str(const std::string &s)
{
    std::string r = "\"";
    for (char ch : s) {
        if (ch == '"' || ch == '\\') r += '\\';
        r += ch;
    }
    return r + "\"";
}

// Python repr of a list of str (single quotes; the messages never contain quotes).
static std::string py_list_repr(const std::vector<std::string> &v, size_t max_n = size_t(-1))
{
    std::string r = "[";
    for (size_t i = 0; i < v.size() && i < max_n; ++i) r += (i ? ", '" : "'") + v[i] + "'";
    return r + "]";
}

static std::string json_list(const std::vector<std::string> &v)
{
    std::string r = "[";
    for (size_t i = 0; i < v.size(); ++i) r += (i ? ", " : "") + json_str(v[i]);
    return r + "]";
}

static std::string bead_name(int v, int A) { return std::to_string(v) + "x" + std::to_string(A); }

struct Summary {
    size_t n = 0;
    double L = 0., E = 0., T = 0.;
    struct Kind { SegKind kind; size_t n; double L, E; };
    std::vector<Kind> kinds;   // insertion order of the first occurrence (Python dict)
};

// make_law_json.summ (values NOT rounded here)
static Summary summ(const std::vector<Segment> &segs)
{
    Summary s;
    s.n = segs.size();
    std::vector<double> Ls, Es, Ts;
    for (const Segment &q : segs) {
        Ls.push_back(q.L); Es.push_back(q.E); Ts.push_back(q.dt);
        auto it = std::find_if(s.kinds.begin(), s.kinds.end(), [&q](const Summary::Kind &k) { return k.kind == q.kind; });
        if (it == s.kinds.end()) { s.kinds.push_back({ q.kind, 0, 0., 0. }); it = s.kinds.end() - 1; }
        ++it->n; it->L += q.L; it->E += q.E;   // plain += as in the reference
    }
    s.L = py_sum(Ls); s.E = py_sum(Es); s.T = py_sum(Ts);
    return s;
}

// kinds as a Python dict repr (quote = '\'') or as JSON (quote = '"')
static std::string kinds_repr(const Summary &s, char quote)
{
    std::string r = "{";
    for (size_t i = 0; i < s.kinds.size(); ++i) {
        const Summary::Kind &k = s.kinds[i];
        r += (i ? ", " : "") + std::string(1, quote) + seg_kind_name(k.kind) + std::string(1, quote) + ": [" +
             std::to_string(k.n) + ", " + r6(k.L) + ", " + r6(k.E) + "]";
    }
    return r + "}";
}

// json.dumps(summ(segs)) body without braces: "n": .., "L": .., "E": .., "T": .., "kinds": {..}
static std::string summ_json(const Summary &s)
{
    return "\"n\": " + std::to_string(s.n) + ", \"L\": " + r6(s.L) + ", \"E\": " + r6(s.E) + ", \"T\": " + r6(s.T) +
           ", \"kinds\": " + kinds_repr(s, '"');
}

// legacy self-test: selftest_ref_tuned_dry.txt (make_law_json.py build_tuned), law JSON without the law_general keys
static int self_test_tuned(const LawParams &lp, FILE *out)
{
    const double eta = lp.mach.eta;
    bool ok = true;
    std::vector<std::string> lines;

    // ---- intermediate
    {
        RestartParams p = restart_params(40, 4, lp);
        ModelState ss = steady_state(lp.model, eta * 160);
        std::string s = "intermediate: {\"restart_params_40x4\": {";
        s += "\"Q\": " + r6(p.Q) + ", \"x_s\": " + r6(p.x_s) + ", \"v_c\": " + r6(p.v_c) + ", \"f\": " + r6(p.f) +
             ", \"L_B\": " + r6(p.L_B) + ", \"a2\": " + r6(p.a2) + ", \"b\": " + r6(p.b) + ", \"L_b\": " + r6(p.L_b) +
             ", \"g\": " + r6(p.g) + "}";
        s += ", \"steady_state_157.44\": {\"V\": " + r6(ss.V) + ", \"V2\": " + r6(ss.V2) + ", \"q\": " + r6(ss.q) + "}";
        auto knob_block = [&](const Knobs &kn, const std::vector<std::pair<int, int>> &beads) {
            std::string b = "{";
            for (size_t i = 0; i < beads.size(); ++i) {
                int v = beads[i].first, A = beads[i].second;
                b += (i ? ", \"" : "\"") + bead_name(v, A) + "\": [" + r6(kn.k_r(v, A, eta)) + ", " + r6(kn.k_e(v, A, eta)) + "]";
            }
            return b + "}";
        };
        s += ", \"knobs_k_r_k_e\": " + knob_block(lp.knobs, { { 20, 4 }, { 100, 4 }, { 85, 6 } });
        Knobs ks = lp.knobs;
        ks.kr_1 = -0.2; ks.ke_1 = 0.1;
        s += ", \"knobs_slope_test kr_1=-0.2 ke_1=0.1\": " + knob_block(ks, { { 20, 4 }, { 100, 4 }, { 85, 6 }, { 5, 1 } });
        s += "}";
        lines.push_back(s);
    }

    // ---- END and RESTART of 7 beads x 4 stops
    const std::pair<int, int> beads[] = { { 20, 4 }, { 40, 4 }, { 60, 4 }, { 100, 4 }, { 85, 6 }, { 45, 7 }, { 50, 5 } };
    const double t_stops[] = { 3.3 + 0.75 + 0.3, 3.3 + 3.0 + 0.3, 3.3 + 8.0 + 0.3, 3.3 + 30.0 + 0.3 };
    for (const auto &bd : beads) {
        const int v = bd.first, A = bd.second;
        std::vector<Segment> end = design_end(v, A, lp);
        std::vector<std::string> bad = check(end, v, lp);
        ok &= bad.empty();
        std::vector<Segment> co, ta;
        for (const Segment &s : end) (s.kind == SegKind::Coast ? co : ta).push_back(s);
        double v_coast_first = co.empty() ? 0. : co.front().L / co.front().dt;
        double v_last        = end.empty() ? 0. : end.back().L / end.back().dt;
        Summary se = summ(end);
        lines.push_back(strfmt("END %s: n %zu taper %zu L %.4f E %.4f T %.4f Ltaper %.4f Lcoast %.4f v_coast0 %.3f v_last %.3f ",
                               bead_name(v, A).c_str(), end.size(), ta.size(), round6(se.L), round6(se.E), round6(se.T),
                               round6(seg_length(ta)), round6(seg_length(co)), round6(v_coast_first), round6(v_last)) +
                        (bad.empty() ? std::string("OK") : py_list_repr(bad, 2)));
        for (double t_stop : t_stops) {
            ModelState st = state_after_stop(lp, eta * A * v, t_stop);
            double G1, D2;
            deficits(lp.model, eta * A * v, st, G1, D2);
            std::vector<Segment> segs = design_restart(v, A, G1, D2, lp);
            std::vector<std::string> rbad = check(segs, v, lp);
            ok &= rbad.empty();   // guardrail: a RESTART that does not pass check() is a failure
            Summary sr = summ(segs);
            std::vector<Segment> nontail;
            for (const Segment &s : segs) if (s.kind != SegKind::Tail) nontail.push_back(s);
            lines.push_back(strfmt("RESTART %s t_stop %5.2f: G1 %.4f D2 %.4f n %zu L %.4f E %.4f T %.4f L_fast %.3f kinds ",
                                   bead_name(v, A).c_str(), t_stop, G1, D2, sr.n, round6(sr.L), round6(sr.E), round6(sr.T),
                                   round6(seg_length(nontail))) +
                            kinds_repr(sr, '\'') + " " + (rbad.empty() ? std::string("OK") : py_list_repr(rbad, 2)));
        }
    }

    // ---- EXTRA
    {
        std::vector<Segment> segs = design_restart(40, 4, 143.6363, 30.4876, lp, true, 120);
        lines.push_back("EXTRA {\"case\": \"design_restart(40,4,143.6363,30.4876,tail_max_L=120)\", " + summ_json(summ(segs)) + "}");
    }
    {
        ModelState st = state_after_stop(lp, 157.44, 2.0);
        std::vector<Segment> e2 = design_end(40, 4, lp, &st);
        lines.push_back("EXTRA {\"case\": \"design_end(40,4,st=state_after_stop(157.44,2.0))\", \"st\": {\"V\": " + r6(st.V) +
                        ", \"V2\": " + r6(st.V2) + ", \"q\": " + r6(st.q) + "}, " + summ_json(summ(e2)) + "}");
    }
    for (double kr : { 0.46, 0.60, 1.0 }) {
        RestartParams prm = restart_params(85, 6, lp, kr);
        std::vector<Segment> segs = design_restart(85, 6, 250.0, 90.0, lp, false, -1., &prm);
        lines.push_back("EXTRA {\"case\": \"design_restart(85,6,G1=250,D2=90,k_r=" + py_repr(kr) + ",tail=False)\", \"x_s\": " + r6(prm.x_s) +
                        ", \"f\": " + r6(prm.f) + ", \"b\": " + r6(prm.b) + ", " + summ_json(summ(segs)) + "}");
        // Guardrail (not in the reference, printed only on failure): every RESTART must pass check().
        std::vector<std::string> rbad = check(segs, 85, lp);
        if (!rbad.empty()) {
            ok = false;
            lines.push_back("GUARDRAIL EXTRA k_r=" + py_repr(kr) + ": " + py_list_repr(rbad, 4));
        }
    }
    for (double ke : { 0.6, 1.0, 1.3 }) {
        std::vector<Segment> e3 = design_end(85, 6, lp, nullptr, ke);
        lines.push_back("EXTRA {\"case\": \"design_end(85,6,k_c=" + py_repr(ke) + ")\", " + summ_json(summ(e3)) +
                        ", \"check\": " + json_list(check(e3, 85, lp)) + "}");
    }

    // ---- POLICY_B
    struct PolCase { int v, A; std::vector<int> Ls; };
    const PolCase pol_cases[] = { { 40, 4, { 10, 25, 40, 60, 200 } }, { 80, 4, { 15, 45, 60, 90, 400 } }, { 100, 4, { 60, 120, 450 } } };
    for (const PolCase &pc : pol_cases) {
        ModelState st = state_after_stop(lp, eta * pc.A * pc.v, 6.6);
        double G1, D2;
        deficits(lp.model, eta * pc.A * pc.v, st, G1, D2);
        for (int L : pc.Ls) {
            PlannedRun pr = plan_run(pc.v, pc.A, L, G1, D2, lp);
            Summary rs = summ(pr.restart);
            Summary en = summ(pr.end);
            double used = round6(seg_length(pr.restart) + seg_length(pr.end));
            lines.push_back(strfmt("POLICY_B %s L %d: %s L_fast %.3f L_end %.3f restart n %zu L %.3f E %.3f | end n %zu L %.3f | used %.3f",
                                   bead_name(pc.v, pc.A).c_str(), L, run_plan_name(pr.plan), pr.L_fast, pr.L_end, rs.n, round6(rs.L),
                                   round6(rs.E), pr.end.empty() ? size_t(0) : en.n, pr.end.empty() ? 0. : round6(en.L), used));
            // Guardrail (not in the reference, printed only on failure): the plan must pass check() and fit the run.
            std::vector<std::string> pbad = check(pr.restart, pc.v, lp);
            std::vector<std::string> ebad = check(pr.end, pc.v, lp);
            pbad.insert(pbad.end(), ebad.begin(), ebad.end());
            if (seg_length(pr.restart) + seg_length(pr.end) > L + 1e-6)
                pbad.push_back(strfmt("plan longer than the run (%.6f > %d)", seg_length(pr.restart) + seg_length(pr.end), L));
            if (!pbad.empty()) {
                ok = false;
                lines.push_back("GUARDRAIL POLICY_B " + bead_name(pc.v, pc.A) + " L " + std::to_string(L) + ": " + py_list_repr(pbad, 4));
            }
        }
    }

    lines.push_back(std::string("SELF-TEST ") + (ok ? "OK" : "FAILED"));
    if (out != nullptr) {
        for (const std::string &l : lines) { fputs(l.c_str(), out); fputc('\n', out); }
        fflush(out);
    }
    return ok ? 0 : 1;
}

// law_general self-test: selftest_ref_general.txt (make_law_json.py build_general, law_general.py), same order and format
static int self_test_general(const LawParams &lp, FILE *out)
{
    const double eta = lp.mach.eta;
    const LawCoef &c = lp.coef;
    bool ok = true;
    std::vector<std::string> lines;

    // ---- intermediate
    {
        RestartParams p = restart_params(40, 4, lp);
        ModelState ss = steady_state(lp.model, eta * 160);
        std::string s = "intermediate: {\"restart_params_40x4\": {";
        s += "\"Q\": " + r6(p.Q) + ", \"x_s\": " + r6(p.x_s) + ", \"v_c\": " + r6(p.v_c) + ", \"f\": " + r6(p.f) +
             ", \"L_B\": " + r6(p.L_B) + ", \"a2\": " + r6(p.a2) + ", \"b\": " + r6(p.b) + ", \"L_b\": " + r6(p.L_b) +
             ", \"g\": " + r6(p.g) + "}";
        s += ", \"steady_state_157.44\": {\"V\": " + r6(ss.V) + ", \"V2\": " + r6(ss.V2) + ", \"q\": " + r6(ss.q) + "}";
        auto knob_block = [&](const Knobs &kn, const std::vector<std::pair<int, int>> &beads) {
            std::string b = "{";
            for (size_t i = 0; i < beads.size(); ++i) {
                int v = beads[i].first, A = beads[i].second;
                b += (i ? ", \"" : "\"") + bead_name(v, A) + "\": [" + r6(kn.k_r(v, A, eta)) + ", " + r6(kn.k_e(v, A, eta)) + "]";
            }
            return b + "}";
        };
        s += ", \"knobs_k_r_k_e\": " + knob_block(lp.knobs, { { 20, 4 }, { 100, 4 }, { 85, 6 } });
        Knobs ks = lp.knobs;
        ks.kr_1 = -0.2; ks.ke_1 = 0.1;
        s += ", \"knobs_slope_test kr_1=-0.2 ke_1=0.1\": " + knob_block(ks, { { 20, 4 }, { 100, 4 }, { 85, 6 }, { 5, 1 } });
        s += ", \"ke_wall\": {";
        const std::pair<int, int> wb[] = { { 4, 4 }, { 10, 4 }, { 40, 4 }, { 80, 4 }, { 100, 4 } };
        for (size_t i = 0; i < 5; ++i)
            s += (i ? ", \"" : "\"") + bead_name(wb[i].first, wb[i].second) + "\": " + r6(lp.knobs.k_e(wb[i].first, wb[i].second, eta, true));
        s += "}, \"caps\": {\"anchor_cap_A\": " + r6(c.anchor_cap_A) + ", \"anchor_max_cmd\": " + r6(c.anchor_max_cmd) +
             ", \"creep_cap_A\": " + r6(c.creep_cap_A) + ", \"fit_total\": " + r6(c.fit_total ? 1. : 0.) +
             ", \"coast_floor_rel\": " + r6(c.coast_floor_rel) + ", \"end_coarse_v\": " + r6(c.end_coarse_v) +
             ", \"end_seg_min\": " + r6(c.end_seg_min) + "}, \"v_floor_eff\": {";
        const int vf[] = { 1, 2, 3, 4, 5, 20 };
        for (size_t i = 0; i < 6; ++i) s += (i ? ", \"" : "\"") + std::to_string(vf[i]) + "\": " + r6(v_floor_eff(vf[i], lp));
        s += "}}";
        lines.push_back(s);
    }

    // ---- END (+ END_WALL) lines
    auto end_line = [&](const char *tag, int v, int A, bool wall) {
        EndInfo info;
        std::vector<Segment> end = design_end(v, A, lp, nullptr, lp.knobs.k_e(v, A, eta, wall), &info);
        std::vector<std::string> bad = check(end, v, lp);
        ok &= bad.empty();
        std::vector<Segment> co, ta;
        for (const Segment &s : end) (s.kind == SegKind::Coast ? co : ta).push_back(s);
        double v_coast_first = co.empty() ? 0. : co.front().L / co.front().dt;
        double v_last        = end.empty() ? 0. : end.back().L / end.back().dt;
        double min_move = end.empty() ? 0. : end.front().L;
        for (const Segment &s : end) min_move = std::min(min_move, s.L);
        Summary se = summ(end);
        lines.push_back(strfmt("%s %s: ", tag, bead_name(v, A).c_str()) + (wall ? strfmt("k_e %.6f ", info.k_c) : std::string()) +
                        strfmt("n %zu taper %zu L %.4f E %.4f T %.4f Ltaper %.4f Lcoast %.4f v_coast0 %.3f v_last %.3f stop %s "
                               "v_floor %.3f min_move %.4f ", end.size(), ta.size(), round6(se.L), round6(se.E), round6(se.T),
                               round6(seg_length(ta)), round6(seg_length(co)), round6(v_coast_first), round6(v_last), info.reason,
                               round6(info.v_floor), round6(min_move)) +
                        (bad.empty() ? std::string("OK") : py_list_repr(bad, 2)));
    };
    const std::pair<int, int> beads[] = { { 20, 4 }, { 40, 4 }, { 60, 4 }, { 100, 4 }, { 85, 6 }, { 45, 7 }, { 50, 5 },
                                          { 1, 4 }, { 2, 4 }, { 3, 4 }, { 5, 4 }, { 8, 4 }, { 12, 4 }, { 5, 6 }, { 8, 5 } };
    const std::vector<double> t_stops_hi { 3.3 + 0.75 + 0.3, 3.3 + 3.0 + 0.3, 3.3 + 8.0 + 0.3, 3.3 + 30.0 + 0.3 };
    const std::vector<double> t_stops_lo { 3.0, 6.6 };
    for (size_t bi = 0; bi < sizeof(beads) / sizeof(beads[0]); ++bi) {
        const int v = beads[bi].first, A = beads[bi].second;
        end_line("END", v, A, false);
        for (double t_stop : (bi < 7 ? t_stops_hi : t_stops_lo)) {
            ModelState st = state_after_stop(lp, eta * A * v, t_stop);
            double G1, D2;
            deficits(lp.model, eta * A * v, st, G1, D2);
            RestartInfo ri;
            std::vector<Segment> segs = design_restart(v, A, G1, D2, lp, true, -1., nullptr, &ri);
            std::vector<std::string> rbad = check(segs, v, lp);
            ok &= rbad.empty();
            Summary sr = summ(segs);
            std::vector<Segment> nontail;
            for (const Segment &s : segs) if (s.kind != SegKind::Tail) nontail.push_back(s);
            lines.push_back(strfmt("RESTART %s t_stop %5.2f: G1 %.4f D2 %.4f n %zu L %.4f E %.4f T %.4f L_fast %.3f kinds ",
                                   bead_name(v, A).c_str(), t_stop, G1, D2, sr.n, round6(sr.L), round6(sr.E), round6(sr.T),
                                   round6(seg_length(nontail))) +
                            kinds_repr(sr, '\'') + strfmt(" x_s %.4f by %s e_c %.4f fit %d inj %.4f ", ri.x_s, ri.x_s_by, ri.e_c,
                                                          ri.fit ? 1 : 0, ri.inj) +
                            (rbad.empty() ? std::string("OK") : py_list_repr(rbad, 2)));
        }
    }
    for (const auto &wbd : { std::make_pair(40, 4), std::make_pair(80, 4), std::make_pair(10, 4), std::make_pair(4, 4) })
        end_line("END_WALL", wbd.first, wbd.second, true);

    // ---- EXTRA
    {
        std::vector<Segment> segs = design_restart(40, 4, 143.6363, 30.4876, lp, true, 120);
        lines.push_back("EXTRA {\"case\": \"design_restart(40,4,143.6363,30.4876,tail_max_L=120)\", " + summ_json(summ(segs)) + "}");
    }
    {
        ModelState st = state_after_stop(lp, 157.44, 2.0);
        std::vector<Segment> e2 = design_end(40, 4, lp, &st);
        lines.push_back("EXTRA {\"case\": \"design_end(40,4,st=state_after_stop(157.44,2.0))\", \"st\": {\"V\": " + r6(st.V) +
                        ", \"V2\": " + r6(st.V2) + ", \"q\": " + r6(st.q) + "}, " + summ_json(summ(e2)) + "}");
    }
    for (double kr : { 0.46, 0.60, 1.0 }) {
        RestartParams prm = restart_params(85, 6, lp, kr);
        std::vector<Segment> segs = design_restart(85, 6, 250.0, 90.0, lp, false, -1., &prm);
        lines.push_back("EXTRA {\"case\": \"design_restart(85,6,G1=250,D2=90,k_r=" + py_repr(kr) + ",tail=False)\", \"x_s\": " + r6(prm.x_s) +
                        ", \"f\": " + r6(prm.f) + ", \"b\": " + r6(prm.b) + ", " + summ_json(summ(segs)) + "}");
        std::vector<std::string> rbad = check(segs, 85, lp);
        if (!rbad.empty()) {
            ok = false;
            lines.push_back("GUARDRAIL EXTRA k_r=" + py_repr(kr) + ": " + py_list_repr(rbad, 4));
        }
    }
    for (double ke : { 0.6, 1.0, 1.3 }) {
        std::vector<Segment> e3 = design_end(85, 6, lp, nullptr, ke);
        lines.push_back("EXTRA {\"case\": \"design_end(85,6,k_c=" + py_repr(ke) + ")\", " + summ_json(summ(e3)) +
                        ", \"check\": " + json_list(check(e3, 85, lp)) + "}");
    }
    {
        RestartInfo ri;
        std::vector<Segment> segs = design_restart(5, 6, 150.0, 10.0, lp, true, -1., nullptr, &ri);
        lines.push_back("EXTRA {\"case\": \"design_restart(5,6,G1=150,D2=10)\", \"x_s\": " + r6(ri.x_s) + ", \"x_s_by\": " +
                        json_str(ri.x_s_by) + ", " + summ_json(summ(segs)) + "}");
        ModelState st = state_after_stop(lp, eta * 8, 1.0);
        std::vector<Segment> e4 = design_end(2, 4, lp, &st);
        lines.push_back("EXTRA {\"case\": \"design_end(2,4,st=state_after_stop(7.872,1.0))\", " + summ_json(summ(e4)) +
                        ", \"check\": " + json_list(check(e4, 2, lp)) + "}");
    }

    // ---- POLICY_B (also at low speed and on walls)
    struct PolCase { int v, A; std::vector<int> Ls; bool wall; };
    const PolCase pol_cases[] = { { 40, 4, { 10, 25, 40, 60, 200 }, false }, { 80, 4, { 15, 45, 60, 90, 400 }, false },
                                  { 100, 4, { 60, 120, 450 }, false }, { 1, 4, { 1, 3, 8, 30 }, false }, { 2, 4, { 1, 4, 10, 40 }, false },
                                  { 5, 4, { 2, 6, 12, 60 }, false }, { 8, 5, { 3, 10, 20, 80 }, false }, { 12, 4, { 4, 12, 25, 100 }, false },
                                  { 40, 4, { 25, 40, 200 }, true }, { 5, 4, { 6, 12, 60 }, true } };
    for (const PolCase &pc : pol_cases) {
        ModelState st = state_after_stop(lp, eta * pc.A * pc.v, 6.6);
        double G1, D2;
        deficits(lp.model, eta * pc.A * pc.v, st, G1, D2);
        for (int L : pc.Ls) {
            PlannedRun pr = plan_run(pc.v, pc.A, L, G1, D2, lp, pc.wall);
            Summary rs = summ(pr.restart);
            Summary en = summ(pr.end);
            double used = round6(seg_length(pr.restart) + seg_length(pr.end));
            lines.push_back(strfmt("POLICY_B %s%s L %d: %s L_fast %.3f L_end %.3f restart n %zu L %.3f E %.3f | end n %zu L %.3f | used %.3f",
                                   bead_name(pc.v, pc.A).c_str(), pc.wall ? " wall" : "", L, run_plan_name(pr.plan), pr.L_fast, pr.L_end,
                                   rs.n, round6(rs.L), round6(rs.E), pr.end.empty() ? size_t(0) : en.n, pr.end.empty() ? 0. : round6(en.L),
                                   used));
            std::vector<std::string> pbad = check(pr.restart, pc.v, lp);
            std::vector<std::string> ebad = check(pr.end, pc.v, lp);
            pbad.insert(pbad.end(), ebad.begin(), ebad.end());
            if (seg_length(pr.restart) + seg_length(pr.end) > L + 1e-6)
                pbad.push_back(strfmt("plan longer than the run (%.6f > %d)", seg_length(pr.restart) + seg_length(pr.end), L));
            if (!pbad.empty()) {
                ok = false;
                lines.push_back("GUARDRAIL POLICY_B " + bead_name(pc.v, pc.A) + " L " + std::to_string(L) + ": " + py_list_repr(pbad, 4));
            }
        }
    }

    lines.push_back(std::string("SELF-TEST ") + (ok ? "OK" : "FAILED"));
    if (out != nullptr) {
        for (const std::string &l : lines) { fputs(l.c_str(), out); fputc('\n', out); }
        fflush(out);
    }
    return ok ? 0 : 1;
}

int self_test(const LawParams &lp, FILE *out)
{
    return lp.coef.general ? self_test_general(lp, out) : self_test_tuned(lp, out);
}

} // namespace ScrewLaw

// ===================================================================================== the stage (C1-C2)
//
// Design choices (MVP_DESIGN sec. 2.1, sec. 6):
// - Chunks are the layers of the CoolingBuffer. The output of a chunk never waits for the next one (GCode.cpp
//   has no flush call): runs are closed at the chunk end (a run that would continue across a boundary without a
//   stop is split there and counted, chunk_splits) and the annotations are emitted with their chunk.
//   The TIMING looks ahead across the boundary: the unsettled tail of a chunk (the moves whose junction speeds
//   still depend on what follows, i.e. the last deceleration distance) is not integrated in the model yet; it
//   is re-timed together with the next chunk (move_time). The timing and the model are therefore those of the
//   whole file planned at once; only a restart inside an unsettled tail (never at a layer change, where the
//   restart is in the next chunk) uses the provisional end-of-chunk = stop timing (counted, "provisional").
// - STOP = in the gap after an extrusion: XY travel (summed path) >= stop_travel_mm, or a Z move without
//   extrusion, or a G4, or a retraction (E < 0, also during a wipe). Extrusions with a Z change (scarf) are
//   extrusions. Short gaps stay inside the run.
// - Model: law_tuned.step on a fixed grid t = i * sim_dt over the whole print, commanded screw flow sampled
//   at the grid points from the trapezoid of the move (as kp2.timeline), delivered = eta * commanded,
//   negative flows (retractions, reverse) = screw stopped. Stops are just moves with zero screw flow.
// - First run: the state before START_PRINT is unknown. Assumed: steady at the Q of the first run, END taper,
//   then a stop of machine.first_stop_s (the START macro: purge, parking, ...) plus the G-code time measured
//   from the stage start (or from the last START_PRINT / *PURGE* line if the stage sees it) to the first
//   extrusion. Logged as an assumption.

namespace ScrewLaw {

const char *stage_mode_name(StageMode m)
{
    switch (m) {
    case StageMode::Emit:     return "emit";
    case StageMode::Dry:      return "dry";
    case StageMode::Annotate: return "annotate";
    }
    return "?";
}

const char *move_type_name(MoveType t)
{
    switch (t) {
    case MoveType::Extrude: return "extrude";
    case MoveType::Travel:  return "travel";
    case MoveType::ZMove:   return "zmove";
    case MoveType::EOnly:   return "eonly";
    case MoveType::Retract: return "retract";
    case MoveType::Dwell:   return "dwell";
    case MoveType::Accel:   return "accel";
    case MoveType::Other:   return "other";
    }
    return "?";
}

} // namespace ScrewLaw

using ScrewLaw::strfmt;

namespace {

using ScrewLaw::Move;
using ScrewLaw::MoveType;

inline bool sl_space(char c) { return c == ' ' || c == '\t'; }
inline char sl_upper(char c) { return (c >= 'a' && c <= 'z') ? char(c - 'a' + 'A') : c; }

// Number at p (fast_float, locale independent; '+' accepted); p advanced on success.
bool sl_num(const char *&p, const char *end, double &out)
{
    const char *q = p;
    if (q < end && *q == '+') ++q;
    auto r = fast_float::from_chars(q, end, out);
    if (r.ptr == q || r.ec != std::errc()) return false;
    p = r.ptr;
    return true;
}

// Classic G-code words "X10.5 Y.3 E-.1" after the command word; stops at ';'. mask bit = letter - 'A'.
struct Words {
    unsigned mask = 0;
    double   val[26] {};
    bool has(char c) const { return (mask >> (c - 'A')) & 1u; }
    double get(char c) const { return val[c - 'A']; }
};

void sl_words(const char *p, const char *end, Words &w)
{
    while (p < end) {
        char c = *p;
        if (c == ';') break;
        if (sl_space(c)) { ++p; continue; }
        c = sl_upper(c);
        ++p;
        if (c >= 'A' && c <= 'Z') {
            double v;
            if (sl_num(p, end, v)) { w.val[c - 'A'] = v; w.mask |= 1u << (c - 'A'); continue; }
        }
        while (p < end && !sl_space(*p) && *p != ';') ++p;   // skip a malformed word
    }
}

// Klipper extended parameter KEY=value (case-insensitive key) in [p, end); false if missing.
bool sl_param(const char *p, const char *end, const char *key, double &out)
{
    const size_t kl = std::strlen(key);
    while (p < end) {
        while (p < end && sl_space(*p)) ++p;
        if (p >= end || *p == ';') return false;
        const char *w = p;
        while (p < end && !sl_space(*p) && *p != '=' && *p != ';') ++p;
        bool match = size_t(p - w) == kl;
        for (size_t i = 0; match && i < kl; ++i) match = sl_upper(w[i]) == key[i];
        if (p < end && *p == '=') {
            ++p;
            const char *vbeg = p;
            while (p < end && !sl_space(*p) && *p != ';') ++p;
            if (match) {
                const char *q = vbeg;
                double v;
                if (sl_num(q, p, v)) { out = v; return true; }
                return false;
            }
        }
    }
    return false;
}

bool sl_starts(const char *p, const char *end, const char *pref)
{
    const size_t n = std::strlen(pref);
    return size_t(end - p) >= n && std::memcmp(p, pref, n) == 0;
}

// kp2.s_at speed: head speed along the move at tau seconds from its start.
inline double sl_speed_at(const Move &m, double tau)
{
    tau = std::min(std::max(tau, 0.0), m.ta + m.tc + m.td);
    if (tau <= m.ta) return m.v0 + m.a * tau;
    if (tau <= m.ta + m.tc) return m.vc;
    return m.vc - m.a * (tau - m.ta - m.tc);
}

// kplanner.junction(m0, m1, klipper=True)
double sl_junction(const Move &m0, const Move &m1, double e_icv)
{
    if (m0.type == MoveType::Dwell || m1.type == MoveType::Dwell) return 0.0;
    if (m0.eonly || m1.eonly) return 0.0;
    if (m0.arc || m1.arc) return 0.0;   // not in kplanner (no arcs there): conservative
    const double c = m0.dir[0] * m1.dir[0] + m0.dir[1] * m1.dir[1] + m0.dir[2] * m1.dir[2];
    double vj = std::min(m0.v, m1.v);
    const double jc = -c;
    const double sin_d2 = std::sqrt(std::max(0.5 * (1.0 - jc), 0.0)), cos_d2 = std::sqrt(std::max(0.5 * (1.0 + jc), 0.0));
    const double oms = 1.0 - sin_d2;
    if (oms > 0.0 && cos_d2 > 0.0) {
        const double R_jd = sin_d2 / oms, s2m1 = std::sqrt(2.0) - 1.0;
        const double v2 = std::min(std::min(R_jd * m1.scv * m1.scv * s2m1, R_jd * m0.scv * m0.scv * s2m1),
                                   std::min(2.0 * m1.L * m1.a * 0.25 * sin_d2 / cos_d2, 2.0 * m0.L * m0.a * 0.25 * sin_d2 / cos_d2));
        vj = std::min(vj, std::sqrt(std::max(0.0, v2)));
    }
    const double de = std::abs(m1.epm - m0.epm);
    if (de > 1e-9) vj = std::min(vj, e_icv / de);
    return vj;
}

} // namespace

#ifdef SCREWLAW_STANDALONE
#define SL_LOG_INFO(expr)  do { std::ostringstream sl_os_; sl_os_ << expr; fprintf(stderr, "[info] %s\n", sl_os_.str().c_str()); } while (0)
#define SL_LOG_WARN(expr)  do { std::ostringstream sl_os_; sl_os_ << expr; fprintf(stderr, "[warning] %s\n", sl_os_.str().c_str()); } while (0)
#define SL_LOG_ERROR(expr) do { std::ostringstream sl_os_; sl_os_ << expr; fprintf(stderr, "[error] %s\n", sl_os_.str().c_str()); } while (0)
#else
#define SL_LOG_INFO(expr)  BOOST_LOG_TRIVIAL(info) << expr
#define SL_LOG_WARN(expr)  BOOST_LOG_TRIVIAL(warning) << expr
#define SL_LOG_ERROR(expr) BOOST_LOG_TRIVIAL(error) << expr
#endif

// machine.stop_travel_mm of the law loaded by the last create (exports are sequential), read by GCode::needs_retraction
static double s_active_stop_travel_mm = 3.0;

bool PelletScrewLaw::enabled_from_env()
{
    static const bool on = [] {
        const char *p = std::getenv("GINGER_SCREW_LAW");
        return p != nullptr && *p != '\0';
    }();
    return on;
}

#ifndef SCREWLAW_STANDALONE
static bool env_flag(const char *name)
{
    const char *v = std::getenv(name);
    return v != nullptr && std::strcmp(v, "1") == 0;
}

bool PelletScrewLaw::enabled(const PrintConfig &config)
{
    return enabled_from_env() || (!config.screw_law_enable.values.empty() && config.screw_law_enable.get_at(0));
}

std::unique_ptr<PelletScrewLaw> PelletScrewLaw::create(const PrintConfig &config)
{
    if (!enabled(config))
        return nullptr;
    std::string path;
    ScrewLaw::LawParams lp;   // the parsers throw on any error
    if (enabled_from_env()) {
        path = std::getenv("GINGER_SCREW_LAW");
        lp = ScrewLaw::LawParams::from_json_file(path);
    } else {
        path = "filament settings (Screw law > Screw characterization)";
        const std::string text = config.screw_law_parameters.values.empty() ? std::string() : config.screw_law_parameters.get_at(0);
        if (text.find_first_not_of(" \t\r\n") == std::string::npos)
            throw Slic3r::RuntimeError("Pellet screw law: the screw characterization is empty (Filament > Screw law)");
        lp = ScrewLaw::LawParams::from_json_text(text, path);
    }
    if (!config.screw_law_restart_scale.values.empty()) lp.knobs.restart_scale = config.screw_law_restart_scale.get_at(0);
    if (!config.screw_law_coast_scale.values.empty())   lp.knobs.coast_scale   = config.screw_law_coast_scale.get_at(0);
    if (!(lp.knobs.restart_scale > 0.) || !(lp.knobs.coast_scale > 0.))
        throw Slic3r::RuntimeError("Pellet screw law: the restart / coasting scales must be > 0 (Filament > Screw law)");
    BOOST_LOG_TRIVIAL(info) << "[SCREWLAW] law loaded from " << path << " (" << lp.version << "): knobs k_r " << lp.knobs.kr_0
                            << (lp.knobs.kr_1 != 0. ? " (sloped)" : "") << ", k_e " << lp.knobs.ke_0 << (lp.knobs.ke_1 != 0. ? " (sloped)" : "")
                            << (lp.knobs.ke_wall_set ? ", ke_wall set" : ", ke_wall = k_e") << ", reverse " << lp.stop.reverse_mm3
                            << " mm3, k_r_trunc " << lp.short_runs.k_r_trunc << ", q_max " << lp.mach.q_max;
    if (lp.coef.general)
        BOOST_LOG_TRIVIAL(info) << "[SCREWLAW] law_general: anchor <= " << lp.coef.anchor_cap_A << " A, f G1, " << lp.coef.anchor_max_cmd
                                << " mm3 cmd; creep <= " << lp.coef.creep_cap_A << " A; fit_total " << lp.coef.fit_total
                                << "; coast floor min(" << lp.coef.v_floor << ", " << lp.coef.coast_floor_rel << " v); coarse END below "
                                << lp.coef.end_coarse_v << " mm/s (moves >= " << lp.coef.end_seg_min << " mm)"
                                << (lp.knobs.ke_wall_set ? strfmt("; ke_wall %g %+g ln(Q/%g) (valid 157-315 mm3/s, clamped outside)",
                                                                  lp.knobs.ke_wall_0, lp.knobs.ke_wall_1, lp.knobs.q_ref)
                                                         : std::string());
    if (!config.use_relative_e_distances.value)
        BOOST_LOG_TRIVIAL(warning) << "[SCREWLAW] use_relative_e_distances is off: the screw law requires relative E (M83)";

    if (env_flag("GINGER_SCREW_LAW_SELFTEST")) {
        const int rc = ScrewLaw::self_test(lp, stderr);
        if (rc != 0) {
            BOOST_LOG_TRIVIAL(error) << "[SCREWLAW] SELF-TEST FAILED (law " << path << ")";
            throw Slic3r::RuntimeError("Pellet screw law: SELF-TEST FAILED with " + path + " (see stderr)");
        }
        BOOST_LOG_TRIVIAL(info) << "[SCREWLAW] SELF-TEST OK";
    }

    ScrewLaw::StageConfig cfg;
    cfg.relative_e = config.use_relative_e_distances.value;
    s_active_stop_travel_mm = lp.mach.stop_travel_mm;
    // Settings that would make the law fail silently at the printer: errors before the export starts.
    {
        auto bad = [&](const std::string &msg) {
            throw Slic3r::RuntimeError("Pellet screw law: " + msg + " (or disable Filament > Screw law).");
        };
        if (config.spiral_mode.value)
            bad("spiral vase mode is not supported (one run across the layers would get an END on every layer): disable it");
        if (config.enable_prime_tower.value)
            bad("the prime tower is not supported (single screw): disable it");
        const double z_hop = config.z_hop.values.empty() ? 0. : config.z_hop.get_at(0);
        if (z_hop <= 0. || z_hop < lp.stop.exit_hop_mm)
            bad(strfmt("z_hop is %g mm: the slicer must lift at every stop, set z_hop >= %g mm (3 mm in the tests) in the printer "
                       "or as a filament override", z_hop, std::max(1., lp.stop.exit_hop_mm)));
        if (!config.retract_lift_above.values.empty() && config.retract_lift_above.get_at(0) > 0.)
            bad("retract_lift_above must be 0: the slicer must lift at every height");
        if (!config.retract_lift_below.values.empty() && config.retract_lift_below.get_at(0) > 0.)
            bad("retract_lift_below must be 0: the slicer must lift at every height");
        if (!config.retract_restart_extra.values.empty() && config.retract_restart_extra.get_at(0) != 0.)
            bad("retract_restart_extra must be 0: the extra length after a travel is a second injection on top of the law's anchor");
    }
    if (!config.printable_area.values.empty()) {   // bounding box of the printable area: the stop exit stays inside
        cfg.bed_min[0] = cfg.bed_min[1] = 1e9; cfg.bed_max[0] = cfg.bed_max[1] = -1e9;
        for (const Vec2d &pt : config.printable_area.values)
            for (int c = 0; c < 2; ++c) { cfg.bed_min[c] = std::min(cfg.bed_min[c], pt[c]); cfg.bed_max[c] = std::max(cfg.bed_max[c], pt[c]); }
    }
    // Planner limits: kplanner.LIMITS (the timing reference), see StageConfig. Z / extruder limits from the JSON.
    ScrewLaw::StageMode mode = ScrewLaw::StageMode::Emit;
    if (env_flag("GINGER_SCREW_LAW_DRY"))      mode = ScrewLaw::StageMode::Dry;
    if (env_flag("GINGER_SCREW_LAW_ANNOTATE")) mode = ScrewLaw::StageMode::Annotate;
    const bool dump = env_flag("GINGER_SCREW_LAW_DUMP");
    BOOST_LOG_TRIVIAL(info) << "[SCREWLAW] mode " << ScrewLaw::stage_mode_name(mode) << (dump ? ", dump" : "");
    fprintf(stderr, "[SCREWLAW] law %s (%s), mode %s%s\n", path.c_str(), lp.version.c_str(), ScrewLaw::stage_mode_name(mode),
            dump ? ", dump" : "");
    return std::make_unique<PelletScrewLaw>(std::move(lp), cfg, mode, dump);
}
#endif // SCREWLAW_STANDALONE

double PelletScrewLaw::active_stop_travel_mm() { return s_active_stop_travel_mm; }

PelletScrewLaw::PelletScrewLaw(ScrewLaw::LawParams lp, ScrewLaw::StageConfig cfg, ScrewLaw::StageMode mode, bool dump)
    : m_lp(std::move(lp)), m_cfg(cfg), m_mode(mode), m_dump(dump)
{
    m_base_restart_scale = m_lp.knobs.restart_scale;
    m_base_coast_scale   = m_lp.knobs.coast_scale;
    m_f       = m_cfg.init_f;
    m_accel   = std::min(m_lp.mach.travel_acc_default, m_cfg.max_accel);
    m_accel_raw = m_lp.mach.travel_acc_default;
    m_scv     = m_cfg.scv;
    m_rel_e   = m_cfg.relative_e;
    m_emit_f  = m_f;
    m_emit_accel = m_accel_raw;
    if (!m_rel_e && m_mode == ScrewLaw::StageMode::Emit)
        throw Slic3r::RuntimeError("Pellet screw law: absolute E distances are not supported: enable use_relative_e_distances (M83) in the printer profile");
    if (!m_rel_e) {
        m_passthrough = m_stats.absolute_e = true;
        SL_LOG_WARN("[SCREWLAW] absolute E (use_relative_e_distances off): the screw law needs relative E, the stage stays in pass-through");
    }
}

void PelletScrewLaw::log_line(const std::string &s) const
{
    SL_LOG_INFO("[SCREWLAW] " << s);
}

// kp2.parse: cruise limit v and acceleration a of a move (m.e, m.f, m.accel set by the caller).
void PelletScrewLaw::kinematics(ScrewLaw::Move &m, double dx, double dy, double dz, double L_xy, bool is_arc) const
{
    const double L = std::sqrt(L_xy * L_xy + dz * dz), e = m.e;
    m.plan = true;
    double v = m.f / 60.0;
    if (L > 1e-9) {
        double a = m.accel;
        v = std::min(v, m_cfg.max_velocity);
        if (std::abs(dz) > 1e-9) {
            v = std::min(v, m_lp.mach.z_max_v * L / std::abs(dz));
            a = std::min(a, m_lp.mach.z_max_a * L / std::abs(dz));
        }
        if (std::abs(e) > 1e-9) {
            v = std::min(v, m_cfg.e_max_v * L / std::abs(e));
            a = std::min(a, m_lp.mach.e_acc * L / std::abs(e));
        }
        m.L = L; m.L_xy = L_xy; m.v = v; m.a = a; m.epm = e / L;
        if (is_arc) { m.dir[0] = dx; m.dir[1] = dy; m.dir[2] = dz; const double c = std::sqrt(dx * dx + dy * dy + dz * dz); if (c > 0) for (double &d : m.dir) d /= c; }
        else { m.dir[0] = dx / L; m.dir[1] = dy / L; m.dir[2] = dz / L; }
    } else {
        m.eonly = true;
        m.L = std::abs(e); m.v = std::min(v, m_cfg.e_max_v); m.a = m_lp.mach.e_acc; m.epm = 0.;
    }
}

// ------------------------------------------------------------------------------------- C1 parser
void PelletScrewLaw::parse(const std::string &g)
{
    using namespace ScrewLaw;
    m_moves.clear();
    const char *base = g.data();
    const size_t n = g.size();
    size_t pos = 0;
    while (pos < n) {
        Move m;
        m.begin = pos;
        const void *nl = std::memchr(base + pos, '\n', n - pos);
        size_t line_end = nl ? size_t(static_cast<const char*>(nl) - base) : n;   // index of '\n' or n
        size_t content_end = line_end;
        if (nl) {
            if (content_end > pos && base[content_end - 1] == '\r') { --content_end; m.eol = 2; }
            else m.eol = 1;
        }
        m.len = content_end - pos;
        pos = nl ? line_end + 1 : n;

        m.f = m_f; m.accel = m_accel; m.accel_raw = m_accel_raw; m.scv = m_scv; m.role = m_role; m.layer = m_layer;
        m.line = m_line_base + m_moves.size(); m.rel_xyz = m_rel_xyz;
        for (int k = 0; k < 3; ++k) m.p0[k] = m.p1[k] = m_pos[k];
        ++m_stats.lines;

        const char *p = base + m.begin, *end = base + m.begin + m.len;
        while (p < end && sl_space(*p)) ++p;
        if (p == end) { ++m_stats.empty; m_moves.push_back(m); continue; }
        if (*p == ';') {
            ++m_stats.comments;
            if (sl_starts(p, end, ";LAYER_CHANGE")) {
                ++m_layer; m_layer_tags = true; ++m_stats.layers; m.layer = m_layer;
            } else if (sl_starts(p, end, ";TYPE:")) {
                std::string role(p + 6, end);
                auto it = std::find(m_roles.begin(), m_roles.end(), role);
                if (it == m_roles.end()) { m_roles.push_back(role); it = m_roles.end() - 1; }
                m_role = int(it - m_roles.begin()); m.role = m_role;
            } else if (sl_starts(p, end, ";PAUSE_PRINT") || sl_starts(p, end, ";COLOR_CHANGE")) {
                mark_pause(m);   // Orca's tag before the machine pause / filament change G-code
            } else if (sl_starts(p, end, ";WIPE_START")) {
                if (m_stats.in_retracts + m_stats.in_fw_retracts + m_stats.in_wipes == 0)
                    m_first_bad_input = strfmt("wipe (;WIPE_START) at layer %d (Z %.3f), stage line %zu", m_layer, m_pos[2], m.line + 1);
                ++m_stats.in_wipes;
            }
            m_moves.push_back(m);
            continue;
        }
        if (m_passthrough) { m_moves.push_back(m); continue; }

        // command word
        const char *w = p;
        while (p < end && !sl_space(*p) && *p != ';') ++p;
        const size_t wl = size_t(p - w);
        const char c0 = sl_upper(*w);
        bool classic = (c0 == 'G' || c0 == 'M' || c0 == 'T') && wl >= 2;
        for (size_t i = 1; classic && i < wl; ++i) classic = (w[i] >= '0' && w[i] <= '9') || w[i] == '.';
        if (!classic) {
            // Klipper extended command / macro: zero time
            if (sl_starts(w, end, "SET_VELOCITY_LIMIT") && wl == 18) {
                double v;
                bool any = false;
                if (sl_param(p, end, "ACCEL", v)) { m_accel = std::min(v, m_cfg.max_accel); m_accel_raw = v; m.sets_accel = any = true; }
                if (sl_param(p, end, "SQUARE_CORNER_VELOCITY", v)) { m_scv = v; any = true; }
                if (sl_param(p, end, "VELOCITY", v)) { m_cfg.max_velocity = v; any = true; }
                m.type = any ? MoveType::Accel : MoveType::Other;
                m.accel = m_accel; m.accel_raw = m_accel_raw; m.scv = m_scv;
                ++m_stats.known;
            } else {
                ++m_stats.macros;
                std::string word(w, wl);
                for (char &ch : word) ch = sl_upper(ch);
                if (!m_model_on && (word == "START_PRINT" || word.find("PURGE") != std::string::npos))
                    m_origin_idx = long(m_moves.size());   // first-stop origin, resolved to a time after move_time()
                if (word == "PAUSE" || word == "M600")
                    mark_pause(m);
            }
            m_moves.push_back(m);
            continue;
        }
        double code = 0.;
        {
            const char *q = w + 1;
            if (!sl_num(q, w + wl, code)) code = -1.;
        }
        Words wd;
        sl_words(p, end, wd);
        if (c0 == 'T') {
            ++m_stats.unsupported;   // tool change: single extruder only
            m_moves.push_back(m);
            continue;
        }
        if (c0 == 'M') {
            const int mc = int(code);
            switch (mc) {
            case 82:
                m_passthrough = m_stats.absolute_e = true;
                SL_LOG_WARN("[SCREWLAW] M82 (absolute E) at line " << (m_line_base + m_moves.size() + 1)
                            << ": the screw law needs relative E, the stage stays in pass-through for the rest of the file");
                ++m_stats.unsupported;
                break;
            case 83: m_rel_e = true; ++m_stats.known; break;
            case 204: {
                // Klipper M204: S, otherwise min(P, T) when both are given
                double a = -1.;
                if (wd.has('S')) a = wd.get('S');
                else if (wd.has('P') && wd.has('T')) a = std::min(wd.get('P'), wd.get('T'));
                if (a > 0.) {
                    m_accel = std::min(a, m_cfg.max_accel); m_accel_raw = a;
                    m.type = MoveType::Accel; m.accel = m_accel; m.accel_raw = a; m.sets_accel = true;
                }
                ++m_stats.known;
                break;
            }
            case 220: case 221:
                ++m_stats.unsupported;   // speed / extrusion factor: the timing and E would be wrong if != 100
                break;
            case 0: case 1: case 25: case 600: case 601:
                mark_pause(m);           // pause / filament change: a long stop of unknown length
                ++m_stats.known;
                break;
            case 17: case 18: case 73: case 84: case 104: case 105: case 106: case 107: case 109: case 117: case 118:
            case 140: case 141: case 190: case 191: case 201: case 203: case 205: case 300: case 400: case 486:
            case 900: case 1002:
                ++m_stats.known;
                break;
            default: ++m_stats.unknown; break;
            }
            m_moves.push_back(m);
            continue;
        }
        // G codes
        const int gc = int(code);
        const bool frac = code != double(gc);
        if (frac) { ++m_stats.unknown; m_moves.push_back(m); continue; }
        switch (gc) {
        case 0: case 1: case 2: case 3: {
            const bool is_arc = gc >= 2;
            double np[3] = { m_pos[0], m_pos[1], m_pos[2] };
            const char ax[3] = { 'X', 'Y', 'Z' };
            for (int k = 0; k < 3; ++k)
                if (wd.has(ax[k])) np[k] = m_rel_xyz ? m_pos[k] + wd.get(ax[k]) : wd.get(ax[k]);
            const double e = wd.has('E') ? wd.get('E') : 0.;   // relative (M83, or G91 as in Klipper)
            if (wd.has('F')) m_f = wd.get('F');
            m.f = m_f;
            m.motion = true; m.has_f = wd.has('F');
            const double dx = np[0] - m_pos[0], dy = np[1] - m_pos[1], dz = np[2] - m_pos[2];
            double L_xy = std::sqrt(dx * dx + dy * dy);
            if (is_arc) {
                m.arc = true;
                ++m_stats.unsupported;
                if (wd.has('I') || wd.has('J')) {
                    const double cx = m_pos[0] + (wd.has('I') ? wd.get('I') : 0.), cy = m_pos[1] + (wd.has('J') ? wd.get('J') : 0.);
                    const double r  = std::hypot(m_pos[0] - cx, m_pos[1] - cy);
                    const double a0 = std::atan2(m_pos[1] - cy, m_pos[0] - cx), a1 = std::atan2(np[1] - cy, np[0] - cx);
                    double sweep = gc == 2 ? a0 - a1 : a1 - a0;
                    const double two_pi = 2.0 * 3.14159265358979323846;
                    while (sweep <= 1e-12) sweep += two_pi;
                    L_xy = r * sweep;
                }
            }
            const double L = std::sqrt(L_xy * L_xy + dz * dz);
            for (int k = 0; k < 3; ++k) { m.p0[k] = m_pos[k]; m.p1[k] = np[k]; }
            for (int k = 0; k < 3; ++k) m_pos[k] = np[k];
            m.e = e;
            if (L < 1e-9 && std::abs(e) < 1e-9) {   // F only / no displacement (kp2 skips it)
                ++m_stats.no_motion_g1; ++m_stats.known;
                m_moves.push_back(m);
                continue;
            }
            ++m_stats.known;
            if (e < -1e-9)                                 m.type = MoveType::Retract;
            else if (std::abs(dz) > 1e-9 && e <= 1e-9)     m.type = MoveType::ZMove;
            else if (L < 1e-9)                             m.type = MoveType::EOnly;
            else if (e > 1e-9)                           { m.type = MoveType::Extrude; m_pause_in_gap = false; }
            else                                           m.type = MoveType::Travel;
            if (m.type == MoveType::Retract) {
                if (m_stats.in_retracts + m_stats.in_fw_retracts + m_stats.in_wipes == 0)
                    m_first_bad_input = strfmt("retraction (E < 0) at layer %d (Z %.3f), stage line %zu: \"%s\"", m_layer, m_pos[2], m.line + 1,
                                               std::string(base + m.begin, std::min<size_t>(m.len, 60)).c_str());
                ++m_stats.in_retracts;
            }
            // kp2.parse: cruise limit v and acceleration a of the move
            kinematics(m, dx, dy, dz, L_xy, is_arc);
            if (!m_layer_tags && m.type == MoveType::Extrude && np[2] > m_layer_z + 1e-6) {   // layers from Z (no tags)
                ++m_layer; m_layer_z = np[2];
            }
            m.layer = m_layer;
            break;
        }
        case 4: {
            double t = 0.;
            if (wd.has('P')) t = wd.get('P') / 1000.0;
            else if (wd.has('S')) t = wd.get('S');
            m.type = MoveType::Dwell; m.plan = true; m.dwell = std::max(0.0, t);
            ++m_stats.known;
            break;
        }
        case 10: case 11:
            // firmware retraction: not modelled (profile constraint use_firmware_retraction = 0); G10 = a stop
            if (gc == 10) m.type = MoveType::Retract;
            if (m_stats.in_retracts + m_stats.in_fw_retracts + m_stats.in_wipes == 0)
                m_first_bad_input = strfmt("firmware retraction (G%d) at layer %d (Z %.3f), stage line %zu", gc, m_layer, m_pos[2], m.line + 1);
            ++m_stats.in_fw_retracts;
            ++m_stats.unsupported;
            break;
        case 28: {
            const char ax[3] = { 'X', 'Y', 'Z' };
            const bool all = !(wd.has('X') || wd.has('Y') || wd.has('Z'));
            for (int k = 0; k < 3; ++k) if (all || wd.has(ax[k])) m_pos[k] = 0.;
            for (int k = 0; k < 3; ++k) m.p1[k] = m_pos[k];
            ++m_stats.unsupported;
            break;
        }
        case 20: ++m_stats.unsupported; break;   // inches: not supported
        case 17: case 18: case 19: case 21: ++m_stats.known; break;
        case 90: m_rel_xyz = false; ++m_stats.known; break;
        case 91: m_rel_xyz = true;  ++m_stats.known; break;
        case 92: {
            const char ax[3] = { 'X', 'Y', 'Z' };
            for (int k = 0; k < 3; ++k) if (wd.has(ax[k])) m_pos[k] = wd.get(ax[k]);
            for (int k = 0; k < 3; ++k) m.p1[k] = m_pos[k];
            ++m_stats.known;
            break;
        }
        default: ++m_stats.unknown; break;
        }
        m_moves.push_back(m);
    }
    for (const Move &m : m_moves) ++m_stats.moves[size_t(m.type)];
}

// ------------------------------------------------------------------------------------- C2 timing
// kp2.simulate(klipper=True) with the lookahead spanning the chunk boundary. The planner runs over the unsettled
// tail of the previous chunk (m_carry, re-timed here) + this chunk, the end of the chunk treated provisionally as
// a full stop (v = 0). A move is SETTLED when its timing cannot change whatever comes next: the backward pass
// run once with end speed 0 and once without an end constraint gives the same junction speeds up to it. Only
// settled moves are integrated in the model and reported; the unsettled tail (the last deceleration distance,
// none after a G4 / an extrude-only move) is carried to the next chunk and re-timed there. So the timing equals
// the one of the whole file in one pass, except the very end of the print (a real stop).
void PelletScrewLaw::move_time()
{
    using namespace ScrewLaw;
    std::vector<Carry> old = std::move(m_carry);
    m_carry.clear();
    m_pre.clear();
    m_settled.assign(m_moves.size(), 1);
    m_first_unsettled = m_moves.size();
    struct P { Move *m; bool chunk; size_t idx; };
    std::vector<P> plan;
    plan.reserve(old.size() + m_moves.size());
    for (size_t i = 0; i < old.size(); ++i) plan.push_back({ &old[i].m, false, i });
    for (size_t i = 0; i < m_moves.size(); ++i)
        if (m_moves[i].plan) plan.push_back({ &m_moves[i], true, i });
    const size_t n = plan.size();
    std::vector<double> lo(n + 1, 0.0);
    lo[0] = old.empty() ? 0.0 : m_carry_v0;   // entry speed of the first carried move (settled)
    for (size_t k = 1; k < n; ++k) lo[k] = sl_junction(*plan[k - 1].m, *plan[k].m, m_lp.mach.icv);
    std::vector<double> hi = lo;
    const bool tail_free = n > 0 && !(plan[n - 1].m->type == MoveType::Dwell || plan[n - 1].m->eonly || plan[n - 1].m->arc);
    hi[n] = tail_free ? std::numeric_limits<double>::infinity() : 0.0;
    auto passes = [&](std::vector<double> &vend) {
        for (size_t kk = n; kk-- > 0;) {
            const Move &m = *plan[kk].m;
            if (m.type == MoveType::Dwell) continue;
            vend[kk] = std::min(vend[kk], std::sqrt(vend[kk + 1] * vend[kk + 1] + 2 * m.a * m.L));
        }
        for (size_t k = 0; k < n; ++k) {
            const Move &m = *plan[k].m;
            if (m.type == MoveType::Dwell) continue;
            vend[k + 1] = std::min(vend[k + 1], std::sqrt(vend[k] * vend[k] + 2 * m.a * m.L));
        }
    };
    passes(lo);
    size_t k_c = n;   // first unsettled plan move
    if (tail_free) {
        passes(hi);
        for (size_t j = 1; j <= n; ++j)
            if (lo[j] != hi[j]) { k_c = j - 1; break; }
    }
    auto time_one = [&](Move &m, size_t k, double &t) {
        m.t0 = t;
        if (m.type == MoveType::Dwell) { t += m.dwell; m.t1 = t; return; }
        const double v0 = lo[k], v1 = lo[k + 1], a = m.a, L = m.L;
        double vc = m.v;
        double la = std::max(0.0, (vc * vc - v0 * v0) / (2 * a)), ld = std::max(0.0, (vc * vc - v1 * v1) / (2 * a));
        if (la + ld > L) {
            const double vp = std::sqrt(std::max(0.0, (2 * a * L + v0 * v0 + v1 * v1) / 2));
            vc = vp;
            la = std::max(0.0, (vp * vp - v0 * v0) / (2 * a)); ld = L - la;
        }
        const double ta = (vc - v0) / a, td = (vc - v1) / a, tc = vc > 0 ? (L - la - ld) / vc : 0.0;
        m.v0 = v0; m.v1 = v1; m.vc = vc; m.la = la; m.ld = ld; m.ta = ta; m.tc = std::max(0.0, tc); m.td = td;
        t += ta + m.tc + td;
        m.t1 = t;
    };
    double t = m_t_settled;
    size_t k = 0;
    for (; k < old.size(); ++k) time_one(old[k].m, k, t);   // the carried moves precede the chunk
    for (size_t i = 0; i < m_moves.size(); ++i) {
        Move &m = m_moves[i];
        if (!m.plan) { m.t0 = m.t1 = t; continue; }
        if (k == k_c) m_first_unsettled = i;
        time_one(m, k, t);
        ++k;
    }
    if (k_c < old.size()) m_first_unsettled = 0;
    for (size_t i = m_first_unsettled; i < m_moves.size(); ++i) m_settled[i] = 0;
    // settled carried moves: integrated first by analyse(); the new unsettled tail goes to m_carry
    for (size_t j = 0; j < n; ++j) {
        const P &p = plan[j];
        if (j < k_c) {
            if (!p.chunk) m_pre.push_back(old[p.idx]);
            if (m_timing_sink) m_timing_sink(p.chunk ? p.m->line : old[p.idx].gline, p.m->type, p.m->t0, p.m->t1);
        } else {
            Carry c;
            if (p.chunk) { c.m = *p.m; c.gline = p.m->line; c.chunk_idx = p.idx; }
            else         { c = old[p.idx]; c.chunk_idx = size_t(-1); }
            m_carry.push_back(std::move(c));
        }
    }
    m_t_settled = k_c < n ? plan[k_c].m->t0 : t;
    m_carry_v0  = k_c < n ? lo[k_c] : 0.0;
    m_stats.carried += m_carry.size();
    m_stats.t_total = t;
    m_t_last = t;
}

// ------------------------------------------------------------------------------------- C2 runs
void PelletScrewLaw::find_runs()
{
    using namespace ScrewLaw;
    m_runs.clear();
    const double stop_xy = m_lp.mach.stop_travel_mm;
    bool in_run = false;   // a run of THIS chunk is open
    Run cur;
    auto close = [&]() {
        // v, A of the first and of the last mm (whole moves until >= 1 mm), v = commanded F
        auto va = [&](bool from_start, double &v, double &A) {
            double sL = 0., sE = 0., sT = 0.;
            const size_t lo = cur.first, hi = cur.last;
            for (size_t c = 0; c <= hi - lo; ++c) {
                const Move &m = m_moves[from_start ? lo + c : hi - c];
                if (m.type != MoveType::Extrude) continue;
                sL += m.L; sE += m.e; sT += m.f > 0 ? m.L / (m.f / 60.0) : 0.;
                if (sL >= 1.0) break;
            }
            A = sL > 0 ? sE / sL : 0.;
            v = sT > 0 ? sL / sT : 0.;
        };
        va(true, cur.v_start, cur.A_start);
        va(false, cur.v_end, cur.A_end);
        cur.layer = m_moves[cur.first].layer;
        m_runs.push_back(cur);
        in_run = false;
    };
    for (size_t i = 0; i < m_moves.size(); ++i) {
        const Move &m = m_moves[i];
        if (m.type == MoveType::Extrude) {
            if (!(in_run && !m_gap_stop)) {
                if (in_run) close();
                else if (m_gap_open && !m_gap_stop) ++m_stats.chunk_splits;   // previous chunk ended in a run, no stop since
                cur = Run();
                cur.first = i;
                in_run = true;
            }
            cur.last = i;
            cur.L += m.L;
            cur.arc |= m.arc;
            m_gap_open = true; m_gap_stop = false; m_gap_xy = 0.;
            continue;
        }
        if (!m_gap_open || m_gap_stop) continue;
        switch (m.type) {
        case MoveType::Travel:
            m_gap_xy += m.L_xy;
            if (m_gap_xy >= stop_xy) {   // a stop with no Z move before the travel: the head travels at layer height
                m_gap_stop = true; ++m_stats.stop_travel;
                if (m_first_bad_gap.empty())
                    m_first_bad_gap = strfmt("travel of %.1f mm at layer height after a run (layer %d, Z %.3f, X %.3f Y %.3f, "
                                             "stage line %zu)", m_gap_xy, m.layer, m.p0[2], m.p0[0], m.p0[1], m.line + 1);
            }
            break;
        case MoveType::EOnly:
            // > 0.05 mm3: Orca leaves zero-length end segments with E ~0.002 (bar stool, 20/10), not an injection
            if (m.e > 0.05) {
                ++m_stats.gap_eonly;
                if (m_first_bad_gap.empty())
                    m_first_bad_gap = strfmt("extrude-only move E %.5f between two runs (layer %d, Z %.3f, stage line %zu)", m.e,
                                             m.layer, m.p0[2], m.line + 1);
            }
            break;
        case MoveType::ZMove:   m_gap_stop = true; ++m_stats.stop_z; break;
        case MoveType::Dwell:   m_gap_stop = true; ++m_stats.stop_dwell; break;
        case MoveType::Retract: m_gap_stop = true; ++m_stats.stop_retract; break;
        default: break;
        }
    }
    if (in_run) close();
}

// ------------------------------------------------------------------------------------- C2 model
void PelletScrewLaw::integrate(const ScrewLaw::Move &m, ScrewLaw::ModelState &st, long long &grid)
{
    using namespace ScrewLaw;
    if (!m_model_on || !m.plan || !(m.t1 > m.t0)) return;
    const double dt = m_lp.mach.sim_dt, eta = m_lp.mach.eta;
    const double q_eonly = m.eonly ? m.e / (m.t1 - m.t0) : 0.;
    for (;;) {
        const double ti = double(grid) * dt;
        if (ti >= m.t1) break;
        double q = 0.;
        if (m.type == MoveType::Dwell) q = 0.;
        else if (m.eonly)              q = q_eonly;
        else                           q = m.epm * sl_speed_at(m, ti - m.t0);
        ScrewLaw::step(m_lp.model, st, eta * std::max(0.0, q), dt);   // negative flow = screw stopped
        ++grid;
        ++m_stats.model_steps;
    }
}

const std::vector<ScrewLaw::Segment> &PelletScrewLaw::end_design(double v, double A, double k_c, double &L_end)
{
    const auto key = std::make_tuple(v, A, k_c);
    auto it = m_end_cache.find(key);
    if (it != m_end_cache.end()) { ++m_stats.end_cache_hits; L_end = it->second.first; return it->second.second; }
    ++m_stats.end_cache_misses;
    if (m_end_cache.size() > 512) m_end_cache.clear();   // bounded memory (a design is ~10-20 kB)
    std::vector<ScrewLaw::Segment> segs = ScrewLaw::design_end(v, A, m_lp, nullptr, k_c);
    const double L = ScrewLaw::seg_length(segs);
    auto res = m_end_cache.emplace(key, std::make_pair(L, std::move(segs)));
    L_end = L;
    return res.first->second.second;
}

void PelletScrewLaw::analyse(std::vector<std::pair<size_t, std::string>> *notes)
{
    using namespace ScrewLaw;
    const double eta = m_lp.mach.eta, dt = m_lp.mach.sim_dt;
    // the tail of the previous chunk, settled now
    for (const Carry &c : m_pre) {
        integrate(c.m, m_st, m_grid);
        if (c.last_ext) m_prev_run_end_t = c.m.t1;
    }
    size_t r = 0;
    double L_fast_cur = 0.;
    for (size_t i = 0; i < m_moves.size(); ++i) {
        const Move &m = m_moves[i];
        if (r < m_runs.size() && m_runs[r].first == i) {
            // ---------------- restart
            const Run &run = m_runs[r];
            ++m_stats.runs;
            const double v = run.v_start, A = run.A_start, Q = eta * A * v;
            const bool first = !m_model_on;
            const bool provisional = !m_settled[i];
            if (provisional) ++m_stats.provisional;
            double t_stop;
            ModelState st = m_st;
            if (first) {
                const double t_gc = m.t0 - m_t_origin;
                t_stop = m_lp.mach.first_stop_s + t_gc;
                m_stats.first_stop_s = t_stop; m_stats.first_stop_gcode_s = t_gc;
                m_st = st = ScrewLaw::state_after_stop(m_lp, Q, t_stop, dt);
                // first grid point at or after the run start (kp2.timeline: samples t_i >= t0 of the move)
                m_grid = (long long)std::ceil(m.t0 / dt);
                while (m_grid > 0 && double(m_grid - 1) * dt >= m.t0) --m_grid;
                while (double(m_grid) * dt < m.t0) ++m_grid;
                m_model_on = true;
                log_line(strfmt("first stop %.2f s, assumed: steady state at the Q of the first run (%.1f mm3/s), END taper, "
                                "then machine.first_stop_s %.2f s (START macro) + %.2f s of G-code before the first extrusion "
                                "(from the %s)", t_stop, Q, m_lp.mach.first_stop_s, t_gc,
                                m_origin_seen ? "last START_PRINT / *PURGE* line" : "stage start"));
            } else {
                t_stop = m.t0 - m_prev_run_end_t;
                ++m_stats.stops;
                if (provisional) {   // state through the unsettled moves before it, provisional timing (copy)
                    long long g = m_grid;
                    for (size_t j = m_first_unsettled; j < i; ++j) integrate(m_moves[j], st, g);
                }
            }
            const bool over = !flow_ok(Q) || !flow_ok(eta * run.A_end * run.v_end);
            if (over) ++m_stats.over_flow;
            if (run.arc) ++m_stats.unsupported_runs;
            double G1 = 0., D2 = 0.;
            ScrewLaw::deficits(m_lp.model, Q, st, G1, D2);
            const bool wall = run.layer >= 2;
            std::string plan_s = "none", gen_s;
            double L_fast = 0., L_end = 0.;
            if (v > 0. && A > 0.) {
                const double k_c = m_lp.knobs.k_e(v, A, eta, wall);
                const std::vector<Segment> &end = end_design(v, A, k_c, L_end);
                PlannedRun pr = ScrewLaw::plan_run(v, A, run.L, G1, D2, m_lp, wall, &end);
                L_fast = pr.L_fast; L_end = pr.L_end;
                plan_s = run_plan_name(pr.plan);
                if (m_lp.coef.general)
                    gen_s = strfmt(" x_s=%.4f x_s_by=%s e_c=%.4f fit=%d", pr.rinfo.x_s, pr.rinfo.x_s_by, pr.rinfo.e_c, pr.rinfo.fit ? 1 : 0);
                switch (pr.plan) {
                case RunPlan::Full:         ++m_stats.full; break;
                case RunPlan::FastEndTrunc: ++m_stats.fast_end_trunc; break;
                case RunPlan::FastTrunc:    ++m_stats.fast_trunc; break;
                }
            } else
                ++m_stats.bad_runs;
            L_fast_cur = L_fast;
            if (notes != nullptr || m_dump) {
                std::string s = strfmt(";_ERS_LAW restart v=%.3f A=%.4f t_stop=%.3f G1=%.4f D2=%.4f V=%.4f V2=%.4f L_run=%.3f plan=%s "
                                       "L_fast=%.3f L_end=%.3f layer=%d", v, A, t_stop, G1, D2, st.V, st.V2, run.L, plan_s.c_str(),
                                       L_fast, L_end, run.layer);
                s += gen_s;
                if (first) s += " first_stop=assumed";
                if (provisional) s += " timing=provisional";
                if (over) s += " over_flow=1";
                if (run.arc) s += " unsupported=arc";
                if (m_dump) log_line(s.substr(1));
                if (notes != nullptr) notes->emplace_back(i, std::move(s));
            }
        }
        if (!m_model_on && m.plan && (m.type == MoveType::EOnly || m.type == MoveType::Retract)) ++m_stats.pre_first_e;
        if (m_settled[i]) integrate(m, m_st, m_grid);
        if (r < m_runs.size() && m_runs[r].last == i) {
            // ---------------- end
            const Run &run = m_runs[r];
            ++m_stats.ends;
            m_prev_run_end_t = m.t1;   // provisional if the move is unsettled: updated when re-timed (last_ext)
            for (Carry &c : m_carry) c.last_ext = c.chunk_idx == i;
            if (notes != nullptr || m_dump) {
                std::string s = strfmt(";_ERS_LAW end v=%.3f A=%.4f L_avail=%.3f", run.v_end, run.A_end, std::max(0.0, run.L - L_fast_cur));
                if (m_lp.coef.general && run.v_end > 0. && run.A_end > 0.) {   // the coast floor and why the END coast stops
                    double L_e = 0.;
                    const std::vector<Segment> &en = end_design(run.v_end, run.A_end, m_lp.knobs.k_e(run.v_end, run.A_end, eta, run.layer >= 2), L_e);
                    s += strfmt(" v_floor=%.3f coast_stop=%s", ScrewLaw::v_floor_eff(run.v_end, m_lp), ScrewLaw::end_stop_reason(en, run.v_end, m_lp));
                }
                if (m_dump) log_line(s.substr(1));
                if (notes != nullptr) notes->emplace_back(i + 1, std::move(s));
            }
            ++r;
        }
    }
}

// ------------------------------------------------------------------------------------- C3/C4 emitter
//
// Emission design (MVP_DESIGN sec. 2.1, 2.4, 2.5):
// - The chunk is parsed (input moves M), the runs found on the input; then the output is written line by line.
//   Lines outside the law are copied verbatim (the slice of the input). Before a verbatim move the stream state
//   it relies on is restored when the law changed it: modal F (only for G0-G3 lines without F, "G1 F<orig> ;_ERS_F")
//   and ACCEL ("SET_VELOCITY_LIMIT ACCEL=<orig> ;_ERS_ACC"); the ACCEL is also restored right after every law block.
// - A run = its extrusion moves (path, arc length s from 0 to L) + the lines between them (in-run lines: comments,
//   short travels, fan...). The law segments (merge_standing of plan_run) are laid along the ORIGINAL path: the
//   restart from s = 0, the END over the last L_end mm; a segment crossing a vertex is split there (same F, E
//   proportional to the length, per-segment error diffusion of the 5-decimal E) and the in-run lines between the
//   two original moves are copied at the vertex. Standing segments (L < l_stand) are E-only moves at the current
//   point (G4 if their E rounds to 0). Original moves entirely outside the law blocks are copied verbatim; an
//   original move cut by a block boundary keeps its E/mm on the outside part (";_ERS_SPLIT", F of the original).
// - Plan (policy B, plan_run) with the real G1/D2 of the model state at the restart, the END designed from the
//   last mm (v_end, A_end) with k_e (ke_wall on layers >= 2). Full plans: the slow tail is cut at
//   tail_max = min(L - L_end, s of the first original move whose F differs from v by more than tail_speed_tol);
//   if the law's step-down after the cut does not fit (restart + END > L), tail_max is lowered by the excess
//   until it fits ("tail fit", the same loop in compare_slicer.py); if 64 such steps do not converge (the length is a step
//   function of tail_max), tail_max is lowered by one tail segment (v * 0.1 s) at a time ("tail fit fallback").
// - END then pause: with the reverse (layers >= 2, stop.reverse_mm3 > 0) "G1 E-V F ;_ERS_REV" sized to
//   stop.reverse_time_s at the extruder acceleration, then "G4 P<rest> ;_ERS_PAUSE" (total machine.pause_s;
//   longer only if the reverse does not fit, logged). eta V (delivered) is added to the NEXT restart in print order:
//   G1' = G1 + eta V / f, f = k_r c_f. fast_trunc: no END, no pause.
// - The model is integrated on the EMITTED moves: they are collected (m_piece) and timed with move_time() (the
//   lookahead carried across pieces / chunks as in C2) before every restart; the state at a restart integrates
//   the unsettled tail provisionally (end of the piece = stop, as the C2 chunk tail). A screw reverse is a
//   negative screw flow = screw stopped (law_tuned / v63), its volume comes back through the compensation.

struct PelletScrewLaw::Out {
    const std::string *g = nullptr;
    std::string s;
    const char *eol = "\n";
    bool   open = false;      // the last verbatim line had no line end
    size_t lines = 0;         // lines written in this chunk
    void put(const std::string &t)
    {
        if (open) { s += eol; open = false; }
        s += t; s += eol; ++lines;
    }
    void verbatim(const ScrewLaw::Move &m)
    {
        if (open) { s += eol; open = false; }
        s.append(*g, m.begin, m.len + m.eol);
        if (m.eol == 0) open = true;
        ++lines;
    }
};

struct PelletScrewLaw::Path {
    const std::vector<ScrewLaw::Move> *M = nullptr;
    std::vector<size_t> P;    // input indices of the extrusion moves of the run
    std::vector<double> S;    // S[k] = arc length at the start of P[k]; S.back() = run length
    size_t k = 0;             // current path move
    double s = 0.;            // current arc length
    double cuts[2] { 0., 0. };// block boundaries (end of the restart, start of the END), after snap()
    const ScrewLaw::Move &cur() const { return (*M)[P[k]]; }
    // A law / block boundary closer than SNAP to a vertex is moved onto it: no sub-0.05 um piece (its E/mm could not
    // be written accurately with 5-decimal E). Changes a segment length by <= 2 * SNAP (= 1e-4 mm).
    static constexpr double SNAP = 5e-5;
    double snap(double sv) const
    {
        auto it = std::lower_bound(S.begin(), S.end(), sv);
        double best = sv, dbest = SNAP;
        if (it != S.end() && *it - sv < dbest) { best = *it; dbest = *it - sv; }
        if (it != S.begin() && sv - *(it - 1) < dbest) best = *(it - 1);
        return std::min(std::max(best, 0.), S.back());
    }
    // Decimals of a point at arc length b inside move kk: 4, more when it is closer than 0.071 mm to a vertex or a block
    // boundary, so that the short piece next to it keeps its length (hence its E/mm and its flow) within 0.1 %
    int decimals(size_t kk, double b) const
    {
        if (b <= S[kk] + 1e-12 || b >= S[kk + 1] - 1e-12) return 4;   // a vertex (input coordinates, <= 4 decimals)
        double dmin = std::min(b - S[kk], S[kk + 1] - b);
        for (double c : cuts) if (std::abs(c - b) > 1e-12) dmin = std::min(dmin, std::abs(c - b));
        if (dmin >= 0.071) return 4;
        return std::min(9, std::max(5, int(std::ceil(std::log10(710. / std::max(dmin, 1e-12))))));
    }
    void point(size_t kk, double sa, double out[3]) const
    {
        const ScrewLaw::Move &m = (*M)[P[kk]];
        if (sa >= S[kk + 1]) { for (int c = 0; c < 3; ++c) out[c] = m.p1[c]; return; }
        const double t = m.L > 0 ? (sa - S[kk]) / m.L : 0.;
        for (int c = 0; c < 3; ++c) out[c] = m.p0[c] + t * (m.p1[c] - m.p0[c]);
    }
};

namespace {
double sl_round(double x, int dec)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "%.*f", dec, x);
    return std::strtod(buf, nullptr);
}
// "%g" (6 significant digits, as Python's :g of the generators) for ACCEL values
std::string sl_g(double x) { return strfmt("%g", x); }
// shortest round-trip decimal of an input value (F / ACCEL restored as the input wrote them), never an exponent
std::string sl_short(double x)
{
    char buf[64];
    for (int p = 0; p <= 12; ++p) {
        snprintf(buf, sizeof(buf), "%.*f", p, x);
        if (std::strtod(buf, nullptr) == x) return buf;
    }
    snprintf(buf, sizeof(buf), "%.12f", x);
    return buf;
}
} // namespace

void PelletScrewLaw::log_cli(const std::string &s) const
{
    SL_LOG_INFO("[SCREWLAW] " << s);
#ifndef SCREWLAW_STANDALONE
    fprintf(stderr, "[SCREWLAW] %s\n", s.c_str());
    fflush(stderr);
#endif
}

void PelletScrewLaw::fail(const std::string &what, const ScrewLaw::Run *run) const
{
    (void)run;   // the location is in the message
    const std::string msg = "Pellet screw law: " + what;
    SL_LOG_ERROR("[SCREWLAW] " << msg);
    throw Slic3r::RuntimeError(msg);
}

void PelletScrewLaw::input_guardrails()
{
    const size_t n = m_stats.in_retracts + m_stats.in_fw_retracts + m_stats.in_wipes;
    if (n == 0) return;
    const std::string what = strfmt("the input G-code contains %zu retraction(s) (E < 0), %zu firmware retraction(s) (G10/G11) "
                                    "and %zu wipe(s); first: %s", m_stats.in_retracts, m_stats.in_fw_retracts, m_stats.in_wipes,
                                    m_first_bad_input.c_str());
    if (m_mode == ScrewLaw::StageMode::Emit)
        fail("set retraction_length = 0, wipe = off (and z_hop >= 3, Normal Lift) in the profile; the law handles the stop. (" +
             what + ")", nullptr);
    if (!m_warned_input) {
        m_warned_input = true;
        SL_LOG_WARN("[SCREWLAW] " << what << " - EMIT mode would fail: set retraction_length = 0, wipe = off (and z_hop >= 3, "
                    "Normal Lift) in the profile");
    }
}

// Gaps between runs (stampa A sA1, 2026-10-18): a stop by XY travel without a Z move before it means the slicer did not
// lift (z_hop 0, slope / spiral lift, skipped retraction): after the END and the exit the head would travel at layer
// height dragging the strand and the bead. An extrude-only move with E > 0 in a gap (retract_restart_extra) is a second
// injection on top of the anchor. Emit: error; Dry / Annotate: warning once.
void PelletScrewLaw::gap_guardrails()
{
    if (m_stats.stop_travel == 0 && m_stats.gap_eonly == 0) return;
    const std::string what = strfmt("%zu stop(s) by a travel at layer height (no Z lift) and %zu extrude-only move(s) between "
                                    "runs; first: %s", m_stats.stop_travel, m_stats.gap_eonly, m_first_bad_gap.c_str());
    if (m_mode == ScrewLaw::StageMode::Emit)
        fail("the slicer must lift (Normal lift, z_hop > 0) at every stop and must not extrude between runs "
             "(retract_restart_extra = 0). (" + what + ")", nullptr);
    if (!m_warned_gap) {
        m_warned_gap = true;
        SL_LOG_WARN("[SCREWLAW] " << what << " - EMIT mode would fail");
    }
}

// Parameter Sweep of the screw-law knobs (GCode, calib.hpp): ";_ERS_SWEEP default" restores the filament scales,
// ";_ERS_SWEEP [obj=N] screw_law_restart_scale=v | screw_law_coast_scale=v" sets the scale (absolute, x the knob) for
// the runs that follow in print order; other keys are ignored.
void PelletScrewLaw::sweep_tag(const char *p, const char *end)
{
    const std::string s(p, end);
    if (s.find("default") != std::string::npos) {
        m_lp.knobs.restart_scale = m_base_restart_scale;
        m_lp.knobs.coast_scale   = m_base_coast_scale;
        return;
    }
    static const char *keys[2] = { "screw_law_restart_scale=", "screw_law_coast_scale=" };
    for (int i = 0; i < 2; ++i) {
        const size_t k = s.find(keys[i]);
        if (k == std::string::npos) continue;
        const double v = std::strtod(s.c_str() + k + std::strlen(keys[i]), nullptr);
        if (!(v > 0.)) continue;
        (i == 0 ? m_lp.knobs.restart_scale : m_lp.knobs.coast_scale) = v;
    }
}

// Print pause / filament change: timed as a dwell of machine.pause_assumed_s (a STOP of unknown, long duration), the line
// itself stays verbatim. One per gap (Orca's tag and the pause command that follows it count once).
void PelletScrewLaw::mark_pause(ScrewLaw::Move &m)
{
    if (m_pause_in_gap) return;
    m_pause_in_gap = true;
    m.type = ScrewLaw::MoveType::Dwell; m.plan = true; m.dwell = m_lp.mach.pause_assumed_s;
    ++m_stats.user_pauses;
    if (m_stats.user_pauses == 1)
        SL_LOG_WARN("[SCREWLAW] print pause / filament change at layer " << m.layer << " (stage line " << m.line + 1
                    << "): timed as a stop of " << m_lp.mach.pause_assumed_s << " s (machine.pause_assumed_s), the real length is unknown");
}

void PelletScrewLaw::push_move(ScrewLaw::Move &&mv, const char *what)
{
    using namespace ScrewLaw;
    const MachineParams &mp = m_lp.mach;
    if (mv.plan && mv.type != MoveType::Dwell) {
        auto where = [&]() { return strfmt(" (%s, output line %zu, layer %d, Z %.3f, X %.3f Y %.3f)", what, mv.line + 1, mv.layer,
                                           mv.p1[2], mv.p1[0], mv.p1[1]); };
        if (mv.e < -1e-9 && mv.type != MoveType::Retract)
            fail("negative E on an emitted move" + where(), nullptr);
        if (mv.e < -1e-9 && std::strcmp(what, "screw reverse") != 0)
            fail(strfmt("negative E %.5f on an emitted move other than the screw reverse", mv.e) + where(), nullptr);
        if (mv.eonly && mv.e > mp.max_extrude_only_e + 1e-9)
            fail(strfmt("extrude-only move E %.3f mm > machine.max_extrude_only_e %.1f (Klipper max_extrude_only_distance)",
                        mv.e, mp.max_extrude_only_e) + where() + "; raise both only if the printer allows it", nullptr);
        if (!mv.eonly && mv.e > 1e-9 && mv.L > 0. && mv.e / mv.L > mp.max_cross_section + 1e-9)
            fail(strfmt("emitted move with E/L %.2f mm2 > machine.max_cross_section %.1f (Klipper max_extrude_cross_section)",
                        mv.e / mv.L, mp.max_cross_section) + where(), nullptr);
    }
    m_piece.push_back(std::move(mv));
}

void PelletScrewLaw::put_verbatim(Out &o, const ScrewLaw::Move &m, bool origin)
{
    using namespace ScrewLaw;
    if (m.type == MoveType::Other && m.len > 11 && o.g->compare(m.begin, 11, ";_ERS_SWEEP") == 0)
        sweep_tag(o.g->data() + m.begin + 11, o.g->data() + m.begin + m.len);   // knobs of the runs that follow
    if (m.motion && !m.has_f && m_emit_f != m.f) {
        o.put("G1 F" + sl_short(m.f) + " ;_ERS_F");
        m_emit_f = m.f;
        ++m_stats.f_restores;
    }
    if (m.plan && !m.eonly && m.type != MoveType::Dwell && m_emit_accel != m.accel_raw)
        put_accel(o, m.accel_raw);
    const size_t line = m_out_line + o.lines;
    o.verbatim(m);
    if (m.motion && m.has_f) m_emit_f = m.f;
    if (m.sets_accel) m_emit_accel = m.accel_raw;
    for (int c = 0; c < 3; ++c) m_epos[c] = m.p1[c];   // p1 = position after every line (G92 / G28 included)
    if (m.plan || origin) {
        Move c = m;
        c.line = line;
        c.origin = origin;
        c.run_end = false;
        push_move(std::move(c), "input line");
    }
}

void PelletScrewLaw::put_accel(Out &o, double acc_raw)
{
    o.put("SET_VELOCITY_LIMIT ACCEL=" + sl_short(acc_raw) + " ;_ERS_ACC");
    m_emit_accel = acc_raw;
    ++m_stats.accel_lines;
}

// One moving piece of a law segment (or the outside part of a split input move): p1 already the exact target.
void PelletScrewLaw::put_law_move(Out &o, const ScrewLaw::Move &ref, const double p1[3], bool z, int dec, long long e_units,
                                  double F, double acc, const std::string &tag, bool split)
{
    using namespace ScrewLaw;
    double acc_r = acc;
    if (split) {   // input E/mm part: the input ACCEL
        if (m_emit_accel != ref.accel_raw) put_accel(o, ref.accel_raw);
        acc_r = ref.accel_raw;
    } else {
        acc_r = std::strtod(sl_g(acc).c_str(), nullptr);
        if (m_emit_accel != acc_r) {
            o.put("SET_VELOCITY_LIMIT ACCEL=" + sl_g(acc) + " ;_ERS_ACC");
            m_emit_accel = acc_r;
            ++m_stats.accel_lines;
        }
    }
    Move mv;
    mv.type = e_units > 0 ? MoveType::Extrude : MoveType::Travel;
    mv.motion = mv.has_f = true;
    mv.role = ref.role; mv.layer = ref.layer; mv.scv = ref.scv;
    mv.accel_raw = acc_r; mv.accel = std::min(acc_r, m_cfg.max_accel);
    for (int c = 0; c < 3; ++c) mv.p0[c] = m_epos[c];
    mv.p1[0] = sl_round(p1[0], dec); mv.p1[1] = sl_round(p1[1], dec); mv.p1[2] = z ? sl_round(p1[2], dec) : m_epos[2];
    mv.e = double(e_units) * 1e-5;
    std::string s = strfmt("G1 X%.*f Y%.*f", dec, p1[0], dec, p1[1]);
    if (z) s += strfmt(" Z%.*f", dec, p1[2]);
    if (e_units != 0) s += strfmt(" E%.5f", mv.e);
    if (split) {
        mv.f = ref.f;
        s += " F" + sl_short(ref.f) + " ;" + tag;
    } else {
        s += strfmt(" F%.2f", F);
        mv.f = sl_round(F, 2);
        s += " ;" + tag;
    }
    const size_t line = m_out_line + o.lines;
    o.put(s);
    m_emit_f = mv.f;
    const double dx = mv.p1[0] - mv.p0[0], dy = mv.p1[1] - mv.p0[1], dz = mv.p1[2] - mv.p0[2];
    kinematics(mv, dx, dy, dz, std::sqrt(dx * dx + dy * dy), false);
    mv.line = line;
    for (int c = 0; c < 3; ++c) m_epos[c] = mv.p1[c];
    if (dec > 4) ++m_stats.fine_xy;
    if (split) ++m_stats.split_lines; else ++m_stats.law_lines;
    m_stats.E_runs_out += mv.e;
    push_move(std::move(mv), split ? "split input move" : "law move");
}

// Standing law piece: E-only move (G4 if E rounds to 0).
void PelletScrewLaw::put_standing(Out &o, const ScrewLaw::Move &ref, double E, double dt, const std::string &tag)
{
    using namespace ScrewLaw;
    Move mv;
    mv.role = ref.role; mv.layer = ref.layer; mv.scv = ref.scv;
    mv.accel_raw = m_emit_accel; mv.accel = std::min(m_emit_accel, m_cfg.max_accel);
    for (int c = 0; c < 3; ++c) mv.p0[c] = mv.p1[c] = m_epos[c];
    const long long eu = std::llround(E * 1e5);
    const size_t line = m_out_line + o.lines;
    if (eu > 0) {
        const double F = 60. * E / dt;
        o.put(strfmt("G1 E%.5f F%.2f ;", double(eu) * 1e-5, F) + tag);
        mv.type = MoveType::EOnly; mv.motion = mv.has_f = true;
        mv.e = double(eu) * 1e-5; mv.f = sl_round(F, 2);
        m_emit_f = mv.f;
        kinematics(mv, 0., 0., 0., 0., false);
        m_stats.E_runs_out += mv.e;
    } else {
        const int P = std::max(1, int(std::lround(dt * 1000.)));
        o.put(strfmt("G4 P%d ;", P) + tag);
        mv.type = MoveType::Dwell; mv.plan = true; mv.dwell = P / 1000.;
    }
    mv.line = line;
    ++m_stats.standing_lines; ++m_stats.law_lines;
    push_move(std::move(mv), "standing law move");
}

// Walk the path from path.s to target: fn(k, a, b) for every part [a, b] inside path move k; the in-run lines
// between two path moves are copied when the walk crosses the vertex.
void PelletScrewLaw::advance(Out &o, Path &path, double target, const std::function<void(size_t, double, double)> &fn)
{
    const size_t n = path.P.size();
    while (path.s < target - 1e-9) {
        const size_t k = path.k;
        if (path.s >= path.S[k + 1] - 1e-9) {
            if (k + 1 >= n) break;   // end of the run (target beyond the path by rounding)
            for (size_t i = path.P[k] + 1; i < path.P[k + 1]; ++i) put_verbatim(o, (*path.M)[i]);
            ++path.k;
            path.s = path.S[path.k];
            continue;
        }
        double b = target;
        if (path.S[k + 1] <= target + 1e-9) b = path.S[k + 1];
        fn(k, path.s, b);
        path.s = b;
    }
}

// Law segments laid along the path from path.s; s_nominal = nominal arc length of the block start (segment ends are
// s_nominal + cumulated L, snapped). Segment E spread over its (snapped) length, 5-decimal error diffusion per segment.
void PelletScrewLaw::lay_block(Out &o, Path &path, const std::vector<ScrewLaw::Segment> &segs, double A_ref, bool rampup, double s_nominal)
{
    using namespace ScrewLaw;
    const double l_stand = m_lp.mach.l_stand;
    double c = s_nominal;
    for (size_t j = 0; j < segs.size(); ++j) {
        const Segment &q = segs[j];
        const std::string tag = std::string(rampup ? "_ERS_RAMPUP " : "_ERS_RAMPDOWN ") + seg_kind_name(q.kind) + " i=" + std::to_string(j);
        if (q.L < l_stand) { put_standing(o, path.cur(), q.E, q.dt, tag); continue; }
        c += q.L;
        const double F = 60. * q.L / q.dt, s0 = path.s, target = path.snap(c), Ls = target - s0;
        const long long e_tot = std::llround(q.E * 1e5);
        long long e_done = 0;
        advance(o, path, target, [&](size_t k, double a, double b) {
            const double piece = b - a;
            const long long e_cum = b >= target - 1e-9 || Ls <= 0. ? e_tot : std::llround(q.E * ((b - s0) / Ls) * 1e5);
            if (piece < 1e-7) { ++m_stats.tiny_skipped; return; }   // its E goes to the next piece of the segment
            double p1[3];
            path.point(k, b, p1);
            const Move &m = (*path.M)[path.P[k]];
            const bool z = std::abs(m.p1[2] - m.p0[2]) > 1e-9;
            put_law_move(o, m, p1, z, path.decimals(k, b), e_cum - e_done, F, q.acc,
                         tag + strfmt(" e0=%.5f", A_ref * piece), false);
            e_done = e_cum;
        });
    }
}

// Input E/mm from path.s to target: whole input moves verbatim, cut ones split (E proportional to L).
void PelletScrewLaw::lay_plain(Out &o, Path &path, double target)
{
    using namespace ScrewLaw;
    advance(o, path, target, [&](size_t k, double a, double b) {
        const Move &m = (*path.M)[path.P[k]];
        if (a <= path.S[k] + 1e-9 && b >= path.S[k + 1] - 1e-9) {
            put_verbatim(o, m);
            ++m_stats.verbatim_run_moves;
            m_stats.E_runs_out += m.e;
            return;
        }
        const double piece = b - a;
        if (piece < 1e-7) { ++m_stats.tiny_skipped; return; }
        double p1[3];
        path.point(k, b, p1);
        const bool z = std::abs(m.p1[2] - m.p0[2]) > 1e-9;
        put_law_move(o, m, p1, z, path.decimals(k, b), std::llround(m.e * piece / m.L * 1e5), m.f, m.accel_raw, "_ERS_SPLIT", true);
    });
}

void PelletScrewLaw::time_piece()
{
    using namespace ScrewLaw;
    m_moves.swap(m_piece);
    m_piece.clear();
    move_time();
    for (const Move &m : m_moves)
        if (m.origin) { m_t_origin = m.t0; m_origin_seen = true; }
    for (const Carry &c : m_pre) {
        integrate(c.m, m_st, m_grid);
        if (c.m.run_end) m_prev_run_end_t = c.m.t1;
    }
    for (size_t i = 0; i < m_moves.size(); ++i)
        if (m_settled[i]) {
            integrate(m_moves[i], m_st, m_grid);
            if (m_moves[i].run_end) m_prev_run_end_t = m_moves[i].t1;
        }
    m_moves.clear();
}

void PelletScrewLaw::emit_run(Out &o, const std::vector<ScrewLaw::Move> &M, const ScrewLaw::Run &run)
{
    using namespace ScrewLaw;
    const MachineParams &mp = m_lp.mach;
    const double eta = mp.eta;
    const Move &m0 = M[run.first];
    auto loc = [&]() { return strfmt("layer %d, Z %.3f, run start X %.3f Y %.3f (stage line %zu)", run.layer, m0.p0[2], m0.p0[0],
                                     m0.p0[1], m0.line + 1); };
    // ---- input guardrails of the run
    if (run.arc) fail("arc moves (G2/G3) inside a run at " + loc() + ": disable arc fitting (enable_arc_fitting = 0)", &run);
    for (size_t i = run.first; i <= run.last; ++i)
        if (M[i].motion && M[i].rel_xyz) fail("relative XYZ (G91) inside a run at " + loc() + " is not supported", &run);
    const double v = run.v_start, A = run.A_start, v_e = run.v_end, A_e = run.A_end;
    const double Q = eta * A * v, Q_e = eta * A_e * v_e;
    for (size_t i = run.first; i <= run.last; ++i) if (M[i].type == MoveType::Extrude) m_stats.E_runs_in += M[i].e;
    if (!(v > 0. && A > 0. && v_e > 0. && A_e > 0.)) {   // cannot happen with E > 0 moves; kept verbatim
        ++m_stats.bad_runs;
        for (size_t i = run.first; i <= run.last; ++i) { put_verbatim(o, M[i]); if (M[i].type == MoveType::Extrude) m_stats.E_runs_out += M[i].e; }
        return;
    }
    if (!flow_ok(Q) || !flow_ok(Q_e)) {
        ++m_stats.over_flow; ++m_stats.rejected;
        fail(strfmt("delivered flow Q = eta A v = %.1f mm3/s (start: v %.2f mm/s, A %.4f mm2) / %.1f mm3/s (end: v %.2f, A %.4f) "
                    "above %.2f * machine.q_max = %.1f mm3/s at ", Q, v, A, Q_e, v_e, A_e, FLOW_GUARD_FACTOR, FLOW_GUARD_FACTOR * mp.q_max) +
             loc() + ": lower the speed, the line width or the layer height of that feature", &run);
    }
    // ---- path
    Path path;
    path.M = &M;
    path.S.push_back(0.);
    for (size_t i = run.first; i <= run.last; ++i)
        if (M[i].type == MoveType::Extrude) { path.P.push_back(i); path.S.push_back(path.S.back() + M[i].L); }
    const double L = path.S.back();
    // ---- model state at the restart: everything emitted so far timed; the unsettled tail provisional
    time_piece();
    ModelState st = m_st;
    long long gr = m_grid;
    double t_prev_end = m_prev_run_end_t;
    const double t_r = m_t_last;
    for (const Carry &c : m_carry) {
        integrate(c.m, st, gr);
        if (c.m.run_end) t_prev_end = c.m.t1;
    }
    ++m_stats.runs;
    const bool first = !m_model_on;
    double t_stop;
    if (first) {
        const double t_gc = t_r - m_t_origin;
        t_stop = mp.first_stop_s + t_gc;
        m_stats.first_stop_s = t_stop; m_stats.first_stop_gcode_s = t_gc;
        m_st = st = ScrewLaw::state_after_stop(m_lp, Q, t_stop, mp.sim_dt);
        m_grid = (long long)std::ceil(t_r / mp.sim_dt);
        while (m_grid > 0 && double(m_grid - 1) * mp.sim_dt >= t_r) --m_grid;
        while (double(m_grid) * mp.sim_dt < t_r) ++m_grid;
        m_model_on = true;
        log_cli(strfmt("first stop %.2f s, ASSUMED: steady state at the Q of the first run (%.1f mm3/s), END taper, then "
                       "machine.first_stop_s %.2f s (START macro: heating, purge, parking) + %.2f s of G-code before the first "
                       "extrusion (from the %s)", t_stop, Q, mp.first_stop_s, t_gc,
                       m_origin_seen ? "last START_PRINT / *PURGE* line" : "stage start"));
    } else {
        t_stop = t_r - t_prev_end;
        ++m_stats.stops;
    }
    // ---- plan
    double G1 = 0., D2 = 0.;
    ScrewLaw::deficits(m_lp.model, Q, st, G1, D2);
    const RestartParams prm = ScrewLaw::restart_params(v, A, m_lp);
    const double comp = m_pending_comp;
    const double G1c = G1 + (comp > 0. ? comp / prm.f : 0.);
    if (comp > 0.) { ++m_stats.comp_applied; m_stats.comp_mm3 += comp; m_pending_comp = 0.; }
    const bool wall = run.layer >= 2;
    const double k_e = m_lp.knobs.k_e(v_e, A_e, eta, wall);
    double L_end0 = 0.;
    const std::vector<Segment> &end_full = end_design(v_e, A_e, k_e, L_end0);
    PlannedRun pr = ScrewLaw::plan_run(v, A, L, G1c, D2, m_lp, wall, &end_full);
    // tail cut: first input move whose speed differs from v
    double s_cut = -1.;
    for (size_t k = 0; k < path.P.size(); ++k) {
        const double fk = M[path.P[k]].f / 60.;
        if (std::abs(fk - v) > mp.tail_speed_tol * v) { s_cut = path.S[k]; break; }
    }
    double tail_max = -1.;
    if (pr.plan == RunPlan::Full) {
        double tmax = L - pr.L_end;
        if (s_cut >= 0. && s_cut < tmax) {
            tmax = s_cut;
            ++m_stats.tail_speed_cuts;
            pr.restart = ScrewLaw::design_restart(v, A, G1c, D2, m_lp, true, tmax, &prm);
        }
        for (int it = 0; it < 64; ++it) {
            const double over = seg_length(pr.restart) + pr.L_end - L;
            if (over <= 1e-9) break;
            tmax -= over;
            ++m_stats.tail_fits;
            pr.restart = ScrewLaw::design_restart(v, A, G1c, D2, m_lp, true, tmax, &prm);
        }
        // Fallback (only when the loop above did not converge, e.g. 120 mm/s: the restart length is a step function of
        // tail_max, 12 mm per tail segment, and the step-down after the cut overshoots by a few um): cut one tail segment
        // (v * 0.1 s) at a time until it fits. Never reached when the loop converges (C3/C4 outputs unchanged).
        for (int it = 0; it < 4096 && seg_length(pr.restart) + pr.L_end > L + 1e-6; ++it) {
            tmax -= v * 0.1;
            ++m_stats.tail_fit_fallbacks;
            pr.restart = ScrewLaw::design_restart(v, A, G1c, D2, m_lp, true, tmax, &prm);
        }
        if (seg_length(pr.restart) + pr.L_end > L + 1e-6)
            fail(strfmt("internal: the restart (%.4f mm) + END (%.4f mm) do not fit the run (%.4f mm) at ", seg_length(pr.restart),
                        pr.L_end, L) + loc(), &run);
        tail_max = tmax;
    }
    switch (pr.plan) {
    case RunPlan::Full:         ++m_stats.full; break;
    case RunPlan::FastEndTrunc: ++m_stats.fast_end_trunc; break;
    case RunPlan::FastTrunc:    ++m_stats.fast_trunc; break;
    }
    {
        std::vector<std::string> bad = ScrewLaw::check(pr.restart, v, m_lp);
        if (!bad.empty()) {
            ++m_stats.rejected;
            fail(strfmt("the restart design (v %.2f mm/s, A %.4f mm2, G1 %.2f, D2 %.2f) violates the machine constraints at ", v, A, G1c, D2) +
                 loc() + ": " + bad.front() + (bad.size() > 1 ? strfmt(" (+%zu more)", bad.size() - 1) : std::string()), &run);
        }
        bad = ScrewLaw::check(pr.end, v_e, m_lp);
        if (!bad.empty()) {
            ++m_stats.rejected;
            fail(strfmt("the END design (v %.2f mm/s, A %.4f mm2) violates the machine constraints at ", v_e, A_e) + loc() + ": " +
                 bad.front(), &run);
        }
    }
    const std::vector<Segment> R  = ScrewLaw::merge_standing(pr.restart, mp.l_stand);
    const std::vector<Segment> En = ScrewLaw::merge_standing(pr.end, mp.l_stand);
    double L_R_mov = 0., L_E_mov = 0.;
    for (const Segment &q : R)  if (q.L >= mp.l_stand) L_R_mov += q.L;
    for (const Segment &q : En) if (q.L >= mp.l_stand) L_E_mov += q.L;
    const double s_E = L - L_E_mov;   // nominal start of the END
    path.cuts[0] = path.snap(L_R_mov);
    path.cuts[1] = path.snap(s_E);
    // ---- restart header + block
    {
        std::string s = strfmt(";_ERS_LAW restart v=%.17g A=%.17g t_stop=%.4f G1=%.17g D2=%.17g V=%.6f V2=%.6f comp=%.17g G1c=%.17g "
                               "f=%.17g L_run=%.17g plan=%s L_fast=%.17g L_end=%.17g tail_max=%.17g tail_cut=%.17g v_end=%.17g "
                               "A_end=%.17g k_e=%.17g wall=%d layer=%d z=%.4f x=%.4f y=%.4f",
                               v, A, t_stop, G1, D2, st.V, st.V2, comp, G1c, prm.f, L, run_plan_name(pr.plan), pr.L_fast, pr.L_end,
                               tail_max, s_cut, v_e, A_e, k_e, wall ? 1 : 0, run.layer, m0.p0[2], m0.p0[0], m0.p0[1]);
        if (first) s += " first_stop=assumed";
        if (m_lp.coef.general) {   // law_general: the anchor laid and its bound, the creep cross-section, the fit_total step-down
            const RestartInfo &ri = pr.rinfo;
            s += strfmt(" x_s=%.17g x_s_by=%s e_c=%.17g fit=%d", ri.x_s, ri.x_s_by, ri.e_c, ri.fit ? 1 : 0);
            const std::string by = ri.x_s_by;
            ++(by == "cap_A" ? m_stats.xs_cap_A : by == "fG1" ? m_stats.xs_fG1 : by == "eonly" ? m_stats.xs_eonly : m_stats.xs_formula);
            if (ri.fit) ++m_stats.fits;
            else if (ri.e_c < std::max(A, mp.q_max / (eta * std::min(prm.v_c, v))) - 1e-12) ++m_stats.creep_capped;
        }
        if (m_dump) log_line(s.substr(1));
        o.put(s);
    }
    ++m_stats.law_restarts;
    lay_block(o, path, R, A, true, 0.);
    if (m_emit_accel != path.cur().accel_raw) put_accel(o, path.cur().accel_raw);
    // ---- middle (input E/mm) and END
    const bool has_end = !En.empty();
    lay_plain(o, path, has_end ? path.snap(s_E) : L);
    if (has_end) {
        std::string s = strfmt(";_ERS_LAW end v=%.17g A=%.17g k_e=%.17g wall=%d plan=%s L_avail=%.17g L_end=%.17g layer=%d",
                               v_e, A_e, k_e, wall ? 1 : 0, run_plan_name(pr.plan), L - pr.L_fast, pr.L_end, run.layer);
        if (m_lp.coef.general) {   // law_general: the coast floor used and why the (full) END coast stopped
            const char *why = ScrewLaw::end_stop_reason(end_full, v_e, m_lp);
            s += strfmt(" v_floor=%.17g coast_stop=%s", ScrewLaw::v_floor_eff(v_e, m_lp), why);
            const std::string w = why;
            ++(w == "T_cmax" ? m_stats.end_T_cmax : w == "floor_rel" ? m_stats.end_floor_rel : m_stats.end_floor_abs);
            if (m_lp.coef.end_coarse_v > 0. && v_e < m_lp.coef.end_coarse_v) ++m_stats.end_coarse;
        }
        if (m_dump) log_line(s.substr(1));
        o.put(s);
        ++m_stats.law_ends; ++m_stats.ends;
        lay_block(o, path, En, A_e, false, s_E);
    }
    lay_plain(o, path, L);   // nothing left normally (rounding)
    if (!m_piece.empty()) m_piece.back().run_end = true;
    // ---- pause (+ screw reverse on layers >= 2)
    if (has_end) {
        double rest = mp.pause_s;
        const Move &ml = M[run.last];
        if (m_lp.stop.reverse_mm3 > 0. && run.layer >= 2) {
            const double V = m_lp.stop.reverse_mm3, T = m_lp.stop.reverse_time_s, a = mp.e_acc;
            const double aT = a * T, disc = aT * aT - 4. * a * V;
            double vr = disc >= 0. ? (aT - std::sqrt(disc)) / 2. : std::sqrt(a * V);   // trapezoid lasting T, else triangle
            vr = std::min(vr, m_cfg.e_max_v);
            const double F = sl_round(vr * 60., 2), vv = F / 60.;
            const double Tr = vv * vv / a <= V ? V / vv + vv / a : 2. * std::sqrt(V / a);
            Move mv;
            mv.role = ml.role; mv.layer = ml.layer; mv.scv = ml.scv;
            mv.accel_raw = m_emit_accel; mv.accel = std::min(m_emit_accel, m_cfg.max_accel);
            for (int c = 0; c < 3; ++c) mv.p0[c] = mv.p1[c] = m_epos[c];
            mv.type = MoveType::Retract; mv.motion = mv.has_f = true;
            mv.e = -sl_round(V, 5); mv.f = F;
            kinematics(mv, 0., 0., 0., 0., false);
            mv.line = m_out_line + o.lines;
            o.put(strfmt("G1 E%.5f F%.2f ;_ERS_REV screw reverse %g mm3 in %.3f s", mv.e, F, V, Tr));
            m_emit_f = F;
            push_move(std::move(mv), "screw reverse");
            m_pending_comp += eta * V;
            ++m_stats.reverses;
            rest = mp.pause_s - Tr;
            if (rest < 0.0005) {
                ++m_stats.pause_extended;
                SL_LOG_WARN("[SCREWLAW] the screw reverse (" << Tr << " s) does not fit machine.pause_s " << mp.pause_s
                            << " s: the pause is extended to the reverse (" << loc() << ")");
            }
        }
        const int P = int(std::lround(rest * 1000.));
        if (P >= 1) {
            Move mv;
            mv.role = ml.role; mv.layer = ml.layer;
            for (int c = 0; c < 3; ++c) mv.p0[c] = mv.p1[c] = m_epos[c];
            mv.type = MoveType::Dwell; mv.plan = true; mv.dwell = P / 1000.;
            mv.line = m_out_line + o.lines;
            o.put(strfmt("G4 P%d ;_ERS_PAUSE", P));
            push_move(std::move(mv), "pause");
            ++m_stats.pauses;
        }
    }
    if (m_lp.stop.exit_mm > 0. && has_end) put_exit(o, M, run);   // fast_trunc (no END, no pause): no exit either
    if (m_emit_accel != M[run.last].accel_raw) put_accel(o, M[run.last].accel_raw);
}

// Stop exit (StopParams): lift exit_hop_mm, then exit_mm forward along the direction of the last moving extrusion of
// the run (shortened to stay inside the printable area), at the current ACCEL; back down to the layer Z only when the
// next input motion of the chunk does not move Z (the slicer travel normally lifts / lands by itself: needs_lift is
// forced with the law). Timed like every emitted move (the stop time of the next restart includes it).
void PelletScrewLaw::put_exit(Out &o, const std::vector<ScrewLaw::Move> &M, const ScrewLaw::Run &run)
{
    using namespace ScrewLaw;
    const StopParams &sp = m_lp.stop;
    double dx = 0., dy = 0.;
    for (size_t i = run.last + 1; i-- > run.first;) {
        const Move &m = M[i];
        if (m.type != MoveType::Extrude) continue;
        const double ex = m.p1[0] - m.p0[0], ey = m.p1[1] - m.p0[1], l = std::sqrt(ex * ex + ey * ey);
        if (l > 1e-6) { dx = ex / l; dy = ey / l; break; }
    }
    double len = (dx == 0. && dy == 0.) ? 0. : sp.exit_mm;
    for (int c = 0; c < 2; ++c) {   // keep the end point inside the printable area
        const double d = c == 0 ? dx : dy, p = m_epos[c];
        if (d > 1e-12) len = std::min(len, (m_cfg.bed_max[c] - p) / d);
        if (d < -1e-12) len = std::min(len, (m_cfg.bed_min[c] - p) / d);
    }
    const double z0 = m_epos[2], F = sl_round(sp.exit_speed_mm_s * 60., 2);
    const Move &ml = M[run.last];
    auto put = [&](const double p1[3], const std::string &txt) {
        Move mv;
        mv.role = ml.role; mv.layer = ml.layer; mv.scv = ml.scv;
        mv.accel_raw = m_emit_accel; mv.accel = std::min(m_emit_accel, m_cfg.max_accel);
        for (int c = 0; c < 3; ++c) { mv.p0[c] = m_epos[c]; mv.p1[c] = p1[c]; }
        mv.type = p1[2] != m_epos[2] ? MoveType::ZMove : MoveType::Travel;
        mv.motion = mv.has_f = true; mv.f = F;
        const double ddx = mv.p1[0] - mv.p0[0], ddy = mv.p1[1] - mv.p0[1], ddz = mv.p1[2] - mv.p0[2];
        kinematics(mv, ddx, ddy, ddz, std::sqrt(ddx * ddx + ddy * ddy), false);
        mv.line = m_out_line + o.lines;
        o.put(txt);
        m_emit_f = F;
        for (int c = 0; c < 3; ++c) m_epos[c] = mv.p1[c];
        push_move(std::move(mv), "stop exit");
        ++m_stats.law_lines;
    };
    const bool hop = sp.exit_hop_mm > 0.;
    if (hop) {
        const double p1[3] = { m_epos[0], m_epos[1], sl_round(z0 + sp.exit_hop_mm, 3) };
        put(p1, strfmt("G1 Z%.3f F%.2f ;_ERS_EXIT hop", p1[2], F));
    }
    if (len >= 0.5) {
        const double p1[3] = { sl_round(m_epos[0] + dx * len, 4), sl_round(m_epos[1] + dy * len, 4), m_epos[2] };
        put(p1, strfmt("G1 X%.4f Y%.4f F%.2f ;_ERS_EXIT forward %.2f mm", p1[0], p1[1], F, len));
    }
    if (hop) {   // the slicer travel normally lifts by itself; otherwise come back to the layer Z
        bool any_motion = false, next_sets_z = false;
        for (size_t i = run.last + 1; i < M.size(); ++i) {
            const Move &m = M[i];
            if (!m.motion || !m.plan || m.eonly) continue;
            any_motion = true;
            next_sets_z = m.p1[2] != m.p0[2];
            break;
        }
        if (any_motion && !next_sets_z) {
            const double p1[3] = { m_epos[0], m_epos[1], z0 };
            put(p1, strfmt("G1 Z%.3f F%.2f ;_ERS_EXIT down", z0, F));
        }
    }
    ++m_stats.exits;
}

std::string PelletScrewLaw::emit_chunk(const std::string &g)
{
    using namespace ScrewLaw;
    const auto t_beg = std::chrono::steady_clock::now();
    double pos0[3] = { m_pos[0], m_pos[1], m_pos[2] };
    parse(g);
    if (m_stats.absolute_e)
        fail("absolute E distances (M82) are not supported: enable use_relative_e_distances (M83) in the printer profile", nullptr);
    input_guardrails();
    find_runs();
    gap_guardrails();
    std::vector<Move> M;
    M.swap(m_moves);
    Out o;
    o.g = &g;
    o.s.reserve(g.size() + g.size() / 4 + 4096);
    if (!M.empty() && M.front().eol == 2) o.eol = "\r\n";
    for (int c = 0; c < 3; ++c) m_epos[c] = pos0[c];
    size_t cur = 0;
    for (const Run &run : m_runs) {
        for (; cur < run.first; ++cur) put_verbatim(o, M[cur], long(cur) == m_origin_idx);
        emit_run(o, M, run);
        cur = run.last + 1;
    }
    for (; cur < M.size(); ++cur) put_verbatim(o, M[cur], long(cur) == m_origin_idx);
    time_piece();
    m_out_line += o.lines;
    m_line_base += M.size();
    m_origin_idx = -1;
    m_stats.law_time += std::chrono::duration<double>(std::chrono::steady_clock::now() - t_beg).count();
    return std::move(o.s);
}

void PelletScrewLaw::finish()
{
    // end of the print: the carried tail is final with the stop at the end
    for (const Carry &c : m_carry) {
        if (m_timing_sink) m_timing_sink(c.gline, c.m.type, c.m.t0, c.m.t1);
        integrate(c.m, m_st, m_grid);
    }
    m_carry.clear();
}

std::string PelletScrewLaw::process_layer(std::string &&gcode)
{
    ++m_stats.chunks;
    if (gcode.empty()) return std::move(gcode);
    if (m_mode == ScrewLaw::StageMode::Emit) return emit_chunk(gcode);
    if (m_passthrough) { m_line_base += size_t(std::count(gcode.begin(), gcode.end(), '\n')) + (gcode.back() != '\n'); return std::move(gcode); }
    parse(gcode);
    input_guardrails();   // warning only (Dry / Annotate)
    std::vector<std::pair<size_t, std::string>> notes;
    if (!m_passthrough) {   // (an M82 in this chunk: nothing of the chunk is analysed)
        move_time();
        if (m_origin_idx >= 0) { m_t_origin = m_moves[size_t(m_origin_idx)].t0; m_origin_seen = true; m_origin_idx = -1; }
        find_runs();
        gap_guardrails();   // warning only (Dry / Annotate)
        analyse(m_mode == ScrewLaw::StageMode::Annotate ? &notes : nullptr);
    }
    m_line_base += m_moves.size();

    // Dry / Annotate: rebuild the chunk from the line slices (+ the notes)
    std::string out;
    out.reserve(gcode.size() + notes.size() * 160);
    size_t ni = 0;
    auto eol_of = [](const ScrewLaw::Move &m) { return m.eol == 2 ? "\r\n" : "\n"; };
    for (size_t i = 0; i < m_moves.size(); ++i) {
        const ScrewLaw::Move &m = m_moves[i];
        for (; ni < notes.size() && notes[ni].first == i; ++ni) { out += notes[ni].second; out += eol_of(m); }
        out.append(gcode, m.begin, m.len + m.eol);
    }
    if (ni < notes.size()) {
        const ScrewLaw::Move *last = m_moves.empty() ? nullptr : &m_moves.back();
        if (last != nullptr && last->eol == 0) out += "\n";
        for (; ni < notes.size(); ++ni) { out += notes[ni].second; out += last != nullptr ? eol_of(*last) : "\n"; }
    }
    if (m_mode == ScrewLaw::StageMode::Dry && out != gcode) {
        SL_LOG_ERROR("[SCREWLAW] DRY: the rebuilt chunk " << m_stats.chunks << " differs from the input (parser bug): the input is returned");
        return std::move(gcode);
    }
    return out;
}

void PelletScrewLaw::log_stats() const
{
    const Stats &s = m_stats;
    std::ostringstream os;
    os << "[SCREWLAW] mode " << ScrewLaw::stage_mode_name(m_mode) << (s.absolute_e ? " (ABSOLUTE E: pass-through)" : "")
       << " | parser: chunks=" << s.chunks << " layers=" << s.layers << " lines=" << s.lines << " comments=" << s.comments
       << " empty=" << s.empty << " macros=" << s.macros << " known=" << s.known << " unknown=" << s.unknown
       << " unsupported=" << s.unsupported << " no_motion_g1=" << s.no_motion_g1 << " moves:";
    for (int k = 0; k < 8; ++k) os << " " << ScrewLaw::move_type_name(ScrewLaw::MoveType(k)) << "=" << s.moves[k];
    os << " | time " << strfmt("%.1f", s.t_total) << " s"
       << " | runs: restarts=" << s.runs << " ends=" << s.ends << " stops=" << s.stops << " (travel " << s.stop_travel
       << ", z " << s.stop_z << ", dwell " << s.stop_dwell << ", retract " << s.stop_retract << ") chunk_splits=" << s.chunk_splits
       << " | plans" << (m_mode == ScrewLaw::StageMode::Emit ? "" : " (not emitted)") << ": full=" << s.full
       << " fast_end_trunc=" << s.fast_end_trunc << " fast_trunc=" << s.fast_trunc
       << " bad=" << s.bad_runs << " | guardrails" << (m_mode == ScrewLaw::StageMode::Emit ? "" : " (report only)")
       << ": unsupported_runs=" << s.unsupported_runs << " over_flow=" << s.over_flow
       << " input retracts/fw/wipes=" << s.in_retracts << "/" << s.in_fw_retracts << "/" << s.in_wipes
       << " | first stop " << (s.first_stop_s >= 0 ? strfmt("%.2f s assumed (first_stop_s %.2f + %.2f s of G-code)", s.first_stop_s,
                                                            m_lp.mach.first_stop_s, s.first_stop_gcode_s)
                                                   : std::string("n/a"))
       << " | lookahead: carried moves " << s.carried << ", provisional restarts " << s.provisional
       << " | pre-first-run E moves " << s.pre_first_e << ", model steps " << s.model_steps << ", END cache " << s.end_cache_hits
       << "/" << (s.end_cache_hits + s.end_cache_misses);
    if (m_mode == ScrewLaw::StageMode::Emit)
        os << " | EMIT: restarts=" << s.law_restarts << " ends=" << s.law_ends << " law lines=" << s.law_lines << " (standing "
           << s.standing_lines << ") split=" << s.split_lines << " verbatim run moves=" << s.verbatim_run_moves << " ACCEL lines="
           << s.accel_lines << " F restores=" << s.f_restores << " pauses=" << s.pauses << " exits=" << s.exits << " user pauses=" << s.user_pauses << " reverses=" << s.reverses
           << " (pause extended " << s.pause_extended << ") compensated restarts=" << s.comp_applied
           << strfmt(" (%.1f mm3)", s.comp_mm3) << " tail speed cuts=" << s.tail_speed_cuts << " tail fits=" << s.tail_fits << " (fallbacks " << s.tail_fit_fallbacks << ")"
           << " tiny pieces skipped=" << s.tiny_skipped << " fine-XY lines=" << s.fine_xy
           << strfmt(" | run E in %.1f out %.1f mm3 (%+.1f) | emitter %.2f s", s.E_runs_in, s.E_runs_out, s.E_runs_out - s.E_runs_in,
                     s.law_time);
    if (m_mode == ScrewLaw::StageMode::Emit && m_lp.coef.general)
        os << " | law_general: anchor by formula/cap_A/fG1/eonly " << s.xs_formula << "/" << s.xs_cap_A << "/" << s.xs_fG1 << "/"
           << s.xs_eonly << ", fit_total " << s.fits << ", creep capped " << s.creep_capped << " | END coast stop floor_abs/floor_rel/T_cmax "
           << s.end_floor_abs << "/" << s.end_floor_rel << "/" << s.end_T_cmax << ", coarse " << s.end_coarse;
    SL_LOG_INFO(os.str());
#ifndef SCREWLAW_STANDALONE
    // the CLI shows only stderr: the summary there too
    fprintf(stderr, "[SCREWLAW] mode %s: restarts %zu (full %zu, fast_end_trunc %zu, fast_trunc %zu), ends %zu, reverses %zu, "
                    "compensated %zu, first stop %.2f s ASSUMED (first_stop_s %.2f + %.2f s of G-code), time %.1f s, chunks %zu\n",
            ScrewLaw::stage_mode_name(m_mode), m_mode == ScrewLaw::StageMode::Emit ? s.law_restarts : s.runs, s.full,
            s.fast_end_trunc, s.fast_trunc, m_mode == ScrewLaw::StageMode::Emit ? s.law_ends : s.ends, s.reverses, s.comp_applied,
            s.first_stop_s, m_lp.mach.first_stop_s, s.first_stop_gcode_s, s.t_total, s.chunks);
    fprintf(stderr, "%s\n", os.str().c_str());
    fflush(stderr);
#endif
}

} // namespace Slic3r
