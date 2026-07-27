#include "WallFusion.hpp"

#include "ClipperUtils.hpp"
#include "Clipper2Utils.hpp"
#include "AABBTreeLines.hpp"
#include "BoundingBox.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <limits>
#include <numeric>

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
static Polylines prepare_branches(const ExPolygon        &island,
                                  const Polylines        &in,
                                  const WallFusionParams &p,
                                  size_t                 &pruned,
                                  size_t                 &dropped_roots,
                                  size_t                 &gorges,
                                  size_t                 &roots_before_prune)
{
    // Every ring of the island counts as wall: a branch rooted on the boundary of a HOLE opens its
    // gorge in the hole's loop, exactly like one rooted on the outer contour. Treating the outer
    // contour as the only wall is what used to leave the holed islands (the stool's base: an
    // annulus with spokes) to the infill, 26 layers of 527 with sparse the merge could not absorb.
    Lines ring_lines = island.contour.lines();
    for (const Polygon &hole : island.holes)
        append(ring_lines, hole.lines());
    AABBTreeLines::LinesDistancer<Line> wall_d(ring_lines);

    struct Cand {
        Polyline pl;
        double   len           = 0.;
        bool     is_root       = false;
        bool     root_at_front = false;
        double   root_dist     = 0.;
        Point    root_on_wall;
    };
    std::vector<Cand> cands;
    cands.reserve(in.size());

    for (const Polyline &src : in) {
        if (src.size() < 2)
            continue;
        const double len = src.length();
        {   // census before pruning: how many anchors were there to remove in the first place
            const double df = wall_d.distance_from_lines<false>(src.points.front());
            const double db = wall_d.distance_from_lines<false>(src.points.back());
            if (std::min(df, db) <= double(p.root_reach))
                ++ roots_before_prune;
        }
        if (p.prune_length > 0 && len < double(p.prune_length)) {
            // R3, off by default: a gorge this short is a dent on the wall rather than a detour.
            // Pruning here is not free - the branch is not printed by anyone else - so it only
            // happens when someone explicitly asks for it.
            ++ pruned;
            continue;
        }
        Cand c;
        c.pl  = src;
        c.len = len;

        const double d_front = wall_d.distance_from_lines<false>(c.pl.points.front());
        const double d_back  = wall_d.distance_from_lines<false>(c.pl.points.back());
        c.root_at_front      = d_front <= d_back;
        c.root_dist          = c.root_at_front ? d_front : d_back;
        if (c.root_dist <= double(p.root_reach)) {
            c.is_root = true;
            auto [dist, idx, np] = wall_d.distance_from_lines_extra<false>(c.root_at_front ? c.pl.points.front() : c.pl.points.back());
            (void) dist; (void) idx;
            c.root_on_wall = Point(coord_t(np.x()), coord_t(np.y()));
        }
        cands.emplace_back(std::move(c));
    }

    // R9: two roots closer than this fuse their mouths into one 2-spacing opening, which is exactly
    // the coverage limit of the two outer flanks - an oblique branch there leaves a slit in the skin.
    // Longest branch wins; the loser is DEMOTED, not dropped. Its material stays (the tube is still
    // subtracted, so it comes back as an inner ring for the rib planner to weld) but it is not
    // extended to the centerline, so it opens no second mouth. Dropping it used to leave the branch
    // to the infill, which no longer exists in a fused island.
    std::vector<size_t> order(cands.size());
    for (size_t i = 0; i < order.size(); ++ i)
        order[i] = i;
    std::sort(order.begin(), order.end(), [&cands](size_t a, size_t b) { return cands[a].len > cands[b].len; });

    std::vector<Point> taken;
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
            cands[i].is_root = false;
            ++ dropped_roots;
        } else {
            taken.emplace_back(cands[i].root_on_wall);
            ++ gorges;
        }
    }

    // R10, nothing may float. Whatever the tubes do not connect to a wall comes back as a closed
    // ring in the middle of the island, and a ring that small (measured: 13 mm of perimeter, a
    // 3x5 mm dot) is below what the rib planner can weld - so it prints on its own, between two
    // travels. On the stool's layer 7 that was 45 dots and 54 extra travels. Two ways in: a tip
    // that stops just outside root_reach (median 8 mm from the wall - the tree ends where the fill
    // boundary is, not where the wall is), and a root R9 has just demoted. Both get linked here:
    // to the neighbouring branch when one is closer (no second mouth, R9 still honoured), to the
    // wall otherwise. The bead it costs is a few mm; the travel it saves is the whole point of
    // single path.
    {
        std::vector<AABBTreeLines::LinesDistancer<Line>> cand_d;
        cand_d.reserve(cands.size());
        for (const Cand &c : cands)
            cand_d.emplace_back(c.pl.lines());

        // Components of the tube graph: two branches are one piece when their tubes overlap.
        std::vector<size_t> parent(cands.size());
        std::iota(parent.begin(), parent.end(), size_t(0));
        std::function<size_t(size_t)> find = [&parent, &find](size_t i) {
            return parent[i] == i ? i : parent[i] = find(parent[i]);
        };
        auto pl_distance = [&cand_d](const Polyline &pl, size_t j) {
            double best = std::numeric_limits<double>::max();
            for (const Point &pt : pl.points)
                best = std::min(best, cand_d[j].distance_from_lines<false>(pt));
            return best;
        };
        // Both directions: a child branch ENDS on its parent, and the parent has no vertex there -
        // testing one way only left a whole sub-tree looking unanchored.
        auto touching = [&pl_distance, &cands, &p](size_t i, size_t j) {
            return std::min(pl_distance(cands[i].pl, j), pl_distance(cands[j].pl, i)) <= double(p.spacing);
        };
        for (size_t i = 0; i < cands.size(); ++ i)
            for (size_t j = i + 1; j < cands.size(); ++ j)
                if (find(i) != find(j) && touching(i, j))
                    parent[find(i)] = find(j);

        std::vector<char> anchored(cands.size(), 0);
        for (size_t i = 0; i < cands.size(); ++ i)
            if (cands[i].is_root)
                anchored[find(i)] = 1;

        for (size_t i = 0; i < cands.size(); ++ i) {
            if (anchored[find(i)])
                continue;
            // Cheapest way out of the void, for the whole component: this branch's end either
            // reaches a wall or reaches a branch that already does.
            const bool   front   = cands[i].root_at_front;
            const Point &tip     = front ? cands[i].pl.points.front() : cands[i].pl.points.back();
            double       d_other = std::numeric_limits<double>::max();
            Point        target;
            for (size_t j = 0; j < cands.size(); ++ j)
                if (find(j) != find(i) && anchored[find(j)]) {
                    auto [d, idx, np] = cand_d[j].distance_from_lines_extra<false>(tip);
                    (void) idx;
                    if (d < d_other) {
                        d_other = d;
                        target  = Point(coord_t(np.x()), coord_t(np.y()));
                    }
                }
            if (d_other <= double(p.spacing)) {
                anchored[find(i)] = 1;   // already touching an anchored piece: nothing to do
            } else if (d_other <= cands[i].root_dist) {
                // Link to the neighbour: a straight segment, half a spacing past it so the tubes
                // certainly merge. One mouth for the pair, which is what R9 asked for.
                const Vec2d dir = (target - tip).cast<double>();
                const double n  = dir.norm();
                const Point  end = n > SCALED_EPSILON
                                 ? target + (dir * (0.5 * double(p.spacing) / n)).cast<coord_t>() : target;
                if (front)
                    cands[i].pl.points.insert(cands[i].pl.points.begin(), end);
                else
                    cands[i].pl.points.emplace_back(end);
                anchored[find(i)] = 1;
            } else {
                // Nothing but wall in reach: open a mouth there, whatever R9 would have said - a
                // second mouth beats a floating dot.
                cands[i].is_root  = true;
                anchored[find(i)] = 1;
                ++ gorges;
            }
        }
    }

    Polylines out;
    out.reserve(cands.size());
    for (Cand &c : cands) {
        // R2: a root's tube must REACH the wall centerline, or the boolean leaves a detached island
        // instead of a detour. Half a spacing past it is enough for the difference to bite. Done
        // here and not before R9, so a demoted root keeps its original ends.
        if (c.is_root)
            extend_end(c.pl, c.root_at_front, c.root_dist + 0.5 * double(p.spacing));
        out.emplace_back(std::move(c.pl));
    }
    return out;
}

