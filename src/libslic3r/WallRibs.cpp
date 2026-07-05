#include "WallRibs.hpp"

#include "AABBTreeLines.hpp"
#include "Line.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

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

// Splice loop B into walk A through a rib at (p on A, q on B): the walk leaves A over one
// link, goes once around B, and returns over a second link. Each loop gets a CUT of one
// stagger around its attach point so the links land in the cut gaps; the cut can sit BEFORE
// or AFTER the attach point on each side (4 combinations) - when the attach point is a
// corner, one choice leaves the second link riding on the first link's axis (the walk turns
// onto an edge parallel to the link), so pick the combination with the largest separation
// between the two links (and never a crossing pair).
static Polygon splice_one(const Polygon &a, const PolyPos &p, const Polygon &b_in, PolyPos q, coord_t stagger)
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

bool splice_wall_loops(const Polygons &loops, coord_t stagger, Polygon &merged, std::vector<size_t> &unmerged)
{
    merged.points.clear();
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

    merged = loops[seed];
    std::vector<size_t> remaining;
    for (size_t i : candidates)
        if (i != seed)
            remaining.emplace_back(i);

    // Prim: repeatedly splice in the loop closest to the growing walk, so the total rib
    // length is (greedily) minimal and hole-to-hole ribs come out naturally.
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
        merged = splice_one(merged, best_p, loops[remaining[best_pos]], best_q, stagger);
        remaining.erase(remaining.begin() + best_pos);
    }
    return merged.size() >= 3;
}

} // namespace Slic3r
