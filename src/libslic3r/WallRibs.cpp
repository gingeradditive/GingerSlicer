#include "WallRibs.hpp"

#include "AABBTreeLines.hpp"
#include "ClipperUtils.hpp"
#include "Line.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>

namespace Slic3r {

namespace {

// A position on a polygon: the point lies on the segment starting at vertex `seg` (i.e. on
// [poly[seg], poly[(seg+1) % size]]).
struct PolyPos
{
    size_t seg { 0 };
    Point  pt;
};

// Advance `pos` along the polygon by arc length `dist`; `forward` follows the stored point
// order, otherwise it walks against it.
static PolyPos walk_along(const Polygon &poly, const PolyPos &pos, double dist, bool forward)
{
    const size_t n = poly.size();
    PolyPos      cur = pos;
    while (dist > 0.) {
        const Point &seg_start = poly[cur.seg];
        const Point &seg_end   = poly[(cur.seg + 1) % n];
        const Point &target    = forward ? seg_end : seg_start;
        const double remaining = (target - cur.pt).cast<double>().norm();
        if (dist <= remaining) {
            if (remaining > 0.) {
                const Vec2d dir = (target - cur.pt).cast<double>() / remaining;
                cur.pt += (dir * dist).cast<coord_t>();
            }
            return cur;
        }
        dist  -= remaining;
        cur.pt = target;
        cur.seg = forward ? (cur.seg + 1) % n : (cur.seg + n - 1) % n;
        if (! forward)
            cur.pt = poly[(cur.seg + 1) % n];
    }
    return cur;
}

// Unit tangent of the polygon at `pos`, in traversal (stored) direction.
static Vec2d tangent_at(const Polygon &poly, const PolyPos &pos)
{
    const size_t n   = poly.size();
    Vec2d        dir = (poly[(pos.seg + 1) % n] - poly[pos.seg]).cast<double>();
    const double len = dir.norm();
    return len > 0. ? Vec2d(dir / len) : Vec2d(1., 0.);
}

// Closest approach between two polygons: vertices of each projected onto the segments of the
// other (bead-scale precision is plenty for rib placement). Returns the distance and the two
// matched positions.
static double closest_approach(const Polygon &a, const Polygon &b, PolyPos &on_a, PolyPos &on_b)
{
    double best = std::numeric_limits<double>::max();

    AABBTreeLines::LinesDistancer<Line> dist_a(a.lines());
    for (size_t j = 0; j < b.size(); ++ j) {
        auto [d, line_idx, nearest] = dist_a.distance_from_lines_extra<false>(b[j]);
        if (std::abs(d) < best) {
            best  = std::abs(d);
            on_a  = { size_t(line_idx), nearest.cast<coord_t>() };
            on_b  = { j, b[j] };
        }
    }
    AABBTreeLines::LinesDistancer<Line> dist_b(b.lines());
    for (size_t i = 0; i < a.size(); ++ i) {
        auto [d, line_idx, nearest] = dist_b.distance_from_lines_extra<false>(a[i]);
        if (std::abs(d) < best) {
            best  = std::abs(d);
            on_a  = { i, a[i] };
            on_b  = { size_t(line_idx), nearest.cast<coord_t>() };
        }
    }
    return best;
}

// Project a free point onto a polygon.
static double project_onto(const Polygon &poly, const Point &pt, PolyPos &out)
{
    AABBTreeLines::LinesDistancer<Line> dist(poly.lines());
    auto [d, line_idx, nearest] = dist.distance_from_lines_extra<false>(pt);
    out = { size_t(line_idx), nearest.cast<coord_t>() };
    return std::abs(d);
}

static void append_dedup(Points &pts, const Point &p)
{
    if (pts.empty() || (pts.back() - p).cast<double>().squaredNorm() > double(SCALED_EPSILON) * double(SCALED_EPSILON))
        pts.emplace_back(p);
}

// Minimal distance between two segments that do not cross (endpoint-to-segment, 4 ways).
static double segment_distance(const Line &l1, const Line &l2)
{
    return std::min(std::min(l1.distance_to(l2.a), l1.distance_to(l2.b)),
                    std::min(l2.distance_to(l1.a), l2.distance_to(l1.b)));
}

// The chosen rib geometry of one splice: the two links and the two cut endpoints.
struct RibJoint
{
    Point a_exit;    // walk leaves the current walk here (link1 start)
    Point b_enter;   // link1 end on the spliced loop
    Point b_exit;    // walk leaves the spliced loop here (link2 start)
    Point a_reenter; // link2 end back on the walk
};

// Splice loop B into walk A through a rib at (p on A, q on B): the walk leaves A over one
// link, goes once around B, and returns over a second link. Each loop gets a CUT of one
// stagger around its attach point so the links land in the cut gaps; the cut can sit BEFORE
// or AFTER the attach point on each side (4 combinations) - when the attach point is a
// corner, one choice leaves the second link riding on the first link's axis (the walk turns
// onto an edge parallel to the link), so pick the combination with the largest separation
// between the two links (and never a crossing pair).
static Polygon splice_one(const Polygon &a, const PolyPos &p, const Polygon &b_in, PolyPos q, coord_t stagger, RibJoint &joint)
{
    Polygon b = b_in;
    // The links stay parallel (no X crossing) when the two curves run anti-parallel at the
    // closest approach; if they run the same way, traverse B in the opposite direction.
    if (tangent_at(a, p).dot(tangent_at(b, q)) > 0.) {
        const size_t m = b.size();
        b.reverse();
        // Position q in the reversed point order: old segment j = [B[j], B[j+1]] becomes
        // segment m-2-j (mod m) of the reversed polygon.
        q.seg = (2 * m - 2 - q.seg) % m;
    }

    // Cut on A = (u, v): the walk STARTS at v, runs forward all the way around A to u (the
    // short u->v stretch is the cut), leaves over link1 at u and returns to v over link2.
    // Cut on B = (w, e): the walk ENTERS B at e, runs forward around to w (cut = w->e),
    // link1 targets e and link2 leaves from w.
    // The cuts are CENTERED on the attach points (half a stagger each side), so the two link
    // beads STRADDLE the p-q connection axis symmetrically - a one-sided cut put one bead on
    // the axis and the second a full bead off it, and the rib read as eccentric against the
    // hole it connects. Only when the centered pair comes out corner-degenerate (attach point
    // on a sharp corner, links nearly touching) fall back to the off-center placements and
    // pick the best-separated combination.
    struct CutA { PolyPos u, v; };
    struct CutB { PolyPos w, e; };
    const double h = 0.5 * double(stagger);
    const CutA cut_a[3] = { { walk_along(a, p, h, false), walk_along(a, p, h, true) },            // centered on p
                            { p, walk_along(a, p, double(stagger), true) },                       // cut after p
                            { walk_along(a, p, double(stagger), false), p } };                    // cut before p
    const CutB cut_b[3] = { { walk_along(b, q, h, false), walk_along(b, q, h, true) },            // centered on q
                            { walk_along(b, q, double(stagger), false), q },                      // cut before q
                            { q, walk_along(b, q, double(stagger), true) } };                     // cut after q

    // Crossing or corner-degenerate link pairs come out with ~0 separation; two clean
    // parallel staggered links score ~stagger. Among the cleanly separated placements take
    // the most CENTERED one (least eccentric), separation only breaks ties; only when no
    // placement is clean (sharp-corner attach) fall back to the best-separated one.
    int best_i = -1, best_j = -1;
    {
        const double clean     = 0.8 * double(stagger);
        const double off_of[3] = { 0., h, h };
        double       sep[3][3];
        for (int i = 0; i < 3; ++ i)
            for (int j = 0; j < 3; ++ j)
                sep[i][j] = segment_distance(Line(cut_a[i].u.pt, cut_b[j].e.pt),
                                             Line(cut_b[j].w.pt, cut_a[i].v.pt));
        double best_off = std::numeric_limits<double>::max();
        double best_sep = -1.;
        for (int i = 0; i < 3; ++ i)
            for (int j = 0; j < 3; ++ j)
                if (sep[i][j] >= clean) {
                    const double off = off_of[i] + off_of[j];
                    if (off < best_off || (off == best_off && sep[i][j] > best_sep)) {
                        best_off = off;
                        best_sep = sep[i][j];
                        best_i   = i;
                        best_j   = j;
                    }
                }
        if (best_i < 0)
            for (int i = 0; i < 3; ++ i)
                for (int j = 0; j < 3; ++ j)
                    if (sep[i][j] > best_sep) {
                        best_sep = sep[i][j];
                        best_i   = i;
                        best_j   = j;
                    }
    }
    const CutA &ca = cut_a[best_i];
    const CutB &cb = cut_b[best_j];
    joint = { ca.u.pt, cb.e.pt, cb.w.pt, ca.v.pt };

    Points out;
    out.reserve(a.size() + b.size() + 4);
    // A from v all the way around to u (the short u->v stretch is the cut).
    append_dedup(out, ca.v.pt);
    for (size_t i = (ca.v.seg + 1) % a.size(); ; i = (i + 1) % a.size()) {
        append_dedup(out, a[i]);
        if (i == ca.u.seg)
            break;
    }
    append_dedup(out, ca.u.pt);
    // Link1 u->e, then B once around from e to w (the w->e stretch is B's cut).
    append_dedup(out, cb.e.pt);
    for (size_t j = (cb.e.seg + 1) % b.size(); ; j = (j + 1) % b.size()) {
        append_dedup(out, b[j]);
        if (j == cb.w.seg)
            break;
    }
    append_dedup(out, cb.w.pt);
    // The closing edge w->v of the polygon is the second link.
    Polygon merged(std::move(out));
    // Guard against a closing point coincident with the first one.
    if (merged.size() > 1 && (merged.points.back() - merged.points.front()).cast<double>().squaredNorm() <= double(SCALED_EPSILON) * double(SCALED_EPSILON))
        merged.points.pop_back();
    return merged;
}

} // namespace

// Support test against the previous layer. A rib STANDS on yesterday's rib corridor, a wall
// bead or solid fill; where it cannot stand, the planner may still place it with a FOUNDATION
// BUTTRESS below (params.can_found decides - see plan_wall_ribs), so this tester only answers
// the "stands right now" question.
class SupportTester
{
public:
    SupportTester(const WallRibSupport *support, double wall_reach) : m_s(support), m_wall_reach(wall_reach)
    {
        if (m_s != nullptr && m_s->prev_walls != nullptr && ! m_s->prev_walls->empty()) {
            Lines lines;
            for (const Polygon &wall : *m_s->prev_walls)
                append(lines, wall.lines());
            if (! lines.empty())
                m_walls = std::make_unique<AABBTreeLines::LinesDistancer<Line>>(std::move(lines));
        }
    }

