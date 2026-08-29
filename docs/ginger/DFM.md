# Design for Manufacturing — Pellet FGF in GingerSlicer

Internal engineering reference: the DFM principles that pellet printing imposes, and how
GingerSlicer implements or automates each of them. Written for people working on the slicer;
for terminology see `GLOSSARY.md`, for the flow-dynamics model see `EXTRUSION_DYNAMICS.md`.

> Scope: large-format screw extruders (beads 1–8 mm, Ginger profiles). Everything here follows
> from four physical facts and the four directives they force.

---

## 1. Prime directives

Pellet FGF differs from desktop FDM in four physical facts:

1. **Beads are huge** (typ. 3.2 mm wide × 1.2–1.5 mm high). One bead is a structural member;
   one flow error is a visible defect. Sparse infill at 5 % puts beads ~70 mm apart, so
   "the layer below" is *usually void*, not support.
2. **Pressure response is slow** (melt time constant τ ≈ 0.15–0.5 s, flow-dependent — see
   `EXTRUSION_DYNAMICS.md`). Flow steps cannot be followed; every discontinuity smears
   material somewhere it should not be.
3. **There is no true retraction.** "Retract" is screw decompression: slow, partial, and it
   degrades the melt sitting in the barrel. Every travel move risks stringing and an
   oozing restart.
4. **Melt degrades while idle.** Long travels are not just wasted time: the material that
   waits in the nozzle prints worse.

These force four directives that the rest of this document instantiates:

- **D1 — Minimize travels.** The ideal layer is ONE continuous extrusion path.
- **D2 — Prefer extrusion over travel.** A bead is cheaper than a hop: material use is
  explicitly not a concern ("preferiamo estrudere"). Corollary: never extrude *twice over
  the same line* (retrace), and never extrude *outside the part*. Canonical form of the
  retrace rule ("stesso interasse"): two same-layer beads whose centerlines run parallel
  closer than one width are doubled material — the violation. Flank contact at exactly one
  width is *fusion* and is the goal (rib link pairs, sparse anchors along walls); transversal
  point crossings are a separate, softer class (geometric integrity of holes/walls); and
  vertical stacking across layers is just FDM — columns are built on it.
- **D3 — Keep flow continuous.** Paths should chain with matched flow; ERS handles what
  geometry cannot (feedrates re-quantized at 0.1 mm/s — 1 mm/s is mm³/s-scale error here).
- **D4 — Every bead must be sustainable.** Support below (wall, rib column, solid) or a
  foundation grown on purpose; sparse infill is *not* support.

---

## 2. Travel minimization — the single path (`single_path_mode`)

Goal: per island and per layer, walls + sparse infill print as one continuous walk.

### 2.1 The Euler connector
Sparse scanlines (chords) + the island boundary form a graph: chord endpoints are vertices on
the contour ring, boundary stretches between consecutive endpoints are *gap arcs*. A selection
of gaps where every vertex keeps exactly one of its two gaps is an *alternating phase*; odd
vertices ("defects") are the open ends of the walk. Pieces emitted = connected components;
2k defects in a component = k open trails.
File: `src/libslic3r/Fill/FillBase.cpp`, `connect_infill_single_path()`.

### 2.2 Exact min-pieces solve (single-contour islands)
Key theorem used: a selection with 0 or 2 defects is **fully determined** by the defect pair
plus one phase bit, so the whole space is `2 + 2·C(m,2)` candidates — enumerable exactly
(m ≤ 160 gate). Cost order: pieces → blocked gaps → defects → mouth span; on the final
lightning-lining emission, ties break toward the selection covering MORE wall (the "second
wall" lining bead — D2). The greedy passes cannot reach many optima (cost rises before it
falls between k closed loops and one open trail; measured 9 pieces greedy vs 1 exact on the
H-section part) and, worse, their landing spot depends on the contour's point-0 rotation on
degenerate inputs: identical three-fragment lightning layers of the D02 flipped between the
1-trail wall-hugging loop and two closed blobs reached by 130–360 mm travels — which is why
the solver also runs under wall lining now. When the wall-hug phase would take a *blocked*
arc (riding it would retrace a fragment), the solver correctly settles for the best clean
1-trail selection: anchored arcs plus one mouth travel, never a doubled bead.
Openness is often *forced*: on an H section the two trail ends provably sit at the far leg
tips (exhaustive: 9 valid selections of 2862, mouth ≥ 350 mm) — an open trail costs ~nothing
when consecutive layers alternate direction and the wall seam lands on the entry.

