#include "WallFusion.hpp"

#include "ClipperUtils.hpp"
#include "Clipper2Utils.hpp"
#include "AABBTreeLines.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace Slic3r {

// The branch tubes: every branch dilated by half a spacing, unioned. Round joins and round ends,
// exactly like multiline_fill() - the round cap is what closes the gorge tip into a turn instead
// of a corner, and the union is what merges branches that touch into one piece.
static Polygons branch_tubes(const Polylines &branches, const coord_t half_width)
{
    if (branches.empty() || half_width <= 0)
        return {};

    Clipper2Lib::Paths64    subject = Slic3rPolylines_to_Paths64(branches);
    Clipper2Lib::ClipperOffset offsetter(2.0);
    offsetter.AddPaths(subject, Clipper2Lib::JoinType::Round, Clipper2Lib::EndType::Round);
    Clipper2Lib::Paths64    solution;
    offsetter.Execute(double(half_width), solution);

    Polygons out;
    out.reserve(solution.size());
    for (const Clipper2Lib::Path64 &p : solution) {
        if (p.size() < 3)
            continue;
        Polygon poly;
        poly.points.reserve(p.size());
        for (const Clipper2Lib::Point64 &pt : p)
            poly.points.emplace_back(coord_t(pt.x), coord_t(pt.y));
        out.emplace_back(std::move(poly));
    }
    return out;
}

// Push one end of a polyline further along the direction of its terminal segment.
static void extend_end(Polyline &pl, const bool at_front, const double by)
{
    if (pl.size() < 2 || by <= 0.)
        return;
    const Point &a   = at_front ? pl.points.front() : pl.points.back();
    const Point &b   = at_front ? pl.points[1]      : pl.points[pl.size() - 2];
    const Vec2d  dir = (a - b).cast<double>();
    const double n   = dir.norm();
    if (n < SCALED_EPSILON)
        return;
    const Point np = a + (dir * (by / n)).cast<coord_t>();
    if (at_front)
        pl.points.insert(pl.points.begin(), np);
    else
        pl.points.emplace_back(np);
}

// R2 + R3 + R9. Every polyline out of Node::convertToPolylines() starts at a leaf; the main one
// ends at the tree root (which sits on the FILL boundary, inside the wall centerline), the side
// ones end at their junction with the parent. So only an end that is close to the wall is a root,
// and only that end is extended.
static Polylines prepare_branches(const Polygon          &wall,
                                  const Polylines        &in,
                                  const WallFusionParams &p,
                                  size_t                 &pruned,
                                  size_t                 &dropped_roots,
                                  size_t                 &gorges)
{
    AABBTreeLines::LinesDistancer<Line> wall_d(wall.lines());

    struct Cand {
        Polyline pl;
        double   len       = 0.;
        bool     is_root   = false;
        Point    root_on_wall;
    };
    std::vector<Cand> cands;
    cands.reserve(in.size());

    for (const Polyline &src : in) {
        if (src.size() < 2)
            continue;
        const double len = src.length();
        if (len < double(p.prune_length)) {
            // R3: a gorge this short is a dent on the wall, not a detour - two flow reversals for
            // nothing. (A short polyline with a junction in the middle would orphan its children;
            // at this length that does not happen in practice, the tree spaces its nodes far wider.)
            ++ pruned;
            continue;
        }
        Cand c;
        c.pl  = src;
        c.len = len;

        const double d_front = wall_d.distance_from_lines<false>(c.pl.points.front());
        const double d_back  = wall_d.distance_from_lines<false>(c.pl.points.back());
        const bool   front   = d_front <= d_back;
        const double d       = front ? d_front : d_back;
        if (d <= double(p.root_reach)) {
            c.is_root = true;
            auto [dist, idx, np] = wall_d.distance_from_lines_extra<false>(front ? c.pl.points.front() : c.pl.points.back());
            (void) dist; (void) idx;
            c.root_on_wall = Point(coord_t(np.x()), coord_t(np.y()));
            // R2: the tube must REACH the wall centerline, or the boolean leaves a detached island
            // instead of a detour. Half a spacing past it is enough for the difference to bite.
            extend_end(c.pl, front, d + 0.5 * double(p.spacing));
        }
        cands.emplace_back(std::move(c));
    }

    // R9: two roots closer than this fuse their mouths into one 2-spacing opening, which is exactly
    // the coverage limit of the two outer flanks - an oblique branch there leaves a slit in the skin.
    // Longest branch wins; the loser is dropped whole (it may survive as an inner loop, which the rib
    // planner welds).
    std::vector<size_t> order(cands.size());
    for (size_t i = 0; i < order.size(); ++ i)
        order[i] = i;
    std::sort(order.begin(), order.end(), [&cands](size_t a, size_t b) { return cands[a].len > cands[b].len; });

    std::vector<Point> taken;
    std::vector<bool>  keep(cands.size(), true);
    const double       gap2 = double(p.root_min_gap) * double(p.root_min_gap);
    for (size_t i : order) {
        if (! cands[i].is_root)
            continue;
        bool clash = false;
        for (const Point &t : taken)
            if ((t - cands[i].root_on_wall).cast<double>().squaredNorm() < gap2) {
                clash = true;
                break;
            }
        if (clash) {
            keep[i] = false;
            ++ dropped_roots;
        } else {
            taken.emplace_back(cands[i].root_on_wall);
            ++ gorges;
        }
    }

    Polylines out;
    out.reserve(cands.size());
    for (size_t i = 0; i < cands.size(); ++ i)
        if (keep[i])
            out.emplace_back(std::move(cands[i].pl));
    return out;
}

WallFusionResult fuse_wall_with_branches(const Polygon          &wall_loop,
                                         const Polylines        &branches,
                                         const WallFusionParams &params)
{
    WallFusionResult res;
    if (wall_loop.size() < 3 || params.spacing <= 0)
        return res;

    Polygon contour = wall_loop;
    if (contour.is_clockwise())
        contour.make_counter_clockwise();
    const ExPolygons P { ExPolygon(contour) };

    const Polylines prepared = prepare_branches(contour, branches, params,
                                                res.pruned, res.dropped_roots, res.gorges);

    ExPolygons R;
    if (prepared.empty()) {
        R = P; // no demand on this layer: the wall is exactly the wall, bit for bit
    } else {
        R = diff_ex(P, branch_tubes(prepared, params.spacing / 2));

        // R5: the cleanup opening removes slivers and rounds the mouth, but run over the WHOLE
        // region it also eats stretches of wall wherever the eroded shape pulls away from the
        // contour (measured on 400 islands: 1.0% of the skin band left uncovered against 0.2%
        // without any cleanup at all). So the opening is kept for the interior only and the
        // collar along the perimeter is put back.
        const coord_t   grow   = params.spacing / 2 - coord_t(SCALED_EPSILON);
        const ExPolygons opened = offset2_ex(R, - float(grow), float(grow));
        const ExPolygons collar = intersection_ex(R, diff_ex(P, offset_ex(P, - float(1.5 * params.spacing))));
        ExPolygons merged = opened;
        append(merged, collar);
        R = union_ex(merged);
    }

    res.loops    = to_polygons(R);
    res.interior = offset_ex(R, - float(params.spacing / 2));

    if (std::getenv("GINGER_FUSION_DEBUG") != nullptr)
        std::fprintf(stderr, "[FUSION] branches=%zu kept=%zu gorges=%zu pruned=%zu dropped_roots=%zu "
                             "loops=%zu\n",
                     branches.size(), prepared.size(), res.gorges, res.pruned, res.dropped_roots,
                     res.loops.size());
    return res;
}

} // namespace Slic3r
