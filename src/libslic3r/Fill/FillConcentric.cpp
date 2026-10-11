#include "../ClipperUtils.hpp"
#include "../ExPolygon.hpp"
#include "../Flow.hpp"
#include "../Surface.hpp"
#include "../VariableWidth.hpp"
#include "Arachne/WallToolPaths.hpp"

#include "FillConcentric.hpp"
#include <libslic3r/ShortestPath.hpp>
#include <unordered_map>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace Slic3r {

// Ginger continuous_path: the ring-merging splice (FillBase.cpp). Same construction as Cura's
// PolygonConnector - two links one bead apart, both rings cut and rejoined into one - plus the
// physical validity rules (containment in the island, no retracing an existing bead).
void single_path_splice_loops(Polylines &loops, double max_link_distance, double stagger, const Polygons *island, bool barrier_only);
void single_path_splice_capture_links(Points *dst);
void single_path_splice_record_links(const Points &pts);

// Tolleranza degli archi per gli offset jtRound della copertura dei cordoni qui sotto. Per jtRound il parametro di
// offset() e' la ArcTolerance in unita' SCALATE, non un miter: con 3. (3 nm) Clipper metteva ~1100 vertici per giro
// e la misura dei buchi era il grosso del costo del confronto (knee: 15 s su 19). 5 um = ~27 vertici per giro, un
// errore d'area trascurabile rispetto alle soglie (mezzo cordone quadro).
static inline double concentric_arc_tolerance() { return scaled<double>(0.005); }

// Ginger (2026-10-04/05, Davide: "ci sono un po' di buchi nei top", Bar Stool piatto 2): il centro di un top
// concentrico. Arachne mette gli anelli finche' ci stanno; dentro l'ultimo resta un grumo (Bar Stool: 4-5 mm di
// diametro, 13-23 mm2 per sgabello, con un cordone da 3.2) in cui non nasce nessuna linea centrale - la zona e'
// tonda, non ha un asse mediano abbastanza lungo (removeSmallLines) ne' e' un cuneo stretto. Alzare
// wall_transition_angle ne chiudeva uno su quattro e moltiplicava i pezzi; il gap fill non agisce sul concentrico.
// Due tempi. PRIMA della fusione degli anelli si trovano i grumi centrali (copertura vera dei cordoni, residuo,
// solo il grumo racchiuso dall'anello PIU' INTERNO - il centro, non le fessure fra due anelli). DOPO la fusione
// il percorso che passa piu' vicino al grumo si apre su un varco di uno stagger e al suo posto fa un'ANSA che
// scende nel grumo e risale: un anello chiuso resta chiuso (con connect il top e' un anello, il router entra ed
// esce nello stesso punto) e le due braccia, a uno stagger, si affiancano come due anelli vicini.
// 2026-10-05, tre tentativi bocciati: una spirale che finiva al centro APRIVA il top; un'ansa o un anellino in
// piu' PRIMA della fusione cambiavano la geometria che la fusione usa e gli anelli non si univano piu'.

static std::vector<ConcentricCenter> find_concentric_centers(const ThickPolylines &thick, size_t first, const Polygons &region, coord_t spacing)
{
    // LOCALE, un anello piu' interno alla volta (2026-10-07: la copertura di tutta la toppa raddoppiava il tempo del
    // top concentrico sullo stool, una toppa sola da 33 m): il grumo centrale sta per definizione dentro un anello che
    // non ne contiene altri, quindi basta la copertura dei pezzi vicini a quell'anello.
    std::vector<ConcentricCenter> out;
    if (thick.size() <= first || region.empty())
        return out;
    const double w = double(spacing);
    std::vector<BoundingBox> bbs(thick.size());
    for (size_t i = first; i < thick.size(); ++ i)
        bbs[i] = BoundingBox(thick[i].points);
    for (size_t ri = first; ri < thick.size(); ++ ri) {
        const ThickPolyline &ring = thick[ri];
        if (! (ring.points.size() > 3 && ring.points.front() == ring.points.back()))
            continue;
        const Polygon pe(Points(ring.points.begin(), ring.points.end() - 1));
        // il piu' interno: nessun altro pezzo dentro
        bool nested = false;
        for (size_t i = first; i < thick.size() && ! nested; ++ i)
            if (i != ri && ! thick[i].points.empty() && bbs[ri].contains(thick[i].points.front()) && pe.contains(thick[i].points.front()))
                nested = true;
        if (nested)
            continue;
        BoundingBox win = bbs[ri];
        win.offset(coord_t(2. * w));
        Polygons cover;
        for (size_t i = first; i < thick.size(); ++ i) {
            const ThickPolyline &t = thick[i];
            if (t.points.size() < 2 || ! win.overlap(bbs[i]))
                continue;
            double wsum = 0.;
            for (const coordf_t wi : t.width)
                wsum += wi;
            const double wavg = t.width.empty() ? w : wsum / double(t.width.size());
            append(cover, offset(Polyline(t.points), float(0.5 * wavg), ClipperLib::jtRound, concentric_arc_tolerance(), ClipperLib::etOpenRound));
        }
        const Polygons region_local = intersection(region, Polygons{ win.polygon() });
        for (const ExPolygon &r : diff_ex(intersection(Polygons{ pe }, region_local), union_(cover))) {
            if (! r.holes.empty() || r.contour.size() < 3 || r.area() < 0.25 * w * w)
                continue;
            // un residuo che tocca il bordo della toppa e' uno spigolo, non il centro
            if (! diff(offset(r, float(0.25 * w)), region_local).empty())
                continue;
            // un grumo troppo sottile non vale un'ansa
            if (offset(r, float(-0.15 * w)).empty())
                continue;
            out.push_back({ r.contour.centroid().cast<double>(), r.contour.points });
        }
    }
    return out;
}