### 2.3 Physical link rules (bridges, joins, welds)
Every artificial connection is an extruded bead governed by D2, not by a length policy:

- **allowed at any length**, provided it (a) lies entirely **inside the island**
  (`intersection_ln` against the boundary polygons) and (b) does **not retrace** an extruded
  line: parallel (<25°) closer than 0.8 bead accumulated over more than 1.5 bead — the same
  coincidence metric as `gap_blocked`;
- links up to **1.5 bead skip the tests** (they cannot exceed what a legal gap arc may
  overlap anyway);
- **virtual edges** (trail split points) are extruded as micro-bridges when they pass the
  rules; **open pieces with almost-touching ends** are joined; **residual closed loops** are
  welded into the walk by `single_path_splice_loops()` (staggered double link, non-crossing
  ladder orientation, candidate validation budgeted per distinct region).

**Connections ride the wall (2026-08-26, stool).** Rules (a)+(b) alone still admitted straight
chords through the open interior, and on a curved leg they are exactly what came out: the splice
attach paid a 61 mm out-and-return "spine" down the middle (its two converging links retrace
EACH OTHER — a case the per-link test never sees), and the same-contour micro-bridges ran up to
60 mm from either wall while the travel they were meant to replace survived anyway. Two
tightenings, both in `FillBase.cpp`:
- a splice link longer than **3 stagger** is never a straight chord: it is ROUTED as a pair of
  rails along the cordolo — outbound on the island contour between the nearest projections,
  return on the `offset(island, −stagger)` contour (fused staggered flanks, the rib-link
  pattern) — and validated with the usual no-retrace sampler; no valid route, no weld;
- a virtual-edge bridge between defects on the **same contour** must hug the boundary (every
  sample within 2 beads of it); different contours (hole↔outer) keep the plain rules, since
  the interior between them is real material.

**4-flip + Z-bridge (2026-08-28, the current mechanism).** With one gap per vertex (forced:
a vertex has 1 chord + 2 gaps and even degree needs exactly one) the selection family is
alternation with phase flips; two flips leave >= 2 components on real sections (a local wish -
trail ends at the leg tip - inverts the phase over half the ring: measured comps 5-6). With
FOUR flips one-component selections exist (375 on the stool ring, uniformly across layers):
the exact solver enumerates quadruples (gated m <= 36 and never under wall lining - lightning
is untouched by explicit choice), scoring by the EMISSION cost: the small defect-pair mouth is
the one the router pays (the greedy virtual pairing takes the closest pair), the large one is
absorbed by layer alternation. A post-filter walks the best ~256 quads and picks the first
whose small pair is actually EXTRUDABLE as a Z-bridge: chord inside the island, not riding any
fragment, within the bridge gate (22 line widths). At emission the virtual edge becomes a real
diagonal bead - Davide's sketched zigzag, endpoints on the boundary, transverse to the chords -
fusing the two trails into ONE open path whose far mouth the arrival alternation absorbs.
Results (stool, grid 5%): travels 122/17.0m -> 26/3.27m, air 47m -> 0, doubled beads 212 -> 0,
wall-to-infill 5.05 -> 2.50mm, and sparse SHRINKS 736 -> 688m (the weld/attach machinery is
simply not needed on 267/277 layers). The gorge/weld attach remains as fallback for genuinely
orphaned cells ([SPQ4] counts layers with no bridgeable quad: 10 on the stool, short stable
travels). Debug: [SPVIRT] (virtual verdicts), [SPQPICK] (chosen quad), [SPBRIDGE], [SPQ4].

