#include "../ClipperUtils.hpp"
#include <mutex>
#include <string>
#include "../Print.hpp"
#include "../ShortestPath.hpp"
#include "FillBase.hpp"
#include "FillLightning.hpp"
#include "Lightning/Generator.hpp"

namespace Slic3r { void single_path_splice_begin_layer(long layer_id); }

// Sonda GINGER_LN_DEBUG: le isole si riempiono in parallelo (TBB) e fprintf spezza le righe lunghe
// a meta' (buffer stdio): i poligoni di due thread si mescolavano sul log. Una riga intera per
// fwrite, sotto mutex.
static void ln_dump_line(const std::string &line)
{
    static std::mutex mtx;
    std::lock_guard<std::mutex> lock(mtx);
    std::fwrite(line.data(), 1, line.size(), stderr);
    std::fflush(stderr);
}
static void ln_dump_points(const char *tag, size_t layer_id, const Slic3r::Points &pts, const char *prefix = nullptr)
{
    std::string line = "[" + std::string(tag) + "] " + (prefix ? prefix : "") + "layer=" + std::to_string(layer_id) + " n=" + std::to_string(pts.size()) + ":";
    char buf[64];
    for (const Slic3r::Point &p : pts) {
        std::snprintf(buf, sizeof(buf), " %.2f,%.2f", p.x() * SCALING_FACTOR, p.y() * SCALING_FACTOR);
        line += buf;
    }
    line += "\n";
    ln_dump_line(line);
}