static void insert_center_hairpins(ThickPolylines &thick, size_t first, const std::vector<ConcentricCenter> &centers, coord_t spacing)
{
    const double w = double(spacing);
    auto ccw = [](const Vec2d &p, const Vec2d &q, const Vec2d &r) { return (q.x() - p.x()) * (r.y() - p.y()) - (q.y() - p.y()) * (r.x() - p.x()); };
    auto cross = [&](const Vec2d &a1, const Vec2d &a2, const Vec2d &b1, const Vec2d &b2) {
        return ccw(a1, a2, b1) * ccw(a1, a2, b2) < 0. && ccw(b1, b2, a1) * ccw(b1, b2, a2) < 0.;
    };
    for (const ConcentricCenter &cc : centers) {
        // il percorso che passa piu' vicino al centro del grumo
        int    ti = -1; size_t kq = 0; double bd = 1e300; Vec2d q = Vec2d::Zero();
        for (size_t i = first; i < thick.size(); ++ i)
            for (size_t k = 0; k + 1 < thick[i].points.size(); ++ k) {
                const Vec2d s0 = thick[i].points[k].cast<double>(), s1 = thick[i].points[k + 1].cast<double>();
                const Vec2d d = s1 - s0; const double l2 = d.squaredNorm();
                const double t = l2 <= 0. ? 0. : std::clamp((cc.c - s0).dot(d) / l2, 0., 1.);
                const Vec2d pr = s0 + d * t;
                if ((pr - cc.c).squaredNorm() < bd) { bd = (pr - cc.c).squaredNorm(); ti = int(i); kq = k; q = pr; }
            }
        if (ti < 0 || std::sqrt(bd) > 3. * w)
            continue;
        ThickPolyline &tp = thick[ti];
        if (tp.width.size() + 2 != 2 * tp.points.size())
            continue;
        const bool closed = tp.points.size() > 3 && tp.points.front() == tp.points.back();
        // si prova il varco avanti (st dopo q) e, se non regge, indietro (percorso rovesciato)
        bool done = false;
        for (int side = 0; side < 2 && ! done; ++ side) {
            ThickPolyline cand = tp;
            size_t        k0   = kq;
            const Vec2d   q0   = q;
            if (side == 1) {
                cand.reverse();
                k0 = cand.points.size() - 2 - kq;
            }
            if (closed) {
                // ruota l'anello in modo che il varco non cada a cavallo della chiusura
                const size_t n = cand.points.size() - 1, rot = (k0 + n / 2) % n;
                ThickPolyline r2;
                for (size_t m = 0; m <= n; ++ m)
                    r2.points.emplace_back(cand.points[(rot + m) % n]);
                for (size_t m = 0; m < n; ++ m) {
                    r2.width.emplace_back(cand.width[2 * ((rot + m) % n)]);
                    r2.width.emplace_back(cand.width[2 * ((rot + m) % n) + 1]);
                }
                r2.endpoints = cand.endpoints;
                k0 = (k0 + n - rot) % n;
                cand = std::move(r2);
            }
            // st: uno stagger avanti di q lungo il percorso, senza uscire dal percorso
            size_t ks = k0; Vec2d st = q0; double left = w; Vec2d cur = q0; bool ok = false;
            while (ks + 1 < cand.points.size()) {
                const Vec2d nxt = cand.points[ks + 1].cast<double>();
                const double l = (nxt - cur).norm();
                if (l >= left) { st = cur + (nxt - cur) * (left / std::max(l, 1e-9)); ok = true; break; }
                left -= l; cur = nxt; ++ ks;
            }
            if (! ok)
                continue;
            // braccia dell'ansa: verso il centro, fino a che il cordone tocca il bordo lontano del grumo
            const Vec2d mid = 0.5 * (q0 + st);
            Vec2d u = cc.c - mid;
            if (u.norm() < 0.1 * w)
                continue;
            u.normalize();
            double far = 0.;
            for (const Point &p : cc.contour)
                far = std::max(far, (p.cast<double>() - mid).dot(u));
            const double depth = std::max(far - 0.5 * w, (cc.c - mid).dot(u));
            if (depth <= 0.25 * w)
                continue;
            const Vec2d    a1 = q0 + u * (depth - (q0 - mid).dot(u));
            const Vec2d    a2 = st + u * (depth - (st - mid).dot(u));
            const coordf_t wq = cand.width[2 * k0 + 1];
            ThickPolyline out;
            for (size_t m = 0; m <= k0; ++ m)
                out.points.emplace_back(cand.points[m]);
            for (size_t m = 0; m < k0; ++ m) { out.width.emplace_back(cand.width[2 * m]); out.width.emplace_back(cand.width[2 * m + 1]); }
            const Vec2d hp[4] = { q0, a1, a2, st };
            out.points.emplace_back(hp[0].cast<coord_t>());                                              // p[k0] -> q
            out.width.emplace_back(cand.width[2 * k0]); out.width.emplace_back(cand.width[2 * k0 + 1]);
            for (int h = 1; h < 4; ++ h) {
                out.points.emplace_back(hp[h].cast<coord_t>());
                out.width.emplace_back(wq); out.width.emplace_back(wq);
            }
            out.points.emplace_back(cand.points[ks + 1]);                                                // st -> p[ks+1]
            out.width.emplace_back(cand.width[2 * ks]); out.width.emplace_back(cand.width[2 * ks + 1]);
            for (size_t m = ks + 1; m + 1 < cand.points.size(); ++ m) {
                out.points.emplace_back(cand.points[m + 1]);
                out.width.emplace_back(cand.width[2 * m]); out.width.emplace_back(cand.width[2 * m + 1]);
            }
            out.endpoints = cand.endpoints;
            if (out.width.size() + 2 != 2 * out.points.size())
                continue;
            // l'ansa non deve incrociare niente: ne' il percorso stesso ne' gli altri, entro 4 cordoni dal centro
            // vicino = il SEGMENTO passa entro 4 cordoni dal centro (non solo un suo estremo: sullo stool gli anelli
            // hanno lati lunghi e dritti che attraversano la zona dell'ansa con gli estremi lontani)
            const double rr2 = sqr(4. * w);
            auto near_seg = [&](const Vec2d &p0, const Vec2d &p1) {
                const Vec2d d = p1 - p0; const double l2 = d.squaredNorm();
                const double t = l2 <= 0. ? 0. : std::clamp((cc.c - p0).dot(d) / l2, 0., 1.);
                return (p0 + d * t - cc.c).squaredNorm() < rr2;
            };
            std::vector<std::pair<Vec2d, Vec2d>> segs;
            std::vector<int>                     own;
            for (size_t m = 0; m + 1 < out.points.size(); ++ m) {
                const Vec2d p0 = out.points[m].cast<double>(), p1 = out.points[m + 1].cast<double>();
                if (near_seg(p0, p1)) { segs.emplace_back(p0, p1); own.push_back(int(m)); }
            }
            for (size_t i = first; i < thick.size(); ++ i)
                if (int(i) != ti)
                    for (size_t m = 0; m + 1 < thick[i].points.size(); ++ m) {
                        const Vec2d p0 = thick[i].points[m].cast<double>(), p1 = thick[i].points[m + 1].cast<double>();
                        if (near_seg(p0, p1)) { segs.emplace_back(p0, p1); own.push_back(-1); }
                    }
            bool bad = false;
            for (size_t x = 0; x < segs.size() && ! bad; ++ x)
                for (size_t y = x + 1; y < segs.size() && ! bad; ++ y) {
                    if (own[x] < 0 && own[y] < 0)
                        continue;
                    if (own[x] >= 0 && own[y] >= 0 && std::abs(own[x] - own[y]) <= 1)
                        continue;
                    bad = cross(segs[x].first, segs[x].second, segs[y].first, segs[y].second);
                }
            if (bad)
                continue;
            if (side == 1)
                out.reverse();
            tp   = std::move(out);
            done = true;
        }
    }
}