Measured on the H-section production part (grid ml=1): 543 intra-island travels → 0 real ones
(43 cosmetic hops ≤ 0.3 mm remain), sparse length −0.2 %. On the stool (grid 5 %, two lobes +
narrow curved leg): air-extrusions 47.1 m → 0.01 m, doubled beads 212 → 0, travels 122 → 26,
wall→infill gap 5.05 → 2.50 mm (the scarf is suppressed on the hooked wall: that seam is the
wall→infill junction, not a scar to hide).

### 2.4 Deviation of boundary-grazing scanlines (scanline patterns only)
A chord running nearly tangent to the contour blocks the arc under it (double bead), which can
veto the only arcs that weld a region together. The grazing interior stretch is re-routed to
sit exactly one bead off the boundary (endpoints untouched, graph unchanged), with a
riding-only collision guard against the other fill lines. **Gated off under lightning wall
lining**: there a wall-hugging stretch is the product (the "second wall"), not an accident.

### 2.4b Multiline single path (`fill_multiline` ≥ 2, connect-before-multiply)
With `single_path_mode` the connected pattern is built in Cura order: the row centerlines are
joined into ONE path first, then `multiline_fill()` widens it into a closed ring and
`single_path_splice_loops()` merges the ring with the walls of the pockets it encloses. The
result is already a single closed loop per island, so three things must be done differently
from `ml == 1` — all gated on `params.connect_polygons && params.multiline > 1`:

- **No boundary connector afterwards.** `chain_or_connect_infill()` would emit gap arcs that
  walk the *island contour* — and at `ml ≥ 2` that contour lies a fraction of a bead from the
  ring's own outer flank, so the arc retraces an already-printed line. Measured on the stool:
  126 mm of the 198 mm right flank walked twice, both passes at x = 727.49. At `ml == 1` that
  same arc is the legal fused rail against the wall; at `ml ≥ 2` there is already a bead there.
  Diagnostic signature: a run of ~1 mm segments (a dense contour walk) collinear with an
  existing bead.
- **Centerline inset `0.5·ml·spacing`.** Half the widened ring's own width, so its outer flank
  lands exactly on the surface boundary — where the `ml == 1` anchor rides (≈ 2.5 mm from the
  wall centerline at 3.2 mm beads). The previous value carried an extra `0.15·spacing` of margin;
  removing it moves the flank by only 0.44 mm. It is the crop below, not this margin, that
  produced the full bead of air.
- **No final `intersection_pl` crop.** The ring is contained by construction, and its
  wall-adjacent flank deliberately rides *outside* the contracted surface — cropping cuts it off.

Verification is by cross-section, not by eye: sample the x of every bead at a fixed y and read
the spacings. A healthy `ml == 2` limb reads `S 727.77  S 730.66  W 733.17` — pattern pair at
2.89 mm, outer flank 2.52 mm from the wall. A retrace reads two `S` at the same x.

### 2.5 Seams and chaining
- A closed sparse path is emitted as an `ExtrusionLoop`: the G-code generator enters it at the
  point nearest the toolhead ("free seam") — closed loop = zero fixed ends.
- Wall-only islands orient their last wall's seam to the closest point of the previous
  island's exit (closest-point chain, `GCode.cpp`).
- The single-path router (`GCode.cpp`, `single_path_mode`) orders an island's infill as one
  spatial walk; loop suspension lets a loop be entered, left for a nested feature, and
  resumed.

Debug: `GINGER_SINGLE_PATH_DEBUG=1` → `[SPEXACT] [SPWELD] [SPBRIDGE] [SPDEVIATE] [SPOPEN]
[SPCUT] [SPCLOSE] [SPDEFECT]` on stderr.