namespace Slic3r::FillLightning {

bool Filler::surface_in_fused_island(const ExPolygon &surface) const
{
    if (fused_islands == nullptr || fused_islands->empty() || surface.contour.points.empty())
        return false;
    // A sparse surface lies strictly inside the wall centerline of its own island (the fill
    // boundary is pulled in by half a spacing, less the wall overlap) and never straddles two
    // islands, so a single boundary point decides.
    const Point &probe = surface.contour.points.front();
    for (const Polygon &wall : *fused_islands)
        if (wall.contains(probe))
            return true;
    return false;
}

void Filler::_fill_surface_single(
    const FillParams              &params,
    unsigned int                   thickness_layers,
    const std::pair<float, Point> &direction,
    ExPolygon                      expolygon,
    Polylines                     &polylines_out)
{
    const Layer &layer      = generator->getTreesForLayer(this->layer_id);
    Polylines    fill_lines = layer.convertToLines(to_polygons(expolygon), scaled<coord_t>(0.5 * this->spacing - this->overlap));
    static const bool ln_dbg_all = ::getenv("GINGER_LN_DEBUG") != nullptr;
    if (ln_dbg_all) {
        const BoundingBox bb = get_extents(expolygon);
        char hdr[256];
        std::snprintf(hdr, sizeof(hdr), "[LNISLE] layer=%zu bbox=(%.0f,%.0f)-(%.0f,%.0f) alberi=%zu area=%.0f\n", size_t(this->layer_id),
                      bb.min.x() * SCALING_FACTOR, bb.min.y() * SCALING_FACTOR, bb.max.x() * SCALING_FACTOR, bb.max.y() * SCALING_FACTOR,
                      fill_lines.size(), expolygon.area() * SCALING_FACTOR * SCALING_FACTOR);
        ln_dump_line(hdr);
        ln_dump_points("LNISLEC", size_t(this->layer_id), expolygon.contour.points);
        for (const Polygon &h : expolygon.holes) ln_dump_points("LNISLEH", size_t(this->layer_id), h.points);
    }

    // Ginger (2026-09-06, Davide): lightning a ml=2 in single path "alla Cura". Oggi l'anello attorno
    // a ogni albero (multiline_fill) viene RIAPERTO dal ritaglio sull'area di sparse - alla radice e
    // ovunque un ramo corra accanto al muro - e il connettore deve richiuderlo lungo la parete: sul
    // figure plate 2 ci riesce una volta su tre (355 chiusi su 1755), il resto sono percorsi aperti,
    // anellini isolati attorno ai monconi e ponti nel vuoto. CuraEngine (multiplyInfill con
    // zig_zaggify, il profilo di Davide) lavora per AREE: contorno interno meno la banda attorno agli
    // alberi. I bordi delle tasche che restano sono anelli chiusi per costruzione, e includono da soli
    // i tratti lungo il muro fra un albero e l'altro (la lining). Poi il PolygonConnector fonde gli
    // anelli i cui fianchi corrono entro mezza linea: qui lo fa la splice. Sonda: GINGER_LN_POCKETS=1.
    // Default ON dal 2026-09-06 (decisione di Davide): misurato su knee, figure plate 2/3 e stool LN80
    // sempre pari o meglio del connettore (capi nel vuoto 0, aria ~0, lining sul 98% del contorno).
    // GINGER_LN_POCKETS=0 torna al percorso vecchio (multiline_fill + ritaglio + connettore).
    static const bool ln_pockets = [] { const char *v = ::getenv("GINGER_LN_POCKETS"); return v == nullptr || std::atoi(v) != 0; }();
    // Non nelle isole fuse (continuous_path_infill_as_wall): li' il muro E' l'anello e la lining lungo il
    // contorno, che le tasche portano con se', sarebbe un secondo cordone accanto a un cordone di
    // parete - salvo che l'utente la chieda su ogni layer (ring_always).
    if (ln_pockets && params.connect_polygons && params.multiline == 2 && ! fill_lines.empty() &&
        (! this->surface_in_fused_island(expolygon) || params.ring_always)) {
        // banda attorno agli alberi: i suoi due bordi sono le due rotaie, a +-mezzo spacing (giunti e
        // capi tondi, come l'offset Clipper2 di multiline_fill)
        const float half_band = float(scale_(0.5 * this->spacing));
        Polygons    band      = offset(fill_lines, half_band, ClipperLib::jtRound, 3., ClipperLib::etOpenRound);
        // contorno interno: l'area di sparse STESSA. Cura arretra di mezza linea perche' il suo
        // inner_contour e' il fianco interno del muro; qui il bordo dell'area e' gia' l'ASSE della
        // lining (arretrato di mezzo spacing meno l'overlap, vedi surface_in_fused_island) ed e'
        // dove il connettore del vecchio percorso posa il cordone lungo il muro. Un ulteriore
        // arretramento di mezzo spacing (misurato sul knee, layer 166) staccava la lining dal muro
        // (2.31 mm dall'asse della parete contro 1.45) e cancellava i bracci piu' stretti di uno
        // spacing: -14% di sparse (333 -> 285 m) senza nessun anello al loro posto.
        ExPolygons  inner     { expolygon };
        // Sonda GINGER_LN_OPEN=1: apertura morfologica di mezzo cordone. I bracci dell'area piu'
        // stretti di un cordone (knee: 0.3-1.7 mm con w=1.9) ricevono due sponde che si
        // sovrappongono - ri-estrusione, che il vecchio percorso fa gia' (8.7% dello sparse a
        // interasse < 1.35 mm). Con l'apertura quei bracci spariscono dall'area e le due sponde
        // restano solo dove stanno almeno un cordone distanti.
        static const bool ln_open = ::getenv("GINGER_LN_OPEN") != nullptr;
        if (ln_open) {
            const float half_w = float(scale_(0.5 * params.flow.width()));
            inner = offset2_ex(ExPolygons{ expolygon }, -half_w, +half_w);
        }
        ExPolygons  pockets   = diff_ex(inner, band);
        static const bool ln_dbg = ::getenv("GINGER_LN_DEBUG") != nullptr;
        if (ln_dbg) {
            char hdr[256];
            std::snprintf(hdr, sizeof(hdr), "[LNPOCK] layer=%zu alberi=%zu banda=%zu inner=%zu tasche=%zu\n", size_t(this->layer_id), fill_lines.size(), band.size(), inner.size(), pockets.size());
            ln_dump_line(hdr);
            const size_t lid = size_t(this->layer_id);
            for (const Polyline &pl : fill_lines) ln_dump_points("LNTREE", lid, pl.points);
            for (const Polygon &p : band)        ln_dump_points("LNBAND", lid, p.points);
            for (const ExPolygon &ex : inner)    { ln_dump_points("LNINNER", lid, ex.contour.points); for (const Polygon &h : ex.holes) ln_dump_points("LNINNERHOLE", lid, h.points); }
            ln_dump_points("LNAREA", lid, expolygon.contour.points);
            for (const Polygon &h : expolygon.holes) ln_dump_points("LNAREAHOLE", lid, h.points);
        }
        Polylines   rings;
        const double min_len = scale_(2. * this->spacing); // Cura: poligoni piu' corti di 2 linee non si stampano
        auto push_ring = [&](const Polygon &p) {
            if (p.size() < 3 || p.length() < min_len)
                return;
            // Clipper restituisce archi con punti a 0.01 mm e duplicati consecutivi (6236 punti per
            // 706 mm): si ripuliscono prima della splice, che lavora con distanze e proiezioni.
            Polygon q = p;
            q.remove_duplicate_points();
            Polygons qs = q.simplify(scale_(0.02));
            if (qs.empty() || qs.front().size() < 3)
                return;
            Polyline pl(qs.front().points);
            pl.points.emplace_back(qs.front().points.front());
            rings.emplace_back(std::move(pl));
        };
        for (const ExPolygon &ex : pockets) {
            push_ring(ex.contour);
            for (const Polygon &h : ex.holes)
                push_ring(h);
        }
        if (! rings.empty()) {
            if (ln_dbg) {
                ln_dump_line("[LNPOCK]   anelli=" + std::to_string(rings.size()) + " -> splice\n");
                for (size_t k = 0; k < rings.size(); ++ k) {
                    char pre[64];
                    std::snprintf(pre, sizeof(pre), "%zu len=%.1f ", k, rings[k].length() * SCALING_FACTOR);
                    ln_dump_points("LNRING", size_t(this->layer_id), rings[k].points, pre);
                }
            }
            // Splice SENZA isola, come nel ramo grid. Misurato sul plate 2 (layer 60-260): con l'isola
            // (area ridotta o intera) la validazione dei raccordi vicino al muro, dove stanno le
            // rotaie della lining, ne rifiuta la meta' - 7.6 unita/layer e 39% di isole spezzate
            // contro 3.8 e 21% senza. GINGER_LN_ISLAND=1 la passa (sonda), GINGER_LN_NOSPLICE=1 salta
            // la fusione. Il rischio di un raccordo sopra un top va misurato a parte (i raccordi sono
            // lunghi un cordone, fra fianchi adiacenti; Cura unisce solo poligoni entro mezza linea).
            static const bool ln_nosplice = ::getenv("GINGER_LN_NOSPLICE") != nullptr;
            // GINGER_LN_ISLAND=1: isola verbatim (misurata peggio: i raccordi lungo il muro cadono
            // sul contorno, caso collineare degenere del clip -> "fuori isola"). =2 (2026-09-06,
            // figure plate 3 di Davide): isola DILATATA di mezzo cordone, come island_region_grown
            // del connettore: i raccordi sul bordo passano, un raccordo che attraversa una parete
            // interna (due anelli ai lati opposti di un muro: layer 93, due rotaie attraverso il
            // muro) resta fuori di almeno un cordone e viene rifiutato.
            // Default (2026-09-06): isola dilatata come BARRIERA (barrier_only). GINGER_LN_ISLAND=0
            // nessuna isola, =1 isola verbatim, =3 isola dilatata con tutte le regole della splice.
            static const int ln_island = [] { const char *v = ::getenv("GINGER_LN_ISLAND"); return v ? std::atoi(v) : 2; }();
            if (! ln_nosplice) {
                Polygons island;
                if (ln_island == 1)
                    island = to_polygons(expolygon);
                else if (ln_island >= 2)
                    // Dilatazione minima: quanto basta perche' un raccordo che corre SUL contorno non
                    // sia collineare col bordo (caso degenere del clip). Con mezzo cordone la barriera
                    // arrivava a 0.4 mm dall'asse della parete e lasciava passare raccordi sopra il
                    // muro (plate 3, layer 139: due rotaie da 10 mm sul cordone di parete).
                    island = offset(expolygon, float(scale_(0.1 * params.flow.width())));
                single_path_splice_begin_layer(long(this->layer_id));
                single_path_splice_loops(rings, scale_(4. * this->spacing * params.multiline), scale_(this->spacing), ln_island ? &island : nullptr, ln_island == 2);
            }
            if (ln_dbg) ln_dump_line("[LNPOCK]   dopo splice=" + std::to_string(rings.size()) + "\n");
            append(polylines_out, std::move(rings));
            return;
        }
    }

    // Apply multiline offset if needed
    multiline_fill(fill_lines, params, spacing);

    if (params.multiline > 1)
        fill_lines = intersection_pl(std::move(fill_lines), expolygon);

    // Ginger single-path: guarantee the wall-hugging "lining" loop. Lightning is demand-driven,
    // so on layers with little demand above only a lone tree survives and the welded walk
    // shrinks to a stub - the inner lining bead (the "second wall") that every other layer has
    // disappears for a band of layers (banding on the inner surface) and the walk loses the
    // rail that carries it to the wall seam (rib). The connector then prefers the contour
    // phase with maximum wall coverage whenever it costs no extra trail.
    //
    // EXCEPT where continuous_path_infill_as_wall already turned the wall into that ring. There the
    // surface contour is no longer the island outline: the fusion carved a gorge out of it for
    // every branch it took over, so "maximum wall coverage" makes the lining trace the outline of
    // each gorge - a second bead 0.75 spacings from a flank that is itself a wall bead. That is
    // where the fusion's material was going (stool: wall +6.0 m on layer 4 while the fill gave
    // back only 1.2 m). The wall IS the ring here, which is exactly what the option's tooltip
    // promises, so the lining preference is dropped for those islands only.
    const bool fused = this->surface_in_fused_island(expolygon);

    // continuous_path_infill_ring_always: with no fill line at all the connector has nothing to connect
    // and emits nothing - that is the band of layers with no second wall (stool: 366 layers of 527
    // print no sparse whatsoever, in runs of up to 100). The ring is then laid down by itself, on
    // the very boundary the lining walks when a tree is there, so the two are the same bead in the
    // same place; a lone closed loop needs no connector and costs no travel. Not under the fusion:
    // there the wall already is that ring.
    if (fill_lines.empty()) {
        // Under the fusion the surface only survives when the ring was asked for on every layer,
        // and then its contour already goes around each carved gorge: walking it IS that ring.
        if (params.ring_always) {
            polylines_out.emplace_back(expolygon.contour.split_at_first_point());
            for (const Polygon &hole : expolygon.holes)
                polylines_out.emplace_back(hole.split_at_first_point());
        }
        return;
    }

    FillParams lining_params = params;
    // In a fused island the ring is normally dropped (the wall is the ring), unless the user asks
    // for one on every layer: then it is wanted here too, and it is the connector's job to walk it
    // around the gorges the fusion carved out of the boundary.
    lining_params.sparse_wall_lining = ! fused || params.ring_always;
    chain_or_connect_infill(std::move(fill_lines), expolygon, polylines_out, this->spacing, lining_params);
}

void GeneratorDeleter::operator()(Generator *p) {
    delete p;
}

GeneratorPtr build_generator(const PrintObject &print_object, const std::function<void()> &throw_on_cancel_callback)
{
    return GeneratorPtr(new Generator(print_object, throw_on_cancel_callback));
}

} // namespace Slic3r::FillAdaptive