// Ginger (2026-10-07): la toppa ha un pezzo APERTO (il cordone di mezzo di un numero dispari di cordoni, sull'asse
// mediano)? La fusione degli anelli non lo puo' chiudere: il top finirebbe al centro con un travel.
// Aree della regione NON coperta dai cordoni di due candidati A e B, nella finestra `region` (allargata verso il
// muro dal chiamante). Ogni tratto con la SUA larghezza, a gradini di 0.25 mm (la larghezza media di un pezzo
// sottostima la copertura dei pezzi a larghezza variabile, e la forcina lo e' molto), tratti consecutivi dello stesso
// gradino uniti in una polilinea (a segmenti singoli il top dello stool costava 697 s invece di 8). La copertura dei
// segmenti COMUNI ai due candidati (stessi estremi, in qualunque verso) si calcola una volta sola; a ciascuno si
// tolgono poi solo i suoi segmenti diversi da un residuo piccolo. Vale sui cordoni grezzi e sul risultato della
// fusione (si confrontano segmenti, non pezzi). Stool, 33 m di top: 560 -> ~250 ms per confronto.
static std::pair<double, double> concentric_uncovered_pair(const ThickPolylines &A, const ThickPolylines &B, const Polygons &region_in, double spacing)
{
    // chiave di un segmento: estremi (in qualunque verso) E larghezza a gradini di 0.25 mm - la seconda corsa di Arachne
    // puo' tenere gli stessi punti e cambiare la larghezza (knee: contandoli comuni si perdevano 15 decisioni su 493)
    const double wq = scaled<double>(0.25);
    auto seg_key_w = [wq](const Point &p, const Point &q, coordf_t w0, coordf_t w1) {
        const Point &a = (p.x() < q.x() || (p.x() == q.x() && p.y() <= q.y())) ? p : q;
        const Point &b = (&a == &p) ? q : p;
        const size_t wk = size_t(std::lround(0.5 * (w0 + w1) / wq));
        return (size_t(a.x()) * 73856093u) ^ (size_t(a.y()) * 19349663u) ^ (size_t(b.x()) * 83492791u) ^ (size_t(b.y()) * 2654435761u) ^ (wk * 0x9e3779b97f4a7c15ull);
    };
    auto seg_key = [&seg_key_w](const ThickPolyline &t, size_t k) {
        return 2 * k + 1 < t.width.size() ? seg_key_w(t.points[k], t.points[k + 1], t.width[2 * k], t.width[2 * k + 1]) :
                                            seg_key_w(t.points[k], t.points[k + 1], 0., 0.);
    };
    std::unordered_map<size_t, int> mult; // +1 per A, +2 per B: 3 = comune
    for (const ThickPolylines *v : { &A, &B })
        for (const ThickPolyline &t : *v)
            for (size_t k = 0; k + 1 < t.points.size(); ++ k)
                mult[seg_key(t, k)] |= (v == &A ? 1 : 2);
    // Nessun segmento diverso: stessa copertura. (La finestra NON si restringe all'intorno dei segmenti diversi: in
    // teoria i buchi lontani si annullano, in pratica il conto sovrastimava di qualche decimo di mm2 e sul knee
    // invertiva 15 decisioni al limite, che nel G-code non aggiungevano buchi.)
    Polylines diff_segs;
    for (const ThickPolylines *v : { &A, &B })
        for (const ThickPolyline &t : *v)
            for (size_t k = 0; k + 1 < t.points.size(); ++ k)
                if (mult[seg_key(t, k)] != 3)
                    diff_segs.emplace_back(Polyline(Points{ t.points[k], t.points[k + 1] }));
    if (diff_segs.empty())
        return { 0., 0. };
    const Polygons &region = region_in;
    std::vector<BoundingBox> comps;
    for (const ExPolygon &e : union_ex(region)) {
        BoundingBox bb = get_extents(e.contour);
        bb.offset(coord_t(spacing));
        comps.emplace_back(bb);
    }
    auto in_win = [&comps](const Point &a, const Point &b) {
        BoundingBox sb(Points{ a, b });
        for (const BoundingBox &c : comps)
            if (c.overlap(sb))
                return true;
        return false;
    };
    // copertura dei segmenti della classe `cls` (0 = comuni, 1 = solo A, 2 = solo B) di un candidato
    auto cover_of = [&](const ThickPolylines &v, int cls) {
        std::unordered_map<int, Polylines> buckets;
        const double q = scaled<double>(0.25);
        for (const ThickPolyline &t : v) {
            Polyline run;
            int      run_key = -1;
            for (size_t k = 0; k + 1 < t.points.size() && 2 * k + 1 < t.width.size(); ++ k) {
                const int  m      = mult[seg_key(t, k)];
                const int  seg_cl = m == 3 ? 0 : (m == 1 ? 1 : 2);
                const int  key    = (seg_cl == cls && in_win(t.points[k], t.points[k + 1])) ? int(std::lround(0.5 * (t.width[2 * k] + t.width[2 * k + 1]) / q)) : -2;
                if (key != run_key) {
                    if (run.points.size() >= 2 && run_key >= 0)
                        buckets[run_key].emplace_back(std::move(run));
                    run.points.clear();
                    run.points.emplace_back(t.points[k]);
                    run_key = key;
                }
                run.points.emplace_back(t.points[k + 1]);
            }
            if (run.points.size() >= 2 && run_key >= 0)
                buckets[run_key].emplace_back(std::move(run));
        }
        Polygons cover;
        for (auto &[k, pls] : buckets)
            if (k > 0)
                append(cover, offset(pls, float(0.5 * k * q), ClipperLib::jtRound, concentric_arc_tolerance(), ClipperLib::etOpenRound));
        return cover;
    };
    const ExPolygons base = diff_ex(region, union_(cover_of(A, 0)));
    auto area_of = [](const ExPolygons &es) { double a = 0.; for (const ExPolygon &e : es) a += e.area(); return a; };
    return { area_of(diff_ex(base, union_(cover_of(A, 1)))), area_of(diff_ex(base, union_(cover_of(B, 2)))) };
}