### 2.6 The wall takes over the infill (`single_path_infill_as_wall`)
On transparent material the *anchor* — where a Lightning branch meets the wall — is the visible
defect. Doubling the branch is what removes it: a single branch touching the wall is a degree-3
vertex (a T, which no non-retracing walk can cross), while a branch with two flanks is entered from
one and left by the other, degree 2 throughout. So the wall loop itself detours around every branch:

    loop = ∂( P \ (branches ⊕ spacing/2) )      P = region inside the wall centerline

A boolean, not a router. The bead keeps its usual position (half a width from the surface) and the
skin stays closed: the two flanks sit one spacing apart and are one width each, so together they
cover the mouth they opened (verified — mouths measure one width median, never above two, which is
the exact coverage limit). Files: `src/libslic3r/WallFusion.{hpp,cpp}`,
`PrintObject::fuse_lightning_into_walls()`, called inside `prepare_infill()` between
`combine_infill()` and `generate_wall_ribs()` — the one window where the trees exist (built in
`bridge_over_infill`), the fill surfaces are final, and the rib planner has not run. The fused loop
is not a new entity: the existing outer loop is reshaped, so role, flow, seam and the
overhang/bridge segmentation of the untouched stretches survive by construction; the new flanks
take the dominant path's role — they are wall, not overhang (trees stack: 99.7% of the flank
length has material directly below, same as the outer wall), and a flank that really hangs is
caught by `detect_overhang_wall` like any other.

Editing a perimeter from inside `prepare_infill` has two consequences, both learned the hard way
(2026-07-26, stool). The option invalidates **`posPerimeters`**: with `posPrepareInfill` only,
switching it off re-ran the fill against walls that were still fused and printed the tree twice,
once as wall and once as infill (+5.6% material). And every fused island is recorded in
`Layer::wall_fused_islands`, so `FillLightning::Filler` drops the wall lining there: the fused loop
IS the ring, while a lining would trace the outline of each carved gorge, one more bead beside
every flank.

Gated on Lightning + `wall_loops = 1` (the gorge is one spacing wide — a second concentric loop has
nowhere to go, and a scanline pattern would cut the island into one cell per chord); outside that it
falls back to the normal infill rings, which `single_path_infill_ring_always` can force on every
layer. Rules, all in the geometry: extend roots to the wall centerline (or the gorge never opens);
clean up the interior only and put the perimeter collar back (the opening run over the whole region
eats stretches of wall); keep two mouths at least two widths apart.

**All or nothing, per island.** An island whose whole tree the wall absorbs gets NO sparse infill:
the surface is dropped, not carved. Leaving it there made the single-path connector walk the outline
of every gorge — 93% of the leftover fill ran 2.3 mm from a flank and supported nothing. That is
also why the fusion refuses nothing any more: what it will not take, nobody prints. Branches are
never pruned (`prune_length = 0`; `GINGER_FUSION_PRUNE_W` is an experiment knob, and any value above
zero puts the island's fill back), and a root that lands too close to one already taken is demoted,
not dropped — it keeps its material as an inner ring for the rib planner. Asking the generator for a
tidier tree is not an option either: its own prune length is the 45° overhang budget (one layer
height per layer), so a twig it kept is a twig something above stands on. Non-pinching comes for
free: the caller takes every curve the boolean returns instead of assuming there is one.

Which means the boolean has to run on the island's whole **ExPolygon**, not on its outer loop: every
ring is wall, so a branch rooted on a hole opens its gorge there (the stool's base is an annulus
with spokes — with the outer loop alone, 26 layers of 527 still printed sparse), and the rings that
come back REPLACE the island's whole loop set, two of them having possibly merged into one (a branch
bridging a hole to the outer wall) or one having split. A branch that reaches no ring at all — the
lone stub under a top shell, whose root is on the solid/sparse interface in the middle of the island
— punches a hole instead: the island comes back with one more ring, for the rib planner to weld.
That is the last 10 layers, and the reason the stool now prints **zero** sparse on all 527.

