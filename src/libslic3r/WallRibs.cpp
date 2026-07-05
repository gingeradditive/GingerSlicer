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
    struct CutA { PolyPos u, v; };
    struct CutB { PolyPos w, e; };
    const CutA cut_a[2] = { { p, walk_along(a, p, double(stagger), true) },                       // cut after p
                            { walk_along(a, p, double(stagger), false), p } };                    // cut before p
    const CutB cut_b[2] = { { walk_along(b, q, double(stagger), false), q },                      // cut before q
                            { q, walk_along(b, q, double(stagger), true) } };                     // cut after q

    int    best_i = 0, best_j = 0;
    double best_score = -1.;
    for (int i = 0; i < 2; ++ i)
        for (int j = 0; j < 2; ++ j) {
            // Crossing or corner-degenerate link pairs come out with ~0 separation; two clean
            // parallel staggered links score ~stagger.
            const double score = segment_distance(Line(cut_a[i].u.pt, cut_b[j].e.pt),
                                                  Line(cut_b[j].w.pt, cut_a[i].v.pt));
            if (score > best_score) {
                best_score = score;
                best_i = i;
                best_j = j;
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

// Support test against the previous layer: a rib may only stand on yesterday's rib corridor,
// on a wall bead, or on solid fill - never over sparse infill or bare air inside the outline.
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

    bool point_supported(const Point &p) const
    {
        if (! this->enabled())
            return true;
        if (m_s->prev_corridors != nullptr)
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
    bool link_supported(const Point &a, const Point &b, double step) const
    {
        if (! this->enabled())
            return true;
        const double len = (b - a).cast<double>().norm();
        const int    n   = std::max(2, int(std::ceil(len / step)) + 1);
        int          consecutive_unsupported = 0;
        for (int i = 0; i < n; ++ i) {
            const double t = double(i) / double(n - 1);
            const Point  p = a + ((b - a).cast<double>() * t).cast<coord_t>();
            if (this->point_supported(p))
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

// Does the segment cross any obstacle loop? Endpoint contacts (the link legitimately starts
// and ends ON a wall) do not count; anything deeper does.
static bool link_crosses_obstacles(const Point &a, const Point &b, const Polygons *obstacles, coord_t stagger)
{
    if (obstacles == nullptr)
        return false;
    const Line link(a, b);
    Point      hit;
    for (const Polygon &obstacle : *obstacles)
        for (const Line &edge : obstacle.lines())
            if (link.intersection(edge, &hit)
                && (hit - a).cast<double>().norm() > double(stagger)
                && (hit - b).cast<double>().norm() > double(stagger))
                return true;
    return false;
}

bool plan_wall_ribs(const Polygons &loops, const WallRibParams &params,
                    const Points *prev_anchors, WallRibMerge &out, std::vector<size_t> &unmerged)
{
    const coord_t stagger = params.stagger;
    out = WallRibMerge{};
    unmerged.clear();
    if (stagger <= 0)
        return false;

    // Mergeable = enough perimeter to host a cut of one stagger without eating the loop.
    std::vector<size_t> candidates;
    for (size_t i = 0; i < loops.size(); ++ i) {
        if (loops[i].size() >= 3 && loops[i].length() > 4. * double(stagger))
            candidates.emplace_back(i);
        else
            unmerged.emplace_back(i);
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

    out.loop_keys.emplace_back(loops[seed].points.front());

    // A position is printable when the link is short enough, does not extrude across another
    // wall/hole, and RESTS on the previous layer (rib column, wall bead or solid below).
    const SupportTester support(params.support, 0.55 * double(stagger));

    // Prim: repeatedly splice in the loop closest to the growing walk, so the total rib
    // length is (greedily) minimal and hole-to-hole ribs come out naturally. A candidate whose
    // best link is too long, crosses another wall, hangs in the air, or would drag its column
    // sideways beyond the drift budget stays unmerged (the single-path seam chain covers it
    // with a short travel - strictly better than a giant rib through the part or a staircase
    // in the air).
    while (! remaining.empty()) {
        double  best_dist = std::numeric_limits<double>::max();
        size_t  best_pos  = 0;
        PolyPos best_p, best_q;
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
            // Everything left is farther than the cap (Prim: best_dist only grows).
            for (size_t i : remaining)
                unmerged.emplace_back(i);
            break;
        }

        const Polygon &loop = loops[remaining[best_pos]];

        bool    accepted = false;
        PolyPos use_p, use_q;
        auto try_pair = [&](const PolyPos &p, const PolyPos &q) {
            if (accepted)
                return;
            if ((p.pt - q.pt).cast<double>().norm() > double(params.max_link_length))
                return;
            if (link_crosses_obstacles(p.pt, q.pt, params.obstacles, stagger))
                return;
            if (! support.link_supported(p.pt, q.pt, 0.5 * double(stagger)))
                return;
            use_p    = p;
            use_q    = q;
            accepted = true;
        };

        // 1) Z alignment: if a previous-layer rib sits between (nearly) the same two curves,
        // reuse its position so the column stacks. If the column exists but following it would
        // exceed the per-layer drift budget, the column ENDS here (no sideways staircase, no
        // relocated replacement - the seam chain covers this hole with a short travel).
        bool column_over_budget = false;
        if (prev_anchors != nullptr) {
            double       best_score  = std::numeric_limits<double>::max();
            PolyPos      anch_p, anch_q;
            const Point *used_anchor = nullptr;
            for (const Point &anchor : *prev_anchors) {
                PolyPos pa, qa;
                const double da = project_onto(merged, anchor, pa);
                const double db = project_onto(loop, anchor, qa);
                // The anchor must sit in THIS gap (near both curves), not across the part.
                const double gap = (pa.pt - qa.pt).cast<double>().norm();
                if (da + db > gap + 2. * double(stagger))
                    continue;
                if (gap > best_dist + 2. * double(stagger))
                    continue; // too much worse than the optimal link - geometry moved away
                if (gap < best_score) {
                    best_score  = gap;
                    anch_p      = pa;
                    anch_q      = qa;
                    used_anchor = &anchor;
                }
            }
            if (used_anchor != nullptr) {
                const Point new_mid = (anch_p.pt + anch_q.pt) / 2;
                if ((new_mid - *used_anchor).cast<double>().norm() > double(params.max_drift))
                    column_over_budget = true;
                else
                    try_pair(anch_p, anch_q);
            }
        }

        if (! column_over_budget) {
            // 2) The optimal closest approach (a NEW column - the support test decides whether
            // this spot can host one, e.g. over the bottom shells of a freshly appeared hole).
            try_pair(best_p, best_q);
            // 3) Scan the gap: sample positions around the loop, project them onto the walk and
            // try them nearest-first - actively LOOKING for a supported spot instead of giving
            // up because the single optimal one hangs over sparse.
            if (! accepted) {
                const double step = std::max(loop.length() / 48., 2. * double(stagger));
                struct ScanPos { double gap; PolyPos p, q; };
                std::vector<ScanPos> scan;
                PolyPos cursor { 0, loop.points.front() };
                for (double walked = 0.; walked < loop.length() - step * 0.5; walked += step) {
                    if (walked > 0.)
                        cursor = walk_along(loop, cursor, step, true);
                    PolyPos      pm;
                    const double gap = project_onto(merged, cursor.pt, pm);
                    if (gap <= double(params.max_link_length))
                        scan.push_back({ gap, pm, cursor });
                }
                std::sort(scan.begin(), scan.end(), [](const ScanPos &l, const ScanPos &r) { return l.gap < r.gap; });
                for (const ScanPos &s : scan) {
                    try_pair(s.p, s.q);
                    if (accepted)
                        break;
                }
            }
        }

        if (! accepted) {
            unmerged.emplace_back(remaining[best_pos]);
            remaining.erase(remaining.begin() + best_pos);
            continue;
        }

        out.loop_keys.emplace_back(loop.points.front());
        RibJoint joint;
        merged = splice_one(merged, use_p, loop, use_q, stagger, joint);
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
        out.anchors.emplace_back((joint.a_exit + joint.b_enter) / 2);
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
    params.corridor_offset = stagger;
    WallRibMerge plan;
    if (! plan_wall_ribs(loops, params, nullptr, plan, unmerged))
        return false;
    merged = std::move(plan.merged);
    return true;
}

} // namespace Slic3r