    bool enabled() const { return m_s != nullptr && ! m_s->first_layer; }

    // `corridors_count = false` restricts the test to REAL material of the previous layer
    // (solid fill, wall beads, the bed): what a rib can stand on with no help from any
    // column. Yesterday's rib corridors only count for the default (full) test.
    bool point_supported(const Point &p, bool corridors_count = true) const
    {
        if (! this->enabled())
            return true;
        if (corridors_count && m_s->prev_corridors != nullptr)
            for (const Polygon &c : *m_s->prev_corridors)
                if (c.contains(p))
                    return true;
        if (m_s->prev_solid != nullptr)
            for (const ExPolygon &e : *m_s->prev_solid)
                if (e.contains(p))
                    return true;
        if (m_walls != nullptr && std::abs(m_walls->distance_from_lines<false>(p)) <= m_wall_reach)
            return true;
        return false;
    }

    // The whole link must rest on the previous layer; a single sub-bead gap (e.g. crossing the
    // carved corridor edge) is tolerated, anything longer is void.
    bool link_supported(const Point &a, const Point &b, double step, bool corridors_count = true) const
    {
        if (! this->enabled())
            return true;
        const double len = (b - a).cast<double>().norm();
        const int    n   = std::max(2, int(std::ceil(len / step)) + 1);
        int          consecutive_unsupported = 0;
        for (int i = 0; i < n; ++ i) {
            const double t = double(i) / double(n - 1);
            const Point  p = a + ((b - a).cast<double>() * t).cast<coord_t>();
            if (this->point_supported(p, corridors_count))
                consecutive_unsupported = 0;
            else if (++ consecutive_unsupported > 1)
                return false;
        }
        return true;
    }

private:
    const WallRibSupport *m_s;
    double                m_wall_reach;
    std::unique_ptr<AABBTreeLines::LinesDistancer<Line>> m_walls;
};

bool rib_segment_conflicts(const Point &a, const Point &b, coord_t stagger,
                           const Lines &own_a, const Lines &own_b, const Lines &foreign)
{
    const Line   link(a, b);
    const double sg = double(stagger);
    Point        hit;
    // Crossing a foreign bead is illegal ANYWHERE along the link - it means extruding across
    // another wall, into a hole cavity, or over an already-printed bead. The old test
    // exempted any crossing within one stagger of an endpoint regardless of WHOSE edge was
    // crossed; the exemption belongs to the attach contact on the OWN curves only.
    for (const Line &edge : foreign)
        if (link.intersection(edge, &hit))
            return true;
    for (const Line &edge : own_a)
        if (link.intersection(edge, &hit) && (hit - a).cast<double>().norm() > sg)
            return true;
    for (const Line &edge : own_b)
        if (link.intersection(edge, &hit) && (hit - b).cast<double>().norm() > sg)
            return true;
    // Riding: segment intersection is blind to (near-)collinear overlap, which is exactly
    // the hard violation - two same-layer beads sharing the interasse. Sample the axis at
    // half a stagger: a RUN of consecutive samples closer than 0.9 width to some bead for
    // more than one stagger of length is riding, a single close sample is a transversal
    // pass-by. The rib's own staggered link pair sits at exactly one width and passes.
    const double len = (b - a).cast<double>().norm();
    if (len <= sg)
        return false; // too short to ride anything
    const double prox      = 0.9 * sg;
    const int    n         = std::max(2, int(std::ceil(len / (0.5 * sg))) + 1);
    const int    run_limit = 2; // > run_limit consecutive samples ~ more than one stagger
    auto closer_than = [](const Lines &lines, const Point &p, double d) {
        for (const Line &l : lines)
            if (l.distance_to(p) < d)
                return true;
        return false;
    };
    int run_foreign = 0, run_own = 0;
    for (int i = 0; i < n; ++ i) {
        const double t = double(i) / double(n - 1);
        const Point  p = a + ((b - a).cast<double>() * t).cast<coord_t>();
        if (! foreign.empty() && closer_than(foreign, p, prox)) {
            if (++ run_foreign > run_limit)
                return true;
        } else
            run_foreign = 0;
        // Own curves only count outside the attach zones - right at the attach the link is
        // legitimately ON its wall.
        const bool in_attach_zone = (p - a).cast<double>().norm() <= 1.5 * sg ||
                                    (p - b).cast<double>().norm() <= 1.5 * sg;
        if (! in_attach_zone && (closer_than(own_a, p, prox) || closer_than(own_b, p, prox))) {
            if (++ run_own > run_limit)
                return true;
        } else
            run_own = 0;
    }
    return false;
}

bool plan_wall_ribs(const Polygons &loops, const WallRibParams &params,
                    const std::vector<std::pair<Point, Point>> *prev_links,
                    WallRibMerge &out, std::vector<size_t> &unmerged)
{
    const coord_t stagger = params.stagger;
    out = WallRibMerge{};
    unmerged.clear();
    if (stagger <= 0)
        return false;

    WallRibStats *st = params.stats;

    // Mergeable = enough perimeter to host a cut of one stagger without eating the loop.
    std::vector<size_t> candidates;
    for (size_t i = 0; i < loops.size(); ++ i) {
        if (loops[i].size() >= 3 && loops[i].length() > 4. * double(stagger))
            candidates.emplace_back(i);
        else
            unmerged.emplace_back(i);
    }
    if (st != nullptr) {
        st->loops_in       += candidates.size();
        st->drop_too_short += loops.size() - candidates.size();
    }
    if (candidates.size() < 2) {
        unmerged.clear();
        return false;
    }

    // Seed the walk with the biggest loop (the outer wall).
    size_t seed = candidates.front();
    for (size_t i : candidates)
        if (std::abs(loops[i].area()) > std::abs(loops[seed].area()))
            seed = i;

    Polygon merged = loops[seed];
    std::vector<size_t> remaining;
    for (size_t i : candidates)
        if (i != seed)
            remaining.emplace_back(i);

    // Which loops are already part of the walk: everything NOT in the walk (not-yet-spliced
    // candidates, dropped and too-short loops) still prints as its own bead this layer and
    // belongs to the obstacle field of later splices.
    std::vector<bool> in_walk(loops.size(), false);
    in_walk[seed] = true;

    out.loop_keys.emplace_back(loops[seed].points.front());

    // A position is printable when the link is short enough, does not extrude across or along
    // another bead, and RESTS on the previous layer (rib column, wall bead or solid below) -
    // or a foundation buttress can be grown under it (works even over true void).
    const SupportTester support(params.support, 0.55 * double(stagger));

    // Prim: repeatedly splice in the loop closest to the growing walk, so the total rib
    // length is (greedily) minimal and hole-to-hole ribs come out naturally. Every loop is
    // MEANT to be connected (that is the whole point of the feature: one single path per
    // island, like a manual micro-cut in CAD would give); a candidate stays unmerged only
    // when its every link would be longer than the user cap, cross another wall, or hang
    // over true void where not even a foundation can be built.
    while (! remaining.empty()) {
        // COLUMN CONTINUITY outranks nearest-first: reproject the PHYSICAL link of the
        // previous layer (its two attach points) onto the walk and each remaining loop; the
        // column continues when both endpoints land within the caller's per-layer drift
        // budget (params.max_drift: half a bead capped at one layer height = the 45-degree
        // lean limit; a half-bead FLOOR here used to nullify the layer-height cap, letting
        // thin-layer columns lean far past 45 degrees). Endpoints sit ON the walls, so their
        // nearest-point reprojection is well conditioned; the old test projected the MID-GAP
        // anchor onto both curves and compared midpoints, which on a long rib amplifies the
        // polygon discretization noise - at resolution 0.05 it killed columns standing on
        // walls that had not moved at all (measured drift 1.4-2mm against a 1.3mm budget),
        // teleporting the rib to the globally cheapest spot across the part.
        const double reuse_tol = double(params.max_drift);
        size_t  best_pos   = 0;
        bool    via_column = false;
        PolyPos anch_p, anch_q;
        if (prev_links != nullptr) {
            double best_move = std::numeric_limits<double>::max();
            for (size_t k = 0; k < remaining.size(); ++ k) {
                const Polygon &cand = loops[remaining[k]];
                for (const auto &link : *prev_links) {
                    // The walk/loop roles of the two endpoints may swap between layers
                    // (seed choice, merge order): try both assignments.
                    for (int flip = 0; flip < 2; ++ flip) {
                        const Point &end_walk = flip ? link.second : link.first;
                        const Point &end_loop = flip ? link.first  : link.second;
                        PolyPos pa, qa;
                        const double da = project_onto(merged, end_walk, pa);
                        const double db = project_onto(cand, end_loop, qa);
                        if (da > reuse_tol || db > reuse_tol)
                            continue;
                        if ((pa.pt - qa.pt).cast<double>().norm() > double(params.max_link_length))
                            continue;
                        const double move = std::max(da, db);
                        if (move < best_move) {
                            best_move  = move;
                            best_pos   = k;
                            anch_p     = pa;
                            anch_q     = qa;
                            via_column = true;
                        }
                    }
                }
            }
        }

        // No column to continue: plain Prim, the loop closest to the growing walk.
        double  best_dist = std::numeric_limits<double>::max();
        PolyPos best_p, best_q;
        if (via_column) {
            best_dist = closest_approach(merged, loops[remaining[best_pos]], best_p, best_q);
        } else {
            for (size_t k = 0; k < remaining.size(); ++ k) {
                PolyPos on_merged, on_loop;
                double  d = closest_approach(merged, loops[remaining[k]], on_merged, on_loop);
                if (d < best_dist) {
                    best_dist = d;
                    best_pos  = k;
                    best_p    = on_merged;
                    best_q    = on_loop;
                }
            }
            if (best_dist > double(params.max_link_length)) {
                // Everything left is farther than the cap (Prim: best_dist only grows), and
                // none of it continues a column.
                if (st != nullptr)
                    st->drop_prim_far += remaining.size();
                for (size_t i : remaining)
                    unmerged.emplace_back(i);
                break;
            }
        }

        const size_t   target_idx = remaining[best_pos];
        const Polygon &loop       = loops[target_idx];

        // The obstacle field of THIS splice: the walk as it stands now (cuts and already
        // inserted rib links included - a later link must not cross or ride an earlier rib,
        // and an edge that became a cut gap is no longer an obstacle), the target loop, and
        // every other bead of the layer (not-yet-spliced candidates, dropped loops, the
        // caller's open thin walls). Rebuilt each iteration because the walk grows.
        const Lines walk_lines   = merged.lines();
        const Lines target_lines = loop.lines();
        Lines       foreign_lines;
        if (params.extra_obstacles != nullptr)
            foreign_lines = *params.extra_obstacles;
        for (size_t i = 0; i < loops.size(); ++ i)
            if (! in_walk[i] && i != target_idx)
                append(foreign_lines, loops[i].lines());

        // Candidate positions in preference order: the column link (Z-aligned rib, prints on
        // yesterday's rib), then - when a column just died - the scan positions NEAR the dead
        // column, then the closest approach (cheapest rib), then the rest of the scan
        // nearest-gap-first. The FIRST candidate whose link rests on the previous layer wins;
        // when none does, the first candidate whose foundation buttress can be grown is taken.
        // EXCEPTION (free re-founding): when the column died, a candidate standing on REAL
        // material (solid/walls, not yesterday's rib corridors) is position-free - the
        // shortest one is taken before any near-dead preference (see the pass below).
        // Only length and obstacle crossing are hard rejections, so a loop stays unmerged only
        // over true void, past the user cap, or when every link would cross a wall.
        struct CandPos { PolyPos p, q; bool is_anchor; };
        std::vector<CandPos> cands;
        std::vector<CandPos> free_cands;
        if (via_column)
            cands.push_back({ anch_p, anch_q, true });
        // A column DIED here (previous links exist, none reusable): re-found near it, not at
        // the global optimum. The old behaviour teleported the rib across the part (166mm on
        // the real part) whenever the geometry under a column changed, even though valid
        // positions existed a few mm away.
        const bool   column_died = ! via_column && prev_links != nullptr && ! prev_links->empty();
        const double prox_radius = 8. * double(stagger);
        auto near_dead_column = [&](const Point &a, const Point &b) {
            const Point mid = (a + b) / 2;
            for (const auto &link : *prev_links)
                if (((link.first + link.second) / 2 - mid).cast<double>().norm() <= prox_radius)
                    return true;
            return false;
        };
        // Scan around the loop. Step of at most one stagger: an existing corridor below has
        // a walkable window of about one stagger along the loop, and the scan must not be able
        // to miss it (that is how columns used to die on annulus sections); short loops scan
        // finer (~96 samples). The walk's distancer is built ONCE here - project_onto would
        // rebuild it per sample, and at one-stagger steps a big part scans hundreds of
        // positions per loop.
        {
            const double step = std::min(loop.length() / 96., double(stagger));
            struct ScanPos { double gap; PolyPos p, q; };
            std::vector<ScanPos> scan;
            AABBTreeLines::LinesDistancer<Line> merged_dist(merged.lines());
            PolyPos cursor { 0, loop.points.front() };
            for (double walked = 0.; walked < loop.length() - step * 0.5; walked += step) {
                if (walked > 0.)
                    cursor = walk_along(loop, cursor, step, true);
                auto [d, line_idx, nearest] = merged_dist.distance_from_lines_extra<false>(cursor.pt);
                const double gap = std::abs(d);
                if (gap <= double(params.max_link_length))
                    scan.push_back({ gap, PolyPos{ size_t(line_idx), nearest.cast<coord_t>() }, cursor });
            }
            std::sort(scan.begin(), scan.end(), [](const ScanPos &l, const ScanPos &r) { return l.gap < r.gap; });
            if (column_died)
                for (const ScanPos &s : scan)
                    if (near_dead_column(s.p.pt, s.q.pt))
                        cands.push_back({ s.p, s.q, false });
            cands.push_back({ best_p, best_q, false });
            for (const ScanPos &s : scan)
                if (! (column_died && near_dead_column(s.p.pt, s.q.pt)))
                    cands.push_back({ s.p, s.q, false });
            // Free-placement list for the re-founding pass below: pure gap order, closest
            // approach first, no near-dead preference.
            if (column_died && support.enabled()) {
                free_cands.push_back({ best_p, best_q, false });
                for (const ScanPos &s : scan)
                    free_cands.push_back({ s.p, s.q, false });
            }
        }

        bool    accepted = false, via_anchor = false, need_foundation = false;
        bool    have_fallback = false;
        size_t  rej_long = 0, rej_obstacle = 0, rej_void = 0;
        PolyPos use_p, use_q;
        CandPos fallback {};
        // FREE RE-FOUNDING pass: the near-dead preference exists to buy SELF-support - a rib
        // placed where yesterday's rib corridor lies keeps standing when nothing else is
        // there. A candidate resting on REAL material (solid fill, wall beads) buys nothing
        // from the corpse's position, so the shortest such link wins outright. Without this,
        // a whole feature dying at once (the engraved text on the stool hub: dozens of loops
        // gone in one layer) made the near-dead order pick a 54 mm chord across the hub
        // standing on plain bottom solid, while the 18 mm rib across the spoke - just as
        // supported - was never even reached.
        for (const CandPos &c : free_cands) {
            if ((c.p.pt - c.q.pt).cast<double>().norm() > double(params.max_link_length)
                || rib_segment_conflicts(c.p.pt, c.q.pt, stagger, walk_lines, target_lines, foreign_lines))
                continue; // counted by the main pass below if nothing is accepted
            if (support.link_supported(c.p.pt, c.q.pt, 0.5 * double(stagger), /*corridors_count=*/false)) {
                use_p    = c.p;
                use_q    = c.q;
                accepted = true;
                break;
            }
        }
        for (const CandPos &c : cands) {
            if (accepted)
                break;
            if ((c.p.pt - c.q.pt).cast<double>().norm() > double(params.max_link_length)) {
                ++ rej_long;
                continue;
            }
            if (rib_segment_conflicts(c.p.pt, c.q.pt, stagger, walk_lines, target_lines, foreign_lines)) {
                ++ rej_obstacle;
                continue;
            }
            const double step = 0.5 * double(stagger);
            if (support.link_supported(c.p.pt, c.q.pt, step)) {
                use_p      = c.p;
                use_q      = c.q;
                via_anchor = c.is_anchor;
                accepted   = true;
                break;
            }
            // Keep looking for a genuinely supported spot, but remember the first candidate a
            // foundation buttress could carry (once found, later candidates only matter if
            // supported - the buttress dry-run is not free).
            if (! have_fallback) {
                if (params.can_found && params.can_found(c.p.pt, c.q.pt)) {
                    fallback      = c;
                    have_fallback = true;
                } else
                    ++ rej_void;
            }
        }
        if (! accepted && have_fallback) {
            use_p           = fallback.p;
            use_q           = fallback.q;
            via_anchor      = fallback.is_anchor;
            need_foundation = true;
            accepted        = true;
        }

        if (! accepted) {
            if (st != nullptr) {
                if (rej_void > 0)
                    ++ st->drop_unsupported;   // true void below every position
                else if (rej_obstacle > 0)
                    ++ st->drop_obstacle;
                else
                    ++ st->drop_prim_far;
            }
            unmerged.emplace_back(remaining[best_pos]);
            remaining.erase(remaining.begin() + best_pos);
            continue;
        }
        if (st != nullptr) {
            ++ st->spliced;
            if (via_anchor)
                ++ st->anchor_reused;
            if (need_foundation)
                ++ st->founded;
        }

        out.loop_keys.emplace_back(loop.points.front());
        RibJoint joint;
        merged = splice_one(merged, use_p, loop, use_q, stagger, joint);
        in_walk[target_idx] = true;
        remaining.erase(remaining.begin() + best_pos);

        // Corridor = the rib quad (both links + both cuts) expanded by bead half width +
        // clearance; subtracted from the fill surfaces so no infill rides over the rib.
        // A (nearly) degenerate quad means the two loops touch - no gap for infill to cross,
        // no corridor needed. Clipper offsetting with JoinType::Miter keeps the quad shape.
        Polygon quad({ joint.a_exit, joint.b_enter, joint.b_exit, joint.a_reenter });
        if (std::abs(quad.area()) > 0.01 * double(stagger) * double(stagger)) {
            quad.make_counter_clockwise();
            for (Polygon &e : offset(quad, float(params.corridor_offset)))
                out.corridors.emplace_back(std::move(e));
        }
        // A rib standing on nothing reports its CONNECTION AXIS (the attach pair, i.e. the
        // centerline the two beads straddle): the caller grows the foundation buttress under
        // it, whose stubs straddle the same axis - stacked exactly under the rib beads.
        if (need_foundation)
            out.founded_links.emplace_back(use_p.pt, use_q.pt);
        // The column anchor is the midpoint of the ATTACH pair, not of the cut link: the cut
        // shifts one link end by up to a stagger to whichever side scores best, and anchoring
        // on that made the column creep half a bead sideways EVERY layer (a 45-degree dashed
        // staircase across dead-vertical walls). The attach midpoint is the fixed point of
        // next layer's projections, so a column on stationary geometry stays truly vertical;
        // the cut may wobble around it, the corridor covers that.
        out.anchors.emplace_back((use_p.pt + use_q.pt) / 2);
        // The physical attach pair: next layer's column-reuse test reprojects these endpoints.
        out.links.emplace_back(use_p.pt, use_q.pt);
    }

    if (merged.size() < 3 || out.loop_keys.size() < 2)
        return false;
    out.merged = std::move(merged);
    return true;
}

bool splice_wall_loops(const Polygons &loops, coord_t stagger, Polygon &merged, std::vector<size_t> &unmerged)
{
    WallRibParams params;
    params.stagger         = stagger;
    params.corridor_offset = coord_t(stagger / 2); // bead half width, mirroring production
    WallRibMerge plan;
    if (! plan_wall_ribs(loops, params, nullptr, plan, unmerged))
        return false;
    merged = std::move(plan.merged);
    return true;
}

bool splice_wall_stub(Polygon &walk, const Point &base_hint, const Point &tip, coord_t stagger, Polygon &quad_out)
{
    if (stagger <= 0 || walk.size() < 3 || walk.length() <= 4. * double(stagger))
        return false;
    PolyPos axis;
    project_onto(walk, base_hint, axis);
    // A stub shorter than one bead has melted back into the wall - nothing to print.
    if ((tip - axis.pt).cast<double>().norm() <= double(stagger))
        return false;
    // Cut centered on the base axis, tips straddling the axis tip: the stub's two beads sit
    // symmetrically about the rib axis above (same geometry as the rib pair itself), so the
    // buttress stacks directly under the rib beads - the same touching staggered pair, no
    // doubled centerline.
    const PolyPos u = walk_along(walk, axis, 0.5 * double(stagger), false);
    const PolyPos v = walk_along(walk, axis, 0.5 * double(stagger), true);
    const Point   cv    = v.pt - u.pt;
    const Point   tip_a = tip - cv / 2;
    const Point   tip_b = tip + cv / 2;
    Points pts;
    pts.reserve(walk.size() + 4);
    append_dedup(pts, v.pt);
    for (size_t i = (v.seg + 1) % walk.size(); ; i = (i + 1) % walk.size()) {
        append_dedup(pts, walk[i]);
        if (i == u.seg)
            break;
    }
    append_dedup(pts, u.pt);
    append_dedup(pts, tip_a);
    append_dedup(pts, tip_b);
    Polygon stubbed(std::move(pts));
    if (stubbed.size() > 1 && (stubbed.points.back() - stubbed.points.front()).cast<double>().squaredNorm() <= double(SCALED_EPSILON) * double(SCALED_EPSILON))
        stubbed.points.pop_back();
    if (stubbed.size() < 3)
        return false;
    quad_out = Polygon({ u.pt, tip_a, tip_b, v.pt });
    quad_out.make_counter_clockwise();
    walk = std::move(stubbed);
    return true;
}

bool wall_rib_link_supported(const Point &a, const Point &b, coord_t stagger, const WallRibSupport &support)
{
    const SupportTester tester(&support, 0.55 * double(stagger));
    return tester.link_supported(a, b, 0.5 * double(stagger));
}

} // namespace Slic3r