**Nothing may float.** Whatever the tubes leave unconnected to a wall comes back as a closed ring in
the middle of the island, and a 13 mm ring (a 3×5 mm dot) is below what the rib planner can weld, so
it prints on its own between two travels — 45 of them on the stool's layer 7. Two ways in: a tip
that stops just outside `root_reach` (the tree ends where the fill boundary is, not where the wall
is: median 8 mm short) and a root R9 has just demoted. Both are linked — to the neighbouring branch
when one is nearer than the wall (no second mouth, R9 still honoured), to the wall otherwise. The
components are found by mutual tube proximity, tested in **both** directions: a child branch ends
*on* its parent, and the parent has no vertex there, so a one-way test leaves whole sub-trees adrift.

**Point budget.** The gorges are wall, and wall is walked by the rib planner, the seam, the cooling
and the G-code writer, so their tessellation is not free. Clipper's default arc tolerance is
*relative* — 1/500 of the radius, i.e. 0.0035 mm on a pellet bead — and the boolean adds its own
points along the straight stretches, so the rings used to carry fourteen times the detail the user
asked for: +96 k moves and +5.2 MB of G-code on the stool. The tube offsets now take an explicit arc
tolerance and the rings come back Douglas-Peucker'd (never `simplify_polygons`: that re-runs Clipper
and would change the topology the caller is about to match). The tolerance is the print `resolution`
**clamped to spacing/500**, and the clamp is the whole point: simplifying all the way to 0.05 mm
buys 21% fewer moves and costs 14 min of print, because the gorge tip stops being a turn and becomes
a corner the motion planner brakes for. At the clamp: −10% moves, −2.7 MB, estimate unchanged.
`GINGER_FUSION_RES_W=<n>` scales it.

The layer loop is `tbb::parallel_for` — one layer never reads another (undo record, fused-island
census and fill surfaces all belong to the layer, the Lightning generator is only read). Together
with the point budget: fusion 1290 ms → 224 ms, whole slice 9.9 s → 7.8 s against 6.3 s with the
fusion off. Note that slicing is **not** reproducible run to run, fusion or no fusion — not caused
by the parallel loop, and worth knowing before diffing two G-codes. One cause is fixed (the `rand()`
that decided the Lightning polyline decomposition, now a hash of the node position); a second one is
still unlocated, in the surface classification inside `prepare_infill`. See [[Wall fusion]] in the
glossary for what has been ruled out.

Price: **2 mm of bead per mm of branch** — and, measured end to end on the stool, that is a wash:
4118 g against 4120 g with the fusion off, +25 min (wall speed instead of infill speed).
Debug: `GINGER_FUSION_DEBUG=1` → `[FUSION]`, `complete=` counts the islands left with no fill;
`GINGER_FUSION_PROFILE=1` → `[FUSION-PROF]`, per-stage CPU against wall clock.

---

## 3. Wall connectivity — ribs (`single_path_wall_ribs`)

Multiple wall loops of one island (outer + holes) are merged into ONE closed walk by inserting
*ribs*: two link segments staggered by one bead (fused flanks, never a doubled centerline),
each spliced loop cut open for one stagger — the automated version of the "micro cut" a user
would model in CAD. Files: `src/libslic3r/WallRibs.hpp/.cpp` (planner spec lives in the
header comments), `PrintObject.cpp::generate_wall_ribs()`, consumed in
`GCode.cpp::extrude_perimeters()`.

Principles (all downstream of D1/D4):

- **Every closed loop of the island is a candidate.** Single path is a *chain of features*,
  not a uniform path: loops are never partitioned by role/width/flow (the old exact-equality
  grouping put every Arachne variable-width loop in its own group — no ribs at all on the
  fantome). The rib's own scalars (stagger, corridor, buttress width) and the link flow at
  emission come from the DOMINANT (longest) source path — never from whatever short special
  stretch a loop happens to start with.
- **Per-layer Prim.** The walk is planned per layer: `loops − 1` ribs (minimum spanning
  tree). The rib count changes only when the geometry's topology changes.