WallFusionResult fuse_wall_with_branches(const ExPolygon        &island,
                                         const Polylines        &branches,
                                         const WallFusionParams &params)
{
    WallFusionResult res;
    if (island.contour.size() < 3 || params.spacing <= 0)
        return res;

    ExPolygon shape = island;
    if (shape.contour.is_clockwise())
        shape.contour.make_counter_clockwise();
    for (Polygon &hole : shape.holes)
        if (hole.is_counter_clockwise())
            hole.make_clockwise();
    const ExPolygons P { shape };

    const Polylines prepared = prepare_branches(shape, branches, params,
                                                res.pruned, res.dropped_roots, res.gorges,
                                                res.roots_before_prune);

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

    res.loops          = to_polygons(R);
    res.interior       = offset_ex(R, - float(params.spacing / 2));
    // Nothing was refused: the caller may drop the island's sparse fill altogether.
    res.left_to_infill = res.pruned;
    if (! prepared.empty())
        res.carve = branch_tubes(prepared, coord_t(0.75 * double(params.spacing)));

    if (std::getenv("GINGER_FUSION_DEBUG") != nullptr) {
        BoundingBox in_bb = get_extents(shape.contour);
        BoundingBox out_bb;
        for (const Polygon &p : res.loops)
            out_bb.merge(get_extents(p));
        std::fprintf(stderr, "[FUSION] branches=%zu kept=%zu roots=%zu gorges=%zu pruned=%zu dropped_roots=%zu "
                             "loops=%zu bbox_in=[%.1f %.1f %.1f %.1f] bbox_out=[%.1f %.1f %.1f %.1f]\n",
                     branches.size(), prepared.size(), res.roots_before_prune, res.gorges, res.pruned, res.dropped_roots,
                     res.loops.size(),
                     unscale<double>(in_bb.min.x()), unscale<double>(in_bb.min.y()),
                     unscale<double>(in_bb.max.x()), unscale<double>(in_bb.max.y()),
                     out_bb.defined ? unscale<double>(out_bb.min.x()) : 0.,
                     out_bb.defined ? unscale<double>(out_bb.min.y()) : 0.,
                     out_bb.defined ? unscale<double>(out_bb.max.x()) : 0.,
                     out_bb.defined ? unscale<double>(out_bb.max.y()) : 0.);
    }
    return res;
}

} // namespace Slic3r