// Lunghezza dei tratti piu' stretti di min_w (un estremo sotto soglia basta).
static double concentric_narrow_length(const ThickPolylines &thick, double min_w)
{
    double len = 0.;
    for (const ThickPolyline &t : thick)
        for (size_t k = 0; k + 1 < t.points.size() && 2 * k + 1 < t.width.size(); ++ k)
            if (std::min(t.width[2 * k], t.width[2 * k + 1]) < min_w)
                len += (t.points[k + 1] - t.points[k]).cast<double>().norm();
    return len;
}

static bool concentric_has_open_piece(const ThickPolylines &thick, size_t first)
{
    for (size_t i = first; i < thick.size(); ++ i)
        if (thick[i].points.size() >= 2 && thick[i].points.front() != thick[i].points.back())
            return true;
    return false;
}

// Fuse the concentric rings of ONE patch into a single loop. The rings come from Arachne as
// ThickPolylines (variable width); the splice works on plain Polylines, so the widths are carried
// over to the fused path by nearest source point - printing a merged top at a constant width would
// leave voids where the patch tapers and over-extrude where it widens.
static void splice_concentric_rings(ThickPolylines &thick, size_t first, const Polygons &island,
                                    double spacing, coord_t min_spacing)
{
    if (thick.size() < first + 2)
        return;
    // Width samples from every source ring: one per segment endpoint.
    struct WSample { Point p; coordf_t w; };
    std::vector<WSample> samples;
    Polylines            rings;
    for (size_t i = first; i < thick.size(); ++ i) {
        const ThickPolyline &tp = thick[i];
        if (tp.points.size() < 2 || tp.width.size() + 2 < tp.points.size() * 2)
            return; // malformed: leave the patch untouched
        for (size_t k = 0; k + 1 < tp.points.size(); ++ k) {
            samples.push_back({ tp.points[k],     tp.width[2 * k] });
            samples.push_back({ tp.points[k + 1], tp.width[2 * k + 1] });
        }
        rings.emplace_back(tp.points);
    }
    if (samples.empty())
        return;
    // Sonda (2026-09-19): i capi dei cordoni APERTI in ingresso. Se combaciano a due a due sono lo
    // stesso cordone spezzato in un punto di diramazione, e basta ricucirli prima della splice.
    if (::getenv("GINGER_TOPSPLICE_ENDS") != nullptr)
        for (const Polyline &pl : rings)
            if (! (pl.points.size() > 3 && pl.points.front() == pl.points.back()))
                std::fprintf(stderr, "[TOPENDS] len=%.2f a=(%.3f,%.3f) b=(%.3f,%.3f)\n",
                             pl.length() * SCALING_FACTOR,
                             pl.points.front().x() * SCALING_FACTOR, pl.points.front().y() * SCALING_FACTOR,
                             pl.points.back().x()  * SCALING_FACTOR, pl.points.back().y()  * SCALING_FACTOR);
    const size_t rings_before = rings.size();
    size_t closed_in = 0;
    for (const Polyline &pl : rings)
        if (pl.points.size() > 3 && pl.points.front() == pl.points.back())
            ++ closed_in;
    // Link ceiling, in ring pitches. Arachne rings have a VARIABLE pitch (variable width), so where
    // the patch tapers two neighbouring rings sit further apart than one pitch and the merge is
    // refused. Tunable for the A/B; the splice itself caps a straight ring-ring link at 3 staggers,
    // so values above 3 do nothing. Cura hits the same wall: with connect_skin_polygons it still
    // leaves 3.04 pieces per patch (from 4.70), i.e. it fuses about a third.
    static const double link_mult = [] {
        const char *e = ::getenv("GINGER_TOPSPLICE_LINK");
        const double v = e == nullptr ? 1.5 : ::atof(e);
        return v > 0. ? v : 1.5;
    }();
    // The island (from the caller) is the outward-offset outline Arachne walled, so the outermost
    // ring centerline lies inside it and a link along it passes the containment test.
    single_path_splice_loops(rings, scale_(link_mult * spacing), scale_(spacing), &island, false);
    if (::getenv("GINGER_TOPSPLICE_DEBUG") != nullptr) {
        // Composizione dell'USCITA: serve a distinguere le due letture possibili del residuo -
        // "i chiusi non si fondono fino in fondo" oppure "i chiusi si fondono e restano gli aperti".
        size_t out_closed = 0;
        for (const Polyline &pl : rings)
            if (pl.points.size() > 3 && pl.points.front() == pl.points.back())
                ++ out_closed;
        std::fprintf(stderr, "[TOPSPLICE] uscita: %zu chiusi + %zu aperti\n", out_closed, rings.size() - out_closed);
    }
    if (::getenv("GINGER_TOPSPLICE_DEBUG") != nullptr)
        std::fprintf(stderr, "[TOPSPLICE] anelli=%zu (chiusi=%zu) -> %zu  link_max=%.2f interassi\n",
                     rings_before, closed_in, rings.size(), link_mult);
    if (rings.size() >= rings_before)
        return; // nothing merged: keep the Arachne widths exactly as they were
    // Nearest-sample lookup on a uniform grid (cell = one ring pitch). A linear scan here would be
    // O(samples * output points) per patch - the same quadratic shape that already cost 52s on the
    // stool once; with 20 rings of 200 points that is 16M distance tests per patch per layer.
    const coord_t cell = std::max<coord_t>(min_spacing, coord_t(SCALED_EPSILON) * 16);
    std::unordered_map<int64_t, std::vector<uint32_t>> grid;
    auto key_of = [cell](const Point &p) -> int64_t {
        const int64_t cx = int64_t(std::floor(double(p.x()) / double(cell)));
        const int64_t cy = int64_t(std::floor(double(p.y()) / double(cell)));
        return (cx << 32) ^ (cy & 0xffffffffLL);
    };
    for (uint32_t i = 0; i < uint32_t(samples.size()); ++ i)
        grid[key_of(samples[i].p)].push_back(i);
    auto width_at = [&samples, &grid, cell](const Point &p) -> coordf_t {
        const int64_t cx = int64_t(std::floor(double(p.x()) / double(cell)));
        const int64_t cy = int64_t(std::floor(double(p.y()) / double(cell)));
        double   best = std::numeric_limits<double>::max();
        coordf_t w    = samples.front().w;
        // Widening rings of cells: a source point sits in the same cell (distance 0) and a link
        // endpoint within one ring pitch, so ring 1 nearly always answers; the loop is a guard.
        for (int ring = 1; ring <= 4; ++ ring) {
            for (int64_t dx = -ring; dx <= ring; ++ dx)
                for (int64_t dy = -ring; dy <= ring; ++ dy) {
                    if (ring > 1 && std::llabs(dx) != ring && std::llabs(dy) != ring)
                        continue; // interior already visited
                    auto it = grid.find(((cx + dx) << 32) ^ ((cy + dy) & 0xffffffffLL));
                    if (it == grid.end())
                        continue;
                    for (uint32_t idx : it->second) {
                        const double d = (samples[idx].p - p).cast<double>().squaredNorm();
                        if (d < best) { best = d; w = samples[idx].w; }
                    }
                }
            if (best < std::numeric_limits<double>::max())
                break; // a hit in this ring of cells cannot be beaten by more than one cell
        }
        return w;
    };
    thick.erase(thick.begin() + first, thick.end());
    for (Polyline &pl : rings) {
        if (pl.points.size() < 2)
            continue;
        ThickPolyline tp;
        tp.points = std::move(pl.points);
        tp.width.reserve((tp.points.size() - 1) * 2);
        for (size_t k = 0; k + 1 < tp.points.size(); ++ k) {
            tp.width.emplace_back(width_at(tp.points[k]));
            tp.width.emplace_back(width_at(tp.points[k + 1]));
        }
        thick.emplace_back(std::move(tp));
    }
}