- **Obstacle field, live** (`rib_segment_conflicts`, exported in `WallRibs.hpp`). A link
  axis must not CROSS any foreign bead anywhere (own curves are exempt only within one
  stagger of their attach), and must not RIDE any bead: centerlines parallel closer than
  0.9 width for more than one stagger is the D2 interasse violation — invisible to segment
  intersection, caught by sampling. The field is rebuilt at every splice: the growing walk
  (cuts and inserted links included), the not-yet-spliced loops, and the island's OPEN
  printed beads (Arachne thin-wall multipaths, `extra_obstacles`). The buttress descent
  applies the same test (ride walk = own, everything else foreign).
- **Columns.** Each rib re-anchors on the previous layer's attach pair while the geometry
  still allows it (bead-overlap reprojection test) → ribs stack into self-standing columns.
  Drift budget per layer = half a bead capped at ONE layer height (~45° lean). Column
  memory is per island and superseded zone by zone: an island whose plan was accepted claims
  its zone; corpses elsewhere carry over (across rib-less layers and founded-failure drops)
  so the near-dead re-founding always sees them.
- **Foundation buttress.** A rib that must start with nothing below grows a lightning-style
  stub chain downward through the *walls*: each layer's stub is 0.5 bead shorter (the
  self-support step), the chain ends on real material (solid shells, the bed) or melts back
  into the wall. Works over true void — sparse is never assumed to support anything.
- **Corridors.** Rib footprints are carved out of the fill surfaces so nothing else extrudes
  across a rib bead.
- **Mixed-role loops merge like any other.** The emission re-attributes every merged segment
  to its source path (role, flow, width, height — the complete `ExtrusionPath` attribute
  set), so a bridge stretch inside the walk still prints as a bridge. Whether a rib may stand
  somewhere is decided by the support/foundation tests alone — never by the role under the
  attach (a loop-level role veto used to kill healthy columns 200 mm away from the offending
  bridge segment).
