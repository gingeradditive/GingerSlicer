# Clipper2 Multiline Infill Migration

Roadmap per portare in GingerSlicer la funzionalità multiline infill
basata su Clipper2 introdotta in OrcaSlicer
[PR #11435](https://github.com/OrcaSlicer/OrcaSlicer/pull/11435)
(con catena prerequisiti
[#11017](https://github.com/OrcaSlicer/OrcaSlicer/pull/11017),
[#11415](https://github.com/OrcaSlicer/OrcaSlicer/pull/11415)).

Follow-up incluso:
[PR #11765](https://github.com/OrcaSlicer/OrcaSlicer/pull/11765) —
fix in `connect_infill()` che salta le connessioni troppo corte quando
`multiline > 1`. **Non richiede Clipper2** (usa solo `params.multiline`),
quindi è applicabile in modo indipendente come quick win.

> Status: **IN PROGRESS**.
> - Fase 1 (Clipper2 dep) — **FATTO**: vendored `deps_src/clipper2`
>   (v1.5.2, mirror esatto di OrcaSlicer incluso `clipper2_z`),
>   registrato in `deps_src/CMakeLists.txt`, linkato come `Clipper2`
>   in `src/libslic3r/CMakeLists.txt`. `/WX` e `-Werror` rimossi.
>   Build target `Clipper2` OK.
> - Fase 2 (Clipper2Utils) — **FATTO**: `src/libslic3r/Clipper2Utils.{hpp,cpp}`
>   creati (mirror OrcaSlicer) e aggiunti alle sources.
> - Fase 3 (multiline_fill Clipper2) — **FATTO**: riscritta in
>   `Fill/FillBase.cpp` con `ClipperOffset(JoinType::Round, EndType::Round)`.
> - Fase 4 (bump max) — **FATTO**: `fill_multiline` `def->max = 5 → 10`
>   in `PrintConfig.cpp`. L'estensione lista UI `have_multiline_infill_pattern`
>   è **rinviata alla Fase 5** (insieme al wiring per evitare opzioni UI
>   incoerenti con pattern non ancora supportati).
> - Fase 5b (#11765) — **FATTO**: filtro skip-connessioni-corte in
>   `connect_infill()` (`Fill/FillBase.cpp`, ~riga 1712) quando
>   `params.multiline > 1`.
> - Fase 5 (estensione pattern) — **FATTO** (mirror PR #11435):
>   - `FillConcentric.cpp`: `min_spacing *= multiline` + contrazione
>     superficie `offset_ex(-0.5*(multiline-1)*spacing)` + `multiline_fill`.
>   - `FillPlanePath.cpp`: bb expand + `distance_between_lines *= multiline`
>     + `multiline_fill` (abilita Archimedean / Hilbert / Octagram).
>   - `Fill3DHoneycomb.cpp`: bb expand `5*scale_(spacing)`.
>   - `FillHoneycomb.cpp`: spacing `1.1*spacing → spacing`.
>   - `FillRectilinear.cpp` `FillQuarterCubic::fill_surface()`:
>     `line_width`/`period` scalati per `multiline`.
>   - `FillAdaptive.cpp`: branch `if (multiline==1)` (erase-collinear +
>     hooks) `else` connessione diretta.
>   - `ConfigManipulation.cpp`: lista UI estesa con `ipConcentric`,
>     `ipTriangles`, `ipQuarterCubic`, `ipArchimedeanChords`,
>     `ipHilbertCurve`, `ipOctagramSpiral`.
>   - Triangles/Grid usano la `fill_surface_by_multilines` esistente di
>     Ginger (path manuale già funzionante), NON la `fill_surface_trapezoidal`.
> - Build di verifica: `libslic3r` → OK (exit 0); `GingerSlicer.dll`
>   rilinkato OK dopo Fase 5.
> - Prossimo: Fase 6 (`fill_surface_trapezoidal` Grid/Triangles +
>   rewrite `fill_surface_by_multilines` — **alto rischio**, rinviata),
>   Fase 7 (testing pellet).
>
> Nota build: i lint clangd su `Clipper2Utils.cpp`/`FillBase.cpp`
> (`clipper2/clipper.h file not found`, `Clipper2Lib undeclared`) sono
> **falsi positivi da indice stale** — il build MSVC compila e linka.
> Rigenerare `compile_commands.json` (`scripts/gen_compile_commands.ps1`)
> per allineare clangd.

## Perché è rilevante per Ginger

Pellet usa nozzle 1.0–8.0 mm. Il parametro `fill_multiline` permette
di stampare l'infill come N linee parallele invece di una sola,
distribuendo flusso e tempo di raffreddamento. Con nozzle grandi:

- Riduce overextrusion locale (linea singola troppo larga → blob).
- Migliora ancoraggio perimetro/infill.
- Permette densità infill alte (>50%) senza overlap.

La PR upstream:
- Alza il limite `fill_multiline` da **5 a 10**.
- Sostituisce calcolo manuale di offset normali con
  `Clipper2Lib::ClipperOffset(JoinType::Round, EndType::Round)` →
  intersezioni pulite, no overlap.
- Estende il supporto multiline a 10 pattern aggiuntivi:
  Triangles, Cubic, AdaptiveCubic, QuarterCubic, ArchimedeanChords,
  Concentric, HilbertCurve, OctagramSpiral, SupportCubic, TriHexagon,
  TPMS, Lateral lattices.
- Aggiunge `FillRectilinear::fill_surface_trapezoidal()` per Grid e
  Triangles con multiline > 1 (pattern non-crossing).

---

## Stato attuale in GingerSlicer (baseline)

- `fill_multiline` esiste con `max = 5` in
  `src/libslic3r/PrintConfig.cpp` (~ line 2419).
- `multiline_fill()` in `src/libslic3r/Fill/FillBase.cpp:2704`
  calcola offset normali a mano (no Clipper2).
- Usato da: `FillRectilinear`, `FillHoneycomb`, `FillCrossHatch`.
- Workaround `remove_overlapped()` post-hoc in
  `src/libslic3r/Fill/FillRectilinear.cpp:2960`.
- **Clipper2 NON è presente** nel codebase:
  - Nessuna dir `deps/Clipper2*` o `deps_src/clipper2*`.
  - Nessuna inclusione di `clipper2/clipper.h`.
  - Nessun file `Clipper2Utils.hpp/.cpp` (esiste solo
    `ClipperUtils.hpp` per Clipper1).
- Pattern UI list in `slic3r/GUI/ConfigManipulation.cpp:604`
  (`have_multiline_infill_pattern`) include Gyroid, Grid, Rectilinear,
  TpmsD/FK, CrossHatch, Honeycomb, LateralLattice/Honeycomb, Cubic,
  Stars, AlignedRectilinear, Lightning, 3DHoneycomb, AdaptiveCubic,
  SupportCubic. Mancano: Concentric, Triangles, QuarterCubic,
  ArchimedeanChords, HilbertCurve, OctagramSpiral.

Il fork Ginger è quindi precedente all'introduzione di Clipper2 in
OrcaSlicer.

---

## Scope completo (13 file modificati upstream)

### Core (richiede Clipper2)

| File | Cambio | Note |
|------|--------|------|
| `src/libslic3r/Clipper2Utils.hpp` | + ~10 righe (decl. helpers) | **NON esiste in Ginger** |
| `src/libslic3r/Clipper2Utils.cpp` | + ~10 righe (impl.) | **NON esiste in Ginger** |
| `src/libslic3r/Fill/FillBase.cpp` | Riscrive `multiline_fill()` con `ClipperOffset(JoinType::Round, EndType::Round)` | ~70 righe modificate |

### Estensione pattern

| File | Cambio | Riga indicativa |
|------|--------|-----------------|
| `Fill/Fill3DHoneycomb.cpp` | `bb.offset(expand)` per evitare artefatti edge | ~200 |
| `Fill/FillAdaptive.cpp` | Branch `if (params.multiline == 1)` per hook handling | ~1371-1410 |
| `Fill/FillConcentric.cpp` | `min_spacing *= multiline`, contraction surface, chiama `multiline_fill()` | ~19-50 |
| `Fill/FillHoneycomb.cpp` | `1.1 * spacing → spacing` in chiamata `multiline_fill()` | ~76 |
| `Fill/FillPlanePath.cpp` | bb expand, distance scaling, chiama `multiline_fill()` (copre ArchimedeanChords, HilbertCurve, OctagramSpiral) | ~81-125 |
| `Fill/FillRectilinear.cpp` | Refactor `fill_surface_by_multilines()` (sostituisce loop manuale con `multiline_fill()` + contraction); **nuovo `fill_surface_trapezoidal()`** per Grid/Triangles con multiline>1 | ~3000-3290 (+ ~250 righe nuove) |
| `Fill/FillRectilinear.hpp` | Declaration `fill_surface_trapezoidal()` | ~29 |
| `FillQuarterCubic::fill_surface` | `line_width *= multiline`, `period *= multiline` | nel file FillRectilinear.cpp ~3144 |

### Config

| File | Cambio |
|------|--------|
| `src/libslic3r/PrintConfig.cpp` | `def->max = 5 → 10` per `fill_multiline` |
| `src/slic3r/GUI/ConfigManipulation.cpp` | Aggiunge `ipConcentric, ipTriangles, ipQuarterCubic, ipArchimedeanChords, ipHilbertCurve, ipOctagramSpiral` alla lista `have_multiline_infill_pattern` |

### Follow-up PR #11765 (indipendente da Clipper2)

| File | Cambio | Riga indicativa (Ginger) |
|------|--------|--------------------------|
| `src/libslic3r/Fill/FillBase.cpp` | In `connect_infill()`, salta le connessioni con `arc.arc_length < scale_(spacing) * params.multiline` quando `multiline > 1` | inserimento tra 1709 e 1710 |

Patch upstream:

```cpp
// Orca: If multiline infill is requested, skip connections that are too short.
if (params.multiline > 1 && arc.arc_length < scale_(spacing) * params.multiline) {
    continue;
}
```

Il contesto (righe 1707-1710 di `FillBase.cpp`) combacia esattamente
con l'HEAD attuale di Ginger.

---

## Roadmap proposta

### Fase 0 — Decisione architetturale

Opzioni per Clipper2:
- **A. Adottarlo come nuova dep**. Allinea Ginger con tutta la
  filiera upstream futura (PR seguenti useranno sempre più Clipper2).
  Costo: 1 sessione di setup + ~50 MB di sorgenti vendored.
- **B. Reimplementare l'algoritmo offset con la nostra Clipper1**.
  Risparmia la dep ma duplicheremo lavoro upstream per ogni PR
  futura. Sconsigliato strategicamente.
- **C. Implementare solo le parti non-Clipper2** (bump max, UI,
  estensioni pattern semplici). Tatticamente utile come quick win.

**Raccomandazione**: A, perché lo sforzo di Clipper2 è
una-tantum e abilita tutte le PR multiline / overhang / interface
upstream successive.

### Fase 1 — Clipper2 dep (1–2 sessioni)

1. Scaricare Clipper2 (release upstream, NON master) in
   `deps_src/Clipper2/` (snapshot vendored come fa OrcaSlicer per
   le altre dep).
2. Aggiungere build entry in `cmake/modules/` o `deps/CMakeLists`
   (replicare pattern di un'altra header-only/small lib esistente).
3. Verificare che il build `cmake --build build --target GingerSlicer
   --config Release --parallel 16` produca ancora `GingerSlicer.dll`.
4. Smoke test: `#include "clipper2/clipper.h"` in un file dummy,
   chiamare `Clipper2Lib::ClipperOffset` e verificare link.

### Fase 2 — Clipper2Utils (1 sessione)

Creare `src/libslic3r/Clipper2Utils.{hpp,cpp}` con:
- `Slic3rPolylines_to_Paths64(const Polylines&)`
- `Paths64_to_polylines(const Clipper2Lib::Paths64&)`
- `Slic3rPoints_to_Paths64(template)` (helper)

Riportare anche `intersection_pl_2`, `diff_pl_2`, `union_ex_2`
**solo se** servono al chiamante della PR #11435 (verificare).

### Fase 3 — Riscrittura `multiline_fill()` (1 sessione)

Sostituire il calcolo manuale di offset normali in
`Fill/FillBase.cpp:2704` con la versione Clipper2:
- `ClipperOffset(miter_limit=2.0)` con
  `JoinType::Round + EndType::Round`.
- Per N lines pari: offset = `{0.5s, 1.5s, 2.5s, ...}` (N/2 valori,
  applicati simmetricamente con segno alterno via due chiamate
  `Execute(+t)`).
- Per N lines dispari: include line centrale `t=0` + offset
  `{s, 2s, ...}` (N/2 valori).
- Chiudere i loop se primo punto ≠ ultimo (PR upstream lo fa).
- Filtro polylines con `< 3` punti dopo offset.

### Fase 4 — Bump limit e UI (inline, 0.5 sessione)

- `PrintConfig.cpp`: `def->max = 5 → 10`.
- `ConfigManipulation.cpp`: aggiungi 6 pattern alla lista
  `have_multiline_infill_pattern`.
- Bump profile version `3.0.0.4 → 3.0.0.5` (lo schema cambia limite).

### Fase 5 — Estensione pattern (1–2 sessioni)

Per ognuno dei seguenti file, replicare il pattern upstream
(chiamata a `multiline_fill()` + eventuali bb expand / line_width
scaling):

1. `Fill/Fill3DHoneycomb.cpp` (3 righe)
2. `Fill/FillAdaptive.cpp` (~30 righe)
3. `Fill/FillConcentric.cpp` (~10 righe)
4. `Fill/FillHoneycomb.cpp` (1 riga)
5. `Fill/FillPlanePath.cpp` (~10 righe) → abilita Archimedean,
   Hilbert, Octagram
6. `FillQuarterCubic::fill_surface()` (2 righe in
   `FillRectilinear.cpp`)

### Fase 5b — Follow-up #11765 (inline, 5 righe)

Inserire in `Fill/FillBase.cpp` `connect_infill()` (tra le righe
1709-1710 nell'HEAD attuale) il filtro che salta le connessioni troppo
corte con multiline. **Indipendente da Clipper2** — può essere applicato
in qualsiasi momento, anche subito. Va testato insieme alla Fase 5
(quando i pattern producono effettivamente multiline > 1).

### Fase 6 — `fill_surface_trapezoidal()` (1–2 sessioni)

Aggiungere il nuovo metodo a `FillRectilinear` per generare pattern
trapezoidali non-crossing per Grid e Triangles quando
`multiline > 1`. ~250 righe nuove, suddivise in:

- Branch `Pattern_type == 0` (Grid): genera pattern a row con shift
  ogni metà periodo.
- Branch `Pattern_type == 1` (Triangles): trapezoidi + base line con
  rotazione per layer.

Aggiornare `FillGrid::fill_surface()` e `FillTriangles::fill_surface()`
per dispatchare a `fill_surface_trapezoidal()` quando
`params.multiline > 1`.

### Fase 7 — Testing pellet (1–2 sessioni)

Test obbligatori, da documentare nella PR:

1. **Nozzle 4 mm, multiline 1**: baseline, deve essere identica
   pre-modifica.
2. **Nozzle 4 mm, multiline 3** su pattern Gyroid: verifica overlap
   zero con `Clipper2Lib::JoinType::Round`.
3. **Nozzle 6 mm, multiline 5** su Grid: verifica trapezoidal pattern
   (no crossings).
4. **Nozzle 6 mm, multiline 10** su Concentric: stress test densità.
5. **Pellet ERS + multiline 3**: verifica che ERS non rompa le
   transizioni multiline (`pellet_ers_mode = 1`).

---

## Rischi

- **Conflitti merge su `FillRectilinear.cpp`**: file di 3600+ righe,
  Ginger ha già modifiche pellet-specifiche. La PR upstream tocca le
  righe 2959–3290; verificare che non confliggano.
- **CMake fragile**: aggiunta di una nuova lib in `deps/` ha rotto
  altre build nel passato. Pianificare test su Windows + Linux + macOS.
- **API Clipper2 in evoluzione**: la PR usa una versione specifica.
  Vendere uno snapshot esatto, NON puntare a `master`.
- **Profile bump cascade**: cambiare `max = 10` non rompe profili
  esistenti (default = 1) ma documentarlo nel changelog.

---

## Riferimenti

- PR principale: <https://github.com/OrcaSlicer/OrcaSlicer/pull/11435>
- PR follow-up: <https://github.com/OrcaSlicer/OrcaSlicer/pull/11765>
- PR base Clipper2 (prereq): <https://github.com/OrcaSlicer/OrcaSlicer/pull/11017>
- PR intermedia (prereq): <https://github.com/OrcaSlicer/OrcaSlicer/pull/11415>
- Diff plain text #11435:
  <https://patch-diff.githubusercontent.com/raw/OrcaSlicer/OrcaSlicer/pull/11435.diff>
- Diff plain text #11765:
  <https://patch-diff.githubusercontent.com/raw/OrcaSlicer/OrcaSlicer/pull/11765.diff>
- Clipper2 upstream: <https://github.com/AngusJohnson/Clipper2>

---

## Decisioni pendenti

- [ ] Adottare Clipper2 come nuova dep (Fase 0 opzione A)?
- [ ] Prioritizzare rispetto ad altre feature pellet in arrivo?
- [ ] Versione Clipper2 da vendere (1.4.0+ raccomandato)?
- [ ] Branch name: `dado/clipper2-multiline-infill`?
- [ ] Test riproducibili: definire 5 print test in `tests/data/`?