void FillConcentric::_fill_surface_single(
    const FillParams                &params, 
    unsigned int                     thickness_layers,
    const std::pair<float, Point>   &direction, 
    ExPolygon                        expolygon,
    Polylines                       &polylines_out)
{
    // no rotation is supported for this infill pattern
    BoundingBox bounding_box = expolygon.contour.bounding_box();
    
    coord_t min_spacing = scale_(this->spacing) * params.multiline;
    coord_t distance = coord_t(min_spacing / params.density);
    
    if (params.density > 0.9999f && !params.dont_adjust) {
        distance = this->_adjust_solid_spacing(bounding_box.size()(0), distance);
        this->spacing = unscale<double>(distance);
    }

    // Contract surface polygon by half line width to avoid excesive overlap with perimeter
    ExPolygons contracted = offset_ex(expolygon, -float(scale_(0.5 * (params.multiline - 1) * this->spacing )));

    Polygons loops = to_polygons(contracted);

    ExPolygons last { std::move(contracted) };
    while (! last.empty()) {
        last = offset2_ex(last, -(distance + min_spacing/2), +min_spacing/2);
        append(loops, to_polygons(last));
    }

    // generate paths from the outermost to the innermost, to avoid
    // adhesion problems of the first central tiny loops
    loops = union_pt_chained_outside_in(loops);
    
    // split paths using a nearest neighbor search
    size_t iPathFirst = polylines_out.size();
    Point last_pos(0, 0);
    for (const Polygon &loop : loops) {
        polylines_out.emplace_back(loop.split_at_index(last_pos.nearest_point_index(loop.points)));
        last_pos = polylines_out.back().last_point();
    }

    // Apply multiline offset if needed
    multiline_fill(polylines_out, params, spacing);

    // clip the paths to prevent the extruder from getting exactly on the first point of the loop
    // Keep valid paths only.
    size_t j = iPathFirst;
    for (size_t i = iPathFirst; i < polylines_out.size(); ++ i) {
        // Ginger continuous_path: keep the ring CLOSED (split_at_index duplicates the split point
        // at both ends). A closed ring becomes an ExtrusionLoop downstream, which the G-code router
        // may enter anywhere - so its exit coincides with its entry and the return travel that a
        // clipped, open ring pays (the distance between its two ends) disappears. The seam gap is
        // applied at emission time by extrude_loop, so not clipping here never re-extrudes a seam.
        if (! params.connect_polygons)
            polylines_out[i].clip_end(this->loop_clipping);
        if (polylines_out[i].is_valid()) {
            if (j < i)
                polylines_out[j] = std::move(polylines_out[i]);
            ++ j;
        }
    }
    if (j < polylines_out.size())
        polylines_out.erase(polylines_out.begin() + j, polylines_out.end());
    //TODO: return ExtrusionLoop objects to get better chained paths,
    // otherwise the outermost loop starts at the closest point to (0, 0).
    // We want the loops to be split inside the G-code generator to get optimum path planning.
}