- **Placement cascade** (in order): yesterday's column → near the dead column → supported
  positions (on yesterday's rib/wall/solid) → foundable positions (buttress dry-run) → drop
  with a counted reason. **Free re-founding exception**: the near-dead preference exists to
  buy self-support (standing on yesterday's rib corridor); when the column died and a
  candidate stands on REAL material (solid/walls), the shortest such candidate wins outright —
  a whole feature dying at once (engraved text) must not capture the re-founded rib into a
  long chord across the part.

Debug: `GINGER_RIBS_DEBUG=1` → `[RIBSTAT]` per layer (loops / candidates / spliced /
anchor_reused / founded / drop reasons) — the tool for "why is this hole not connected".

---

## 4. Overhang management

- **Layer height is the overhang knob.** Bead width is fixed by the nozzle; the printable
  overhang angle scales with `atan(width_step / layer_height)`. Lowering layer height (also
  locally, via adaptive layer height) is the primary way to attenuate overhangs on pellet.
- **Bridges live inside walks.** Bridge/overhang stretches keep their role, speed, fan and
  flow through any rib merge (see §3) — the classifier's segmentation survives single-path.
- **Buttresses are engineered overhangs**: the 0.5 bead/layer regression *is* the printable
  overhang ratio applied to a growing stub.
- **Design-time check**: the "Print check" gizmo (dado/DfM_UI branch) analyses the mesh for
  thin walls (1×/2× bead) and paints overhang gradients before slicing.

---

## 5. Flow continuity — pellet ERS

See `EXTRUSION_DYNAMICS.md` (model, saturation, identification protocol) and the ERS section
of `GLOSSARY.md`. DFM consequences used throughout this codebase:

- `pellet_ers_mode` extends rate smoothing **across gaps** (travels/retracts), tagged
  `;_ERS_RAMPUP/RAMPDOWN/STEADY` in `PressureEqualizer.cpp`.
- Re-emitted feedrates are quantized at **0.1 mm/s** (upstream Orca uses 1 mm/s — mm³/s-scale
  flow error at pellet cross-sections).
- Fewer path pieces (D1) means fewer ramps: single-path is also a flow-quality feature. Every
  travel eliminated removes one decompression/recompression cycle and one ERS transition.
- Calibration: the per-layer Parameter Sweep (`Calib_Param_Sweep`, also headless via
  `--sweep "key:start:end:step"`) sweeps a calibration key along Z. Supported keys are the
  `calib.hpp` sets: ERS (`max_volumetric_extrusion_rate_slope`, `pellet_ers_*`), retraction
  (`retraction_length/speed`, `deretraction_speed`, `retract_restart_extra`) and wipe
  (`wipe_distance`, `wipe_speed`); anything else is rejected at parse.

---

## 6. Cooling constraints

Volume-based cooling (h² × k model, see `GLOSSARY.md`) sets layer times from section volume:
small cross-sections need long layer times. DFM guidance: section area, not height, is the
schedule driver — massive short parts cool layer-bound, thin tall parts print speed-bound.
`cooling_time_per_cross_section` is the profile knob.

---

## 7. Debug tooling index

| Env / tool | Output | Use for |
|---|---|---|
| `GINGER_SINGLE_PATH_DEBUG=1` | `[SPEXACT] [SPWELD] [SPBRIDGE] [SPDEVIATE] [SPOPEN] [SPCUT] [SPDEFECT] [SPVIRT] [SPQPICK] [SPQ4] [SPGORGE] [SPHOOK]` | sparse single-path decisions per island. `[SPOPEN]` also reports `dentro_isola=%`: 0 % on a mouth that lies on the contour means the collinear-degenerate case, not a bridge over air |
| `GINGER_SP_CLOSE=1` | `[SPCLOSEM]` | opt-IN, default OFF: mouth closure by boundary rails (see GLOSSARY) |
| `GINGER_SP_DUMP=1` | `[SPDUMP]` | per-position dump of the exact solver's chosen selection |
| `GINGER_SP_INJECT=1` | — | opt-IN: inject a crossing chord when the weave finds no crossing |
| `GINGER_SP_PROFILE=1` | per-phase timings + Clipper call counts | where the connector spends its time |
| `GINGER_RIBS_DEBUG=1` | `[RIBSTAT]` per layer + emission drops | wall rib planning census |
| `GINGER_FUSION_DEBUG=1` | `[FUSION]` per island and per object | wall/lightning fusion census (roots, gorges, pruned branches, dropped roots, extra loops) |
| `GINGER_FUSION_PROFILE=1` | `[FUSION-PROF]` per object | fusion cost per stage (CPU, summed over threads) against the wall clock |
| `GINGER_FUSION_PRUNE_W=<n>` / `_NOCARVE=1` / `_NOREPLACE=1` | — | fusion bisection: R3 threshold in wall spacings (default 0), skip the gorge carve, skip the loop replacement |
| `GINGER_FUSION_RES_W=<n>` | — | scales the fused rings' simplification tolerance (default 1 = `resolution` clamped to spacing/500; 0 = no simplification) |
| `GINGER_DETERMINISM_PROBE=1` | `[DET]` per step, per object | why two identical slices differ: hashes slices/perimeters/fill surfaces/rib plan/fills after every pipeline step, ordered **and** commutative, so a permutation is told apart from a geometric change. Slice twice, diff the streams, first differing line names the step |
| `GINGER_SPCUT_Z=<z>` | per-hole detail near one z | racetrack cut inspection |
| Headless slice | `Ginger-Slicer.exe --slice <plate> --outputdir <dir> project.3mf` | verify slicing changes without GUI (3MF must embed settings) |
| `--sweep "opt:from:to:step"` | per-layer swept G-code | parameter calibration prints |

Filament diameter on Ginger pellet profiles is 1.12838 mm → 1 mm² cross-section: ΔE in mm
equals mm³ extruded (convenient for G-code analysis).