void FillConcentric::_fill_surface_single(const FillParams& params,
    unsigned int                   thickness_layers,
    const std::pair<float, Point>& direction,
    ExPolygon                      expolygon,
    ThickPolylines& thick_polylines_out)
{
    assert(params.use_arachne);
    assert(this->print_config != nullptr && this->print_object_config != nullptr);

    // no rotation is supported for this infill pattern
    Point   bbox_size = expolygon.contour.bounding_box().size();
    coord_t min_spacing = scaled<coord_t>(this->spacing);

    if (params.density > 0.9999f && !params.dont_adjust) {
        coord_t                loops_count = std::max(bbox_size.x(), bbox_size.y()) / min_spacing + 1;
        Polygons               polygons = offset(expolygon, float(min_spacing) / 2.f);

        double min_nozzle_diameter = *std::min_element(print_config->nozzle_diameter.values.begin(), print_config->nozzle_diameter.values.end());
        Arachne::WallToolPathsParams input_params;
        input_params.min_bead_width = 0.85 * min_nozzle_diameter;
        input_params.min_feature_size = 0.25 * min_nozzle_diameter;
        input_params.wall_transition_length = 1.0 * min_nozzle_diameter;
        input_params.wall_transition_angle = 10;
        input_params.wall_transition_filter_deviation = 0.25 * min_nozzle_diameter;
        input_params.wall_distribution_count = 1;
        // Ginger (2026-10-09, poppot, Davide: "dare piu' margine di spessore per fittare con anelli pari"):
        // concentric_even_beads = con connect, numero di cordoni sempre PARI (EvenBeadingStrategy): anelli chiusi, nessun
        // cordone dispari aperto al centro; il candidato split non serve piu'. Larghezze dal process (concentric_*).
        const PrintRegionConfig *rc = params.config;
        const bool even_beads = params.connect_polygons && rc != nullptr && rc->concentric_even_beads.value;
        const Arachne::WallToolPathsParams base_params = input_params; // i parametri di sempre (regola delle schegge)
        if (even_beads) {
            input_params.even_max_bead_width = float(rc->concentric_max_bead_width.get_abs_value(min_nozzle_diameter));
            input_params.even_min_pair_width = float(rc->concentric_min_hairpin_width.get_abs_value(min_nozzle_diameter));
            input_params.even_band           = std::max(2, rc->concentric_band_beads.value);
            // un avvallamento dello spessore di dT sposta ogni cordone della banda di dT / banda: il filtro delle
            // transizioni puo' spianarne di piu' (pizzicature della coppia centrale)
            input_params.wall_transition_filter_deviation = float(0.25 * min_nozzle_diameter * 0.5 * input_params.even_band);
        }
        Arachne::WallToolPaths wallToolPaths(polygons, min_spacing, min_spacing, loops_count, 0, params.layer_height, input_params);

        std::vector<Arachne::VariableWidthLines>    loops = wallToolPaths.getToolPaths();
        std::vector<const Arachne::ExtrusionLine*> all_extrusions;
        for (Arachne::VariableWidthLines& loop : loops) {
            if (loop.empty())
                continue;
            for (const Arachne::ExtrusionLine& wall : loop)
                all_extrusions.emplace_back(&wall);
        }

        // Split paths using a nearest neighbor search.
        size_t firts_poly_idx = thick_polylines_out.size();
        Point  last_pos(0, 0);
        for (const Arachne::ExtrusionLine* extrusion : all_extrusions) {
            if (extrusion->empty())
                continue;

            ThickPolyline thick_polyline = Arachne::to_thick_polyline(*extrusion);
            if (extrusion->is_closed)
                thick_polyline.start_at_index(last_pos.nearest_point_index(thick_polyline.points));
            thick_polylines_out.emplace_back(std::move(thick_polyline));
            last_pos = thick_polylines_out.back().last_point();
        }

        // Ginger (2026-10-07, Davide: "piuttosto del buco meglio stampare un cordone piu' stretto che riempie, senza
        // spezzare il path"). Solo con connect e solo nelle toppe dove Arachne ha lasciato un pezzo APERTO: si rifa'
        // Arachne con la soglia di divisione del cordone di mezzo abbassata (il trucco di Cura, min_even_wall_line_width:
        // split = 2 * minimo / larghezza - 1, minimo 0.55 ugello). Il cordone di mezzo di un numero dispari si divide in
        // due cordoni piu' stretti che fanno una FORCINA chiusa, e la fusione la aggancia al resto: il top resta un anello.
        // Cura 5.10 con questo trucco porta le barre a centro dispari da 1 anello + 1 linea aperta (travel 4.5-14.7 mm)
        // a 1 anello chiuso, stesso volume e stessi buchi. SOLO dove serve (verifica dell'agente, 2026-10-07): applicato
        // a tutte le toppe, i centri tondi che gia' si chiudevano lasciavano anellini interni staccati (Bar Stool piatto 2
        // z 807.5: un anello da 576 mm diventava 4 pezzi) e sulle schegge senza top nascevano pezzetti nuovi.
        // Le altre soglie restano quelle di prima: cambiarle tutte insieme (2026-10-06) dava cordoni fino a 1.77 mm.
        // Non sulle schegge: Fill::fill_surface_extrusion scarta una superficie connessa il cui riempimento e' piu'
        // corto di 2.5 larghezze, e la forcina raddoppia la lunghezza della linea di mezzo - la scheggia passerebbe
        // e verrebbe stampata (verifica 2026-10-07: 13 pezzetti nuovi sul piatto 2, su strati che prima non avevano top).
        ThickPolylines patch_spliced;            // fusione gia' fatta per la toppa (se c'e' stato il confronto)
        bool           patch_spliced_set = false;
        double patch_len = 0.;
        for (size_t i = firts_poly_idx; i < thick_polylines_out.size(); ++ i)
            patch_len += thick_polylines_out[i].length();
        // Conteggio pari: una scheggia che con i parametri di sempre e' UNA linea (e se sotto 2.5 larghezze non si stampa:
        // Fill::fill_surface_extrusion) qui diventa una forcina chiusa, piu' lunga, e passava il filtro (verifica
        // 2026-10-09: +18/+20 layer di top sul poppot p4, mentre una soglia fissa sull'anello scartava forcine che la
        // base stampava - le due forcine nascono in modo diverso e nessun rapporto di lunghezza le lega). Quindi, per una
        // toppa che e' UNA forcina chiusa (toppe minuscole, costa poco), si rifa' Arachne con i parametri di sempre e si
        // decide come la base: si scarta se la sua corsa e' piu' corta di 2.5 larghezze.
        if (even_beads && thick_polylines_out.size() == firts_poly_idx + 1 &&
            thick_polylines_out.back().points.front() == thick_polylines_out.back().points.back()) {
            Arachne::WallToolPaths base_paths(polygons, min_spacing, min_spacing, loops_count, 0, params.layer_height, base_params);
            double base_len = 0.;
            for (const Arachne::VariableWidthLines &vl : base_paths.getToolPaths())
                for (const Arachne::ExtrusionLine &l : vl)
                    if (! l.empty())
                        base_len += Arachne::to_thick_polyline(l).length();
            if (base_len < 2.5 * double(params.flow.scaled_width())) {
                thick_polylines_out.resize(firts_poly_idx);
                return;
            }
        }
        // Non sul primo layer (aderenza: un bottom a cordoni stretti attacca peggio). E la seconda corsa e' solo una
        // CANDIDATA: si fa la fusione di prova su entrambe e la si tiene solo se migliora davvero la toppa - meno pezzi
        // aperti piu' pezzi in tutto, buchi (sulla fusione) non peggiori di mezzo cordone quadro, pochi cordoni sotto
        // 0.55 ugello. Verifica 2026-10-07 (agente): senza questo controllo in alcune toppe la seconda corsa peggiorava
        // (mozzo del Bar Stool piatto 2 z 139.3: un anello -> 3 pezzi; piatto 1 z 2.8: linea divisa a meta' e un travel
        // nuovo di 13 mm; stool z 355.9: un buco di 90 mm2; cordoni a 1.46 mm sul primo layer del bottom).
        if (params.connect_polygons && ! even_beads && concentric_has_open_piece(thick_polylines_out, firts_poly_idx) &&
            patch_len >= 2.5 * double(params.flow.scaled_width()) && this->layer_id != 0) {
            Arachne::WallToolPathsParams split_params = input_params;
            const float ext_width = Flow::rounded_rectangle_extrusion_width_from_spacing(unscale<float>(min_spacing), float(params.layer_height));
            split_params.split_middle_threshold = std::clamp(float(2. * 0.55 * min_nozzle_diameter / ext_width - 1.), 0.01f, 0.99f);
            Arachne::WallToolPaths split_paths(polygons, min_spacing, min_spacing, loops_count, 0, params.layer_height, split_params);
            std::vector<Arachne::VariableWidthLines> split_loops = split_paths.getToolPaths();
            ThickPolylines split_cand;
            Point          split_last(0, 0);
            double         split_minw = std::numeric_limits<double>::max();
            for (const Arachne::VariableWidthLines &vl : split_loops)
                for (const Arachne::ExtrusionLine &l : vl) {
                    if (l.empty())
                        continue;
                    ThickPolyline tp = Arachne::to_thick_polyline(l);
                    if (l.is_closed)
                        tp.start_at_index(split_last.nearest_point_index(tp.points));
                    for (const coordf_t w : tp.width)
                        split_minw = std::min(split_minw, double(w));
                    split_cand.emplace_back(std::move(tp));
                    split_last = split_cand.back().last_point();
                }
            ThickPolylines normal_cand(thick_polylines_out.begin() + int(firts_poly_idx), thick_polylines_out.end());
            auto keep_valid = [](ThickPolylines &v) {
                v.erase(std::remove_if(v.begin(), v.end(), [](const ThickPolyline &t) { return ! t.is_valid(); }), v.end());
            };
            keep_valid(normal_cand);
            keep_valid(split_cand);
            // il risultato di prova di chi vince diventa quello definitivo (niente terza fusione). Comprende l'ansa del
            // centro: e' lei a coprire il centro, e senza nel confronto un buco coperto dall'ansa della corsa normale
            // non si vedeva (Bar Stool piatto 2 z 572.2: misurato -0.6 mm2, nel G-code +7.5)
            // i raccordi scelti da una fusione di PROVA non vanno nell'isteresi del layer sopra: si raccolgono a parte
            // e si registrano solo quelli del candidato tenuto
            auto spliced = [&](ThickPolylines cand, size_t &pieces, size_t &open, Points &links) {
                const std::vector<ConcentricCenter> cand_centers = find_concentric_centers(cand, 0, polygons, min_spacing);
                {
                    struct CaptureGuard {
                        explicit CaptureGuard(Points *dst) { single_path_splice_capture_links(dst); }
                        ~CaptureGuard() { single_path_splice_capture_links(nullptr); }
                    } capture(&links);
                    splice_concentric_rings(cand, 0, offset(expolygon, float(min_spacing) / 2.f), this->spacing, min_spacing);
                }
                if (! cand_centers.empty())
                    insert_center_hairpins(cand, 0, cand_centers, min_spacing);
                pieces = cand.size();
                open   = 0;
                for (const ThickPolyline &t : cand)
                    if (t.points.size() >= 2 && t.points.front() != t.points.back())
                        ++ open;
                return cand;
            };
            size_t pn = 0, on = 0, ps = 0, os = 0;
            Points links_n, links_s;
            ThickPolylines spliced_n = spliced(normal_cand, pn, on, links_n);
            const double w_nom   = double(min_spacing);
            // i buchi si confrontano in una FINESTRA intorno ai cordoni che le due corse posano in modo DIVERSO (la toppa
            // intera, stool 33 m, costava piu' dello slicing). Non intorno ai soli pezzi aperti: la seconda corsa sposta
            // anche cordoni lontani (stool: nella finestra dei pezzi aperti -24 mm2, sulla toppa intera +35 mm2).
            auto line_key = [](const ThickPolyline &t) {
                size_t h = t.points.size();
                for (const Point &pt : t.points)
                    h = h * 1000003u ^ (size_t(pt.x()) * 2654435761u) ^ (size_t(pt.y()) * 40503u);
                return h;
            };
            std::unordered_map<size_t, int> seen;
            for (const ThickPolyline &t : normal_cand) ++ seen[line_key(t)];
            for (const ThickPolyline &t : split_cand)  -- seen[line_key(t)];
            Polylines changed;
            for (const ThickPolylines *cv : { &normal_cand, &split_cand })
                for (const ThickPolyline &t : *cv)
                    if (seen[line_key(t)] != 0)
                        changed.emplace_back(Polyline(t.points));
            // regione allargata di mezzo cordone verso il muro: il buco del piatto 2 z 572.2 stava fra i cordoni del top e
            // il muro, fuori dal contorno di Arachne (il muro pesa uguale nei due candidati, il confronto resta giusto)
            const Polygons window   = intersection(offset(polygons, float(0.5 * w_nom)), offset(changed, float(2. * w_nom)));
            // sul risultato della FUSIONE, cioe' su quello che va nel G-code: la fusione aggiunge raccordi e ne taglia,
            // e sui cordoni grezzi un buco coperto da un raccordo della corsa normale non si vedeva (verifica
            // 2026-10-07: Bar Stool piatto 2 z 572.2, misurato +0.4 mm2, nel G-code +7.5)
            // scarto preliminare sui cordoni GREZZI: se i buchi crescono gia' nettamente (oltre 4 volte la soglia) la
            // fusione non li richiude, e si risparmiano la seconda fusione e la misura finale (stool: grezzo e fuso
            // danno lo stesso delta, +23..+202 mm2; la fusione del top da 33 m costa ~0.4 s, la misura grezza ~0.05 s.
            // Sul knee invece quasi nessun candidato si scarta qui: 3 su 492, e la misura costa 0.4 s in tutto)
            const auto [raw_n, raw_s] = concentric_uncovered_pair(normal_cand, split_cand, window, w_nom);
            const bool pre_reject = raw_s > raw_n + 2. * w_nom * w_nom;
            ThickPolylines spliced_s;
            double unc_n = raw_n, unc_s = raw_s;
            if (! pre_reject) {
                spliced_s = spliced(split_cand, ps, os, links_s);
                std::tie(unc_n, unc_s) = concentric_uncovered_pair(spliced_n, spliced_s, window, w_nom);
            }
            const double narrow  = concentric_narrow_length(split_cand, 0.45 * scaled<double>(min_nozzle_diameter));
            double split_len = 0.;
            for (const ThickPolyline &t : split_cand)
                split_len += t.length();
            // I cordoni grezzi di Arachne hanno capi stretti alle transizioni anche nella corsa normale: non conta il
            // minimo assoluto ma quanta lunghezza scende sotto 0.45 ugello (al massimo un passo, o lo 0.2% della toppa:
            // il top dello stool e' una toppa sola da 33 m).
            const bool   better  = ! pre_reject && ! split_cand.empty() &&
                                   // la SOMMA pezzi aperti + pezzi deve scendere: un pezzo aperto in meno pagato con un anello
                                   // in piu' e' un pareggio (mozzo del piatto 2 z 139.3: 1 aperto -> 2 chiusi, scartata)
                                   (os + ps < on + pn) &&
                                   narrow <= std::max(w_nom, 0.002 * split_len) &&
                                   unc_s <= unc_n + 0.5 * w_nom * w_nom;
            if (static const bool split_dbg = ::getenv("GINGER_SPLIT_DEBUG") != nullptr; split_dbg) { // diagnostica: una riga per toppa candidata
                const double cn = unc_n, cs = unc_s;
                std::fprintf(stderr, "[SPLITDBG] L%zu aperti %zu->%zu pezzi %zu->%zu minw %.2f stretti %.1fmm scoperto %.1f->%.1f (%+.1f mm2) => %s%s\n",
                             this->layer_id, on, os, pn, ps, split_minw * SCALING_FACTOR, narrow * SCALING_FACTOR, cn * SCALING_FACTOR * SCALING_FACTOR, cs * SCALING_FACTOR * SCALING_FACTOR,
                             (cs - cn) * SCALING_FACTOR * SCALING_FACTOR, better ? "TENUTA" : "scartata", pre_reject ? " (scarto preliminare)" : "");
            }
            thick_polylines_out.erase(thick_polylines_out.begin() + int(firts_poly_idx), thick_polylines_out.end());
            append(thick_polylines_out, better ? std::move(split_cand) : std::move(normal_cand));
            patch_spliced     = better ? std::move(spliced_s) : std::move(spliced_n);
            // differita: i raccordi li registra la fusione della superficie
            if (! m_defer_splice)
                single_path_splice_record_links(better ? links_s : links_n);
            patch_spliced_set = true;
        }

        // clip the paths to prevent the extruder from getting exactly on the first point of the loop
        // Keep valid paths only.
        size_t j = firts_poly_idx;
        for (size_t i = firts_poly_idx; i < thick_polylines_out.size(); ++i) {
            // Ginger continuous_path: see the polyline branch above - a closed thick polyline makes
            // variable_width() emit an ExtrusionLoop instead of loose ExtrusionPaths.
            if (! params.connect_polygons)
                thick_polylines_out[i].clip_end(this->loop_clipping);
            if (thick_polylines_out[i].is_valid()) {
                if (j < i)
                    thick_polylines_out[j] = std::move(thick_polylines_out[i]);
                ++j;
            }
        }
        if (j < thick_polylines_out.size())
            thick_polylines_out.erase(thick_polylines_out.begin() + int(j), thick_polylines_out.end());

        // Ginger continuous_path: N closed rings are N loop units for the router, i.e. N-1 ring-to-ring
        // hops. Cura pairs concentric with connect_skin_polygons for exactly this reason - the pattern
        // alone is not the point, fusing its rings into one path is. Measured on plate 3 before this:
        // the closed rings cut Top->Sparse by 12.7 m but Top->Top grew by 12.3 m over 5021 hops.
        // centro del top: solo superfici piene (top, bottom, solido), non lo sparse concentrico
        if (m_defer_splice) {
            // pezzo di una superficie divisa: restano i cordoni (del candidato tenuto) e i loro centri, la fusione e le
            // anse le fa fill_surface_arachne su tutti i pezzi insieme
            append(m_deferred_centers, find_concentric_centers(thick_polylines_out, firts_poly_idx, polygons, min_spacing));
        } else if (patch_spliced_set) {
            // la toppa e' gia' stata fusa (e con l'ansa del centro) nel confronto fra le due corse
            thick_polylines_out.erase(thick_polylines_out.begin() + int(firts_poly_idx), thick_polylines_out.end());
            append(thick_polylines_out, std::move(patch_spliced));
        } else {
            std::vector<ConcentricCenter> centers;
            if (params.density > 0.9999f)
                centers = find_concentric_centers(thick_polylines_out, firts_poly_idx, polygons, min_spacing);

            if (params.connect_polygons)
                splice_concentric_rings(thick_polylines_out, firts_poly_idx, offset(expolygon, float(min_spacing) / 2.f), this->spacing, min_spacing);

            // l'ansa del centro DOPO la fusione: non tocca la geometria che la fusione usa
            if (! centers.empty())
                insert_center_hairpins(thick_polylines_out, firts_poly_idx, centers, min_spacing);
        }

        reorder_by_shortest_traverse(thick_polylines_out);
    }
    else {
        Polylines polylines;
        this->_fill_surface_single(params, thickness_layers, direction, expolygon, polylines);
        append(thick_polylines_out, to_thick_polylines(std::move(polylines), min_spacing));
    }
}

// Ginger (2026-10-09, sedile racing, Davide: "se una feature e' in una superficie chiusa quale meccanismo non assicura
// l'unione di tutto il path concentrico?"). Fill::fill_surface_arachne ritira la superficie di mezzo passo (l'area degli
// assi dei cordoni, come per ogni pattern) e riempie ogni ExPolygon risultante da solo; _fill_surface_single la riallarga
// per Arachne. Un collo piu' stretto di un passo sparisce in quell'apertura e la superficie arriva a pezzi - giusto per
// Arachne (nel collo non c'e' posto per un anello), sbagliato per la fusione, che girava per pezzo e non vedeva mai gli
// anelli dell'altro: sul sedile 23 isole su 38 col top spezzato venivano da qui (54 toppe su 274 tagliate), con gli anelli
// esterni dei due pezzi a 2.9-3.0 mm, un cordone, attraverso il taglio. Ora Arachne e il candidato split restano per pezzo,
// la fusione e le anse del centro si fanno una volta sulla superficie, con la superficie stessa (piu' l'overlap) come
// isola. Superficie in un pezzo solo: identica a prima.
ThickPolylines FillConcentric::fill_surface_arachne(const Surface *surface, const FillParams &params)
{
    ExPolygons expp = offset_ex(surface->expolygon, float(scale_(this->overlap - 0.5 * this->spacing)));
    const bool per_surface = params.connect_polygons && params.use_arachne && params.density > 0.9999f && ! params.dont_adjust &&
                             expp.size() > 1;
    ThickPolylines out;
    m_defer_splice = per_surface;
    m_deferred_centers.clear();
    for (ExPolygon &expoly : expp)
        _fill_surface_single(params, surface->thickness_layers, _infill_direction(surface), std::move(expoly), out);
    m_defer_splice = false;
    if (per_surface && ! out.empty()) {
        const coord_t min_spacing = scaled<coord_t>(this->spacing);
        splice_concentric_rings(out, 0, offset(surface->expolygon, float(scale_(this->overlap))), this->spacing, min_spacing);
        if (! m_deferred_centers.empty())
            insert_center_hairpins(out, 0, m_deferred_centers, min_spacing);
        m_deferred_centers.clear();
        reorder_by_shortest_traverse(out);
    }
    return out;
}

} // namespace Slic3r
