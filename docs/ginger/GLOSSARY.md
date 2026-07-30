# Ginger Pellet — Domain Glossary

Concise reference for terminology used across pellet-specific code paths in
GingerSlicer. Each entry points to the primary source file when applicable.

> Scope: pellet extrusion, Ginger profiles, custom slicer behavior. Generic
> FDM terms (perimeter, infill, bridge…) are not redefined here.

---

## Extrusion & material flow

- **Pellet extruder** — Screw-based extruder fed by plastic pellets/regrind
  instead of filament. Slower volumetric response, larger nozzles
  (1.0–8.0 mm), no native retraction in the FDM sense.
  Toggle: `pellet_modded_printer` in `PrintConfig.hpp`.

- **Volumetric extrusion rate** — Flow expressed as mm³/s (filament: also
  derivable from `feedrate × cross_section`). On pellet machines this is the
  fundamental quantity, not feedrate, because the screw responds to volume
  demand. See `m_max_volumetric_extrusion_rate` in `PressureEqualizer.cpp`.

- **Extruder rotation volume** (`extruder_rotation_volume`) — mm³ extruded
  per full revolution of the screw. Pellet equivalent of "E-steps per mm" for
  filament printers. Per-extruder array.

- **Mixing stepper rotation volume** (`mixing_stepper_rotation_volume`) —
  Analogous quantity for an auxiliary mixing stepper, when present in
  active-feeding setups.

- **Active pellet feeding** (`use_active_pellet_feeding`) — Setups where an
  upstream feeder motor pre-charges the screw. Affects start-of-print purge
  and recovery after pauses. See `active_feeder_motor_name`.

- **Virtual retract** — Pellet printers cannot retract filament mechanically.
  "Retraction" is emulated via screw decompression (brief reverse rotation)
  and/or by reducing target volumetric flow before travels. See pellet-ramp
  logic around `m_pellet_ers_min_rate` in `PressureEqualizer.cpp`.

---

## Extrusion Rate Smoothing (ERS)

- **ERS — Extrusion Rate Smoothing** — Post-processing pass in
  `PressureEqualizer` that clamps `dV/dt` so volumetric flow changes do not
  exceed `max_volumetric_extrusion_rate_slope`. Compensates for slow pressure
  response of long melt zones (especially pellet screws).
  File: `src/libslic3r/GCode/PressureEqualizer.cpp`.

- **Slope** (`max_volumetric_extrusion_rate_slope`) — Maximum allowed
  change in mm³/(s·s) during transitions between adjacent extrusion paths.
  Lower slope = smoother ramps, slower transitions.

- **Slope segment length**
  (`max_volumetric_extrusion_rate_slope_segment_length`) — Maximum geometric
  length over which a ramp is allowed to be distributed before the slicer
  forces shorter sub-segments.

- **Pellet ERS mode** (`pellet_ers_mode`) — Extends ERS across **gaps**
  (travels, retracts, discontinuities) rather than only within continuous
  extrusion. Required on pellet machines because pressure does not collapse
  during travel as it does on Bowden/Direct filament setups.
  Field: `m_pellet_ers_mode` in `PressureEqualizer.hpp`.

- **Travel threshold**
  (`pellet_ers_travel_threshold_mm`) — Travel moves shorter than this are
  treated as continuous extrusion (no ramp-down/ramp-up). Default 3 mm.
  Avoids redundant micro-ramps over hops/seams.

- **Ramp profile** (`pellet_ers_ramp_profile`, enum `PelletERSRampProfile`)
  — Interpolation curve used to bridge the volumetric flow between current
  and target rate. Values: `Linear`, `Sqrt`, `Exponential`. **Sqrt is the
  only slope-exact profile** (constant dQ/dt equal to the configured slope —
  the kinematic law). Linear/Exponential concentrate the slope locally, so
  `apply_effective_slopes()` divides their effective slope by 2 / 3 to keep
  the instantaneous peak bounded (Linear is exact after the correction;
  Exponential still overshoots ~2.5x in space-parametrization — prefer Sqrt).
  In the parameter sweep the profile is addressed numerically:
  0=linear, 1=sqrt, 2=exponential.

- **Deceleration slope** (`pellet_ers_deceleration_slope`) — Independent
  slope for **all negative flow transitions** in pellet mode (boundary
  ramp-downs AND internal decelerations — overhangs, slower features, width
  changes). `0` means use the same value as the main slope. The
  pressurize/release asymmetry is a property of the screw, not of travels.
  Applied via `apply_effective_slopes()` into the per-role slope table.

- **Pressure time constant** (`pellet_ers_pressure_tau`, seconds) — τ of
  the first-order melt-reservoir model (τ = R·C: nozzle resistance × melt
  compressibility). Primary ramp flow compensation: the extruded amount in
  every `;_ERS_RAMPUP`/`;_ERS_RAMPDOWN` segment is scaled by
  `1 ± τ·slope/Q(x)` — the slicer-side equivalent of the ORNL BAAM
  feedforward lead filter and of firmware pressure advance, but for screw
  dynamics. Adapts automatically to the configured slope and to the
  position along the ramp (stronger near min rate). Calibrate ONCE per
  nozzle (sweep); stays valid for any slope. BAAM reference: τ ≈ 0.06–0.09 s
  on a large-nozzle machine; expect more on small nozzles (R ∝ ~1/r⁴).
  0 = disabled. Theory: `docs/ginger/EXTRUSION_DYNAMICS.md`.

- **Ramp flow trim** (`pellet_ers_rampup_flow` /
  `pellet_ers_rampdown_flow`, %) — Empirical multipliers applied on top of
  the τ compensation inside the ramp zones (with τ = 0 they act alone as a
  constant-percentage compensation). They absorb what the linear model does
  not capture: shear-thinning τ(Q), screw feed non-linearity. The feedrate
  ramp alone only redistributes the pressure mismatch — under-extrusion at
  path start / over-extrusion at path end, the classic bad-seam signature
  even with stringing solved. Both sweepable. Requires relative E.
  100% = no trim.

- **Min rate** (`pellet_ers_min_rate`) — Floor of the ramp in mm³/min. The
  ramp never goes below this even at the boundary of a travel; prevents the
  screw from stalling. Default 30 mm³/min (≈ 0.5 mm³/s).

- **Pellet ramp segment** — Sub-segment generated by the pellet-ERS
  mini-pass to enforce ramps across gaps. Tagged via `GCodeLine::pellet_ramp`
  in `PressureEqualizer.hpp` to distinguish from native ERS adjustments.

- **Mini-pass** — Second-stage adjustment in pellet mode: re-scans the line
  buffer to insert ramp sub-lines around detected travel boundaries that the
  primary ERS pass cannot reach. See `adjust_volumetric_rate(..., is_segment_start, is_segment_end)`.

- **`travel_before_polyline` / `travel_after_polyline`** — Per-line metadata
  in `GCodeLine` storing the travel distance immediately preceding/following
  the line. Drives the threshold check for whether a ramp must be inserted.

- **ERS G-code tags** — Debug comments emitted by the PressureEqualizer on
  every line it touches: `;_ERS_RAMPUP`, `;_ERS_RAMPDOWN`, `;_ERS_STEADY`
  (zone of the ramp), `;_ERS_CALIB layer=N param=value` (active global sweep
  value, one per layer), plus the `;_ERS_SWEEP` tags emitted by GCode for
  per-object sweeps (see Calibration below). Grep these to analyze ramps
  without the GUI.

- **Feedrate quantization** — `push_line_to_output` rounds every re-emitted
  feedrate to **0.1 mm/s** (was 1 mm/s upstream). With pellet bead
  cross-sections (~3.8 mm² at 3.2×1.3) 1 mm/s of speed is ~1.9 mm³/s of
  flow — enough to overshoot `filament_max_volumetric_speed` and to
  staircase the ERS ramps. Historical bug: walls printed at alternating
  150/151.89 mm³/s because 39.5 mm/s was rounded up to 40.

- **`POLYLINE_START` / `POLYLINE_END` markers** — Comments emitted by
  `GCode::_extrude()` when `pellet_ers_mode` is enabled. They delimit each
  continuous extrusion polyline and carry `travel_mm=` so the
  PressureEqualizer can detect segments and decide ramp-up/ramp-down without
  re-deriving travel distances.

---

## Calibration (parameter sweep)

- **Parameter sweep** (`CalibMode::Calib_Param_Sweep`) — The only calibration
  exposed in the Ginger UI (menu **Calibration → Parameter tuning (per-layer
  sweep)**). Varies ONE parameter layer by layer (no test model is loaded):
  `value(layer) = start + step × layer`, clamped at `end`, direction inferred
  from start/end. `step <= 0` (the default, empty field in the dialog) means
  **automatic**: the step is derived from the target's layer count
  (`calib_sweep_effective_step`) so the value reaches `end` exactly at the
  last layer — i.e. the value is linear in Z over the whole object, and
  objects of different heights each span the full range. The layer count is
  resolved at G-code time (`PrintObject::layer_count()` per object,
  `m_layer_count` for the global sweep, passed pre-resolved to the
  PressureEqualizer ctor). The dialog's **Apply to** combo targets either
  **All objects** (one global sweep for the whole plate) or a single object,
  so several per-object sweeps (one per object, different parameters/ranges)
  can run in the same print. Dialog: `Param_Sweep_Dlg` in
  `src/slic3r/GUI/calib_dlg.cpp` (Apply = set & stay open, OK = set & close;
  reads the print via `get_partplate_list().get_current_fff_print()`, NOT
  `Plater::fff_print()` which is the legacy unused print); plumbing:
  `Calib_Params` (`sweep_param`, `object_id`; -1 = global) in
  `src/libslic3r/calib.hpp`; storage: `Print::m_calib_params` is a list —
  one global entry OR per-object entries, never mixed
  (`Print::set_calib_params` enforces it, `Calib_None` removes the target's
  entry).

- **Sweepable parameters** — Two application paths (`calib_is_writer_param` /
  `calib_is_ers_param` in `calib.hpp`):
  - retraction group (`retraction_length`, `retraction_speed`,
    `deretraction_speed`, `retract_restart_extra`): applied to the G-code
    writer config — global sweep at each layer change, per-object sweeps at
    each object change (`GCode::apply_per_object_sweep`, which also restores
    profile values on non-swept objects); comment
    `; Calib_Param_Sweep: layer: N[, object: name], key: value`.
  - ERS group (`max_volumetric_extrusion_rate_slope`,
    `pellet_ers_deceleration_slope`, `pellet_ers_min_rate`,
    `pellet_ers_ramp_profile`, `pellet_ers_rampup_flow`,
    `pellet_ers_rampdown_flow`, `pellet_ers_pressure_tau`): applied inside
    the PressureEqualizer (global sweep via ctor `const Calib_Params*`,
    comment `;_ERS_CALIB`; per-object sweeps via `;_ERS_SWEEP` tags, see
    below). Requires `max_volumetric_extrusion_rate_slope > 0` (otherwise
    the PressureEqualizer is never instantiated). The pellet-only parameters
    (decel slope, min rate, ramp profile) additionally require
    `pellet_ers_mode = 1` to have any effect.

- **`;_ERS_SWEEP` tags** — Per-object ERS sweep transport: GCode emits
  `;_ERS_SWEEP obj=<ModelObject id> <key>=<value>` before each swept
  object's extrusions and `;_ERS_SWEEP default` at layer changes / before
  non-swept objects. The PressureEqualizer turns each tag into a
  `SweepEvent` (line index + full `SweepSnapshot` built from the profile
  baseline, `rebuild_sweep_events`) and replays events per extrusion segment
  while processing and per line while outputting (ramp profile, flow factors
  and τ are consumed at output time). The tags survive into the final
  G-code: grep `;_ERS_SWEEP` for the object→value map.

- **CLI sweep** — `--sweep "parameter:start:end"` (automatic step) or
  `--sweep "parameter:start:end:step"` (explicit, step > 0), global or
  per-object with `@Object name` (entries separated by `;`, matched by
  ModelObject name) together with `--slice N`:
  `Ginger-Slicer.exe --slice 2 --sweep "pellet_ers_deceleration_slope:5:40@Cube" --outputdir out project.3mf`.
  Parsed in `GingerSlicer.cpp` (slice branch) → `Print::set_calib_params`.
  With duplicate object names the first match wins. Fully headless: grep
  `;_ERS_CALIB` (global) or `;_ERS_SWEEP` (per-object) in the output.

- **Sweep lifecycle** — `Print::set_calib_params` invalidates `psGCodeExport`
  so the next slice regenerates G-code. Loading new files resets the calib
  mode to `Calib_None` (SoftFever legacy in `Plater::priv::load_files`):
  load objects FIRST, then set the sweep. Sweeps are NOT saved in the 3MF.
  Per-object sweeps reference `ModelObject::id()` (session-scoped ObjectID),
  so they don't survive a project reload either; the dialog's "Active
  sweeps" box shows what is currently set.

- **One-layer output buffering** — The PressureEqualizer emits layer N−1
  while processing layer N. The sweep keeps a `SweepSnapshot` of the
  parameters each layer was processed with, so the output pass and the
  `;_ERS_CALIB` comment always match the ramps actually generated.

---

## Profiles & distribution

- **Vendor profile** — Bundle of system presets shipped with the slicer.
  Ginger's lives in `resources/profiles/Ginger Additive/` plus its index
  `Ginger Additive.json`.

- **System preset vs user preset** — System presets are read-only and bundled;
  user presets live in `<datadir>/user/<id>/{print,filament,machine}/` and
  inherit from system ones via the `inherits` key. See
  `PresetCollection::get_preset_parent` in `Preset.cpp`.

- **Preset bundle version** — The `version` field present in every
  versioned profile JSON. CI (`scripts/check_profile_version_bump.py`)
  requires it to be strictly greater than the version on `origin/main` for
  **every** versioned file under `resources/profiles/Ginger Additive/`. A
  single profile change forces a bump of **all 41 versioned files**
  (the 2 machine `_common.json` files are unversioned by design).

- **OTA bundle** — ZIP packaged by `scripts/pack_profiles.sh` and uploaded
  to the `nightly-builds` GitHub release. Consumed by `PresetUpdater` in
  `src/slic3r/Utils/`.

- **Filament ID** (`filament_id`) — String key used to cross-reference
  filaments across vendor library and user presets. Collisions are silent —
  treat as a globally unique tag.

---

## Slicing pipeline (pellet-relevant subset)

- **PerimeterGenerator** — Generates wall extrusion paths (Classic or
  Arachne). Arachne is preferred on pellet (variable-width walls absorb
  wide nozzles better). File: `src/libslic3r/PerimeterGenerator.cpp`.

- **CoolingBuffer** — Computes per-layer time estimates and applies
  speed/fan slowdowns to meet `min_layer_time`. Owns the **volume-based
  cooling** branch (see dedicated section below). File:
  `src/libslic3r/GCode/CoolingBuffer.cpp`.

- **PressureEqualizer** — Final post-process pass over the G-code line
  buffer. Owns ERS + pellet-ERS logic. File:
  `src/libslic3r/GCode/PressureEqualizer.cpp`.

- **`PrintObjectSlice.cpp`** — Where 3D mesh becomes per-layer 2D
  `ExPolygon`s. Entry point for any future feature-size pre-check based on
  Minkowski erosion.

- **Host options vs invalidation** — The Ginger sidebar "Connection"
  selector writes `print_host` into the **printer preset** (stock Orca keeps
  it in the separate physical-printer config). `Print::apply` has TWO
  invalidation paths: `print_diff` (routed through
  `invalidate_state_by_config_options`, per-option granularity) and
  `full_config_diff` (`PrintApply.cpp`, ANY changed key invalidates
  `psGCodeExport`). The 9 `print_host`/`printhost_*` keys are excluded from
  both, otherwise changing the printer IP would silently discard the sliced
  G-code and its print statistics — breaking the output filename template
  (`{filament_type[initial_tool]}` → "Non-integer index" error) right before
  an upload.

---

## Single path & wall ribs (Ginger)

Full rationale and implementation map in `docs/ginger/DFM.md`.

- **Single path mode** (`single_path_mode`) — Print-wide toggle: walls and
  sparse infill of each island chain into one continuous walk (travels are
  the enemy on pellet: no true retract, melt degrades while idle). Drives
  `FillParams::connect_polygons` for connectable sparse patterns and the
  wall/seam routing in `GCode.cpp`.

- **Euler connector** — `connect_infill_single_path()` in
  `src/libslic3r/Fill/FillBase.cpp`: chords (scanlines) + boundary gap arcs
  form a ring graph; an alternating gap *phase* makes every vertex even;
  odd vertices (**defects**) are the open ends of the walk.

- **Exact min-pieces solve** — For single-contour islands (m ≤ 160): a
  selection with 0/2 defects is fully determined by the defect pair + one
  phase bit, so all `2 + 2·C(m,2)` candidates are enumerated exactly.
  Cost: pieces → blocked → defects → mouth; under lightning wall lining
  (final emission) ties break toward max wall coverage — the lining. Runs
  under lining too (the greedy's landing spot was rotation-sensitive and
  flipped identical layers between wall-hug and floating blobs).
  Debug line `[SPEXACT]` (incl. coverage).

- **Physical link rule** — Any artificial connection (bridge over a
  virtual edge, join of near-touching open ends, weld of a residual loop)
  is an extruded bead allowed at ANY length provided it stays inside the
  island and does not retrace an extruded line (parallel < 25° within
  0.8 bead for > 1.5 bead accumulated). Links ≤ 1.5 bead skip the tests.
  Canonical retrace rule ("stesso interasse"): same-layer centerlines
  parallel closer than one width = doubled material; flank contact at
  exactly one width = fusion, legal and often the goal.

- **Wall lining** (`FillParams::sparse_wall_lining`) — Under lightning +
  single path, the wall-hugging boundary stretch the connector keeps when
  it costs no extra trail: the "second wall" bead fused to the perimeter,
  the rail that carries the walk to the wall seam. It is the connector's
  optimum choice, not an unconditional extra loop — layers whose tree is
  empty print no sparse at all (lightning is demand-driven).

- **Weld / splice** — `single_path_splice_loops()`: staggered double-link
  merge of loops into the walk (ladder, never crossing); with the `island`
  parameter it enforces the physical link rule.

- **Deviation** — Boundary-grazing interior stretch of a scanline is
  re-routed one bead off the contour so the arc under it stays legal
  (`[SPDEVIATE]`). Scanline patterns only — under lightning wall lining a
  wall-hugging row is the product, not an accident.

- **Wall rib** (`single_path_wall_ribs`) — Two staggered link segments
  welding two wall loops into one walk (the automated CAD "micro cut").
  Planned per layer by Prim (`loops − 1` ribs) in
  `PrintObject::generate_wall_ribs()` / `src/libslic3r/WallRibs.hpp`.
  EVERY closed loop of the island is a candidate — no role/width/flow
  partition (exact-equality grouping left Arachne's variable-width loops
  each in its own group: zero ribs). Rib scalars and emission link flow
  come from the DOMINANT (longest) source path.

- **Rib obstacle field** (`rib_segment_conflicts`, `WallRibs.hpp`) — A
  link/stub axis may not cross a foreign bead anywhere (own curves exempt
  within one stagger of their attach) nor ride one (parallel < 0.9 width
  for > one stagger = the interasse violation, sampled — intersection
  tests are blind to it). Rebuilt live at every splice: growing walk +
  unspliced loops + open thin walls (`extra_obstacles`).

- **Rib column** — A rib re-anchored on the previous layer's attach pair;
  columns are the self-standing vertical structure of ribs. Per-layer
  drift budget: half a bead capped at one layer height (~45° lean).
  Column memory is per island, superseded zone by zone (accepted plans
  claim their zone; corpses elsewhere survive for near-dead re-founding).

- **Free re-founding** — When a column dies, candidates near the dead column
  are preferred ONLY because they may stand on yesterday's rib corridor
  (self-support). A candidate standing on REAL material (solid/wall beads)
  is position-free: the shortest wins outright, so a whole feature dying at
  once (engraved text) cannot capture the new rib into a long chord.

- **Foundation buttress** — Lightning-style stub chain grown downward
  through the walls under a rib that starts over void; each stub 0.5 bead
  shorter per layer, ends on real material or the bed. Sparse infill is
  never treated as support.

- **Rib corridor** — The rib footprint carved out of the layer's fill
  surfaces so nothing else extrudes across the rib beads.

- **`[RIBSTAT]`** (`GINGER_RIBS_DEBUG=1`) — Per-layer census of the rib
  planner (loops, spliced, anchor_reused, founded, drop reasons).

---

## Multiline infill (Clipper2)

Aligned to OrcaSlicer PR **#11435** (Clipper2 multiline), **#11765**
(short-connection skip) and **#10967** (density adjustment + crash fix).

- **Multiline infill** (`fill_multiline`, max `10`) — Prints each infill
  line as N parallel passes instead of one. On pellet (1.0–8.0 mm nozzles)
  a single bead is often too wide → over-extrusion/blobbing; splitting into
  N lines distributes flow and improves adhesion. `1` = disabled.
  Definition: `src/libslic3r/PrintConfig.cpp`.

- **`multiline_fill()`** — Core routine in `src/libslic3r/Fill/FillBase.cpp`
  (decl in `FillBase.hpp`). Offsets each base polyline by
  `Clipper2Lib::ClipperOffset(JoinType::Round, EndType::Round)` to produce
  the N parallel lines. **Output is closed rings** — every caller MUST run
  `intersection_pl(...)` against the surface before
  `chain_or_connect_infill`, otherwise the unclipped rings crash the
  boundary-graph builder (`create_boundary_infill_graph`).

- **Clipper2** — Vendored polygon-clipping lib v1.5.2 in
  `deps_src/clipper2/` (mirror of OrcaSlicer, incl. `clipper2_z`). Linked as
  target `Clipper2`. Slic3r↔Clipper2 conversion helpers live in
  `src/libslic3r/Clipper2Utils.{hpp,cpp}`. Distinct from Clipper1
  (`ClipperUtils.hpp`), still used elsewhere.

- **`fill_surface_trapezoidal()`** — Non-crossing pattern in
  `src/libslic3r/Fill/FillRectilinear.cpp` used for **Grid** (`Pattern_type
  0`, 45°) and **Triangles** (`Pattern_type 1`, 90°) when `multiline > 1`.
  Replaces the crossing grid/triangle lines (which would double-extrude at
  every intersection) with offset trapezoids. Selected by the branch in
  `FillGrid::fill_surface` / `FillTriangles::fill_surface`.

- **Density adjustment (`n_multiline`)** — For Adaptive Cubic / Support
  Cubic (`FillAdaptive.cpp`, `adaptive_fill_line_spacing`) and Lightning
  (`Fill/Lightning/Generator.cpp`, `m_supporting_radius`), the base line
  spacing / supporting radius is multiplied by `fill_multiline`. Without
  this, adding N parallel lines without widening the base grid makes the
  infill N× too dense → over-extrusion. Applied **only** to the infill
  `Generator(const PrintObject&...)` ctor, not the BBS support ctor (which
  divides by an explicit `density`).

- **Short-connection skip (#11765)** — In `connect_infill()`
  (`FillBase.cpp`), when `multiline > 1` the connector skips links shorter
  than the line spacing, preventing the N adjacent lines from being stitched
  into a zig-zag blob.

- **`[multiline_fill]` debug logs** — `BOOST_LOG_TRIVIAL(debug/trace)`
  tags inside `multiline_fill()` reporting pattern / multiline count /
  spacing / density / input+output polyline counts. Enable via
  **Preferences → Debug → Log Level** (`debug` or `trace`; non-public
  builds only, `#if !BBL_RELEASE_TO_PUBLIC`). Output:
  `%APPDATA%\GingerSlicer\log\debug_*.log`.

---

## Geometry & quality concepts (Ginger usage)

- **Feature size vs nozzle** — On pellet (≥1 mm nozzles) geometric features
  smaller than ~`nozzle/2` will be silently dropped by the slicer. Future
  geometry-analyzer checks should warn before slicing.

- **Layer-time warping** — On large pellet parts, layers with very long
  per-perimeter time allow material to crystallize/cool between passes,
  producing visible bands. Counter-measure: enforce `min_layer_time` and
  consider Z-hop strategy.

- **Decompression / oozing** — Without retraction, pellet machines ooze
  during travels. Mitigated by: (a) brief screw reverse, (b) higher travel
  speeds, (c) wiping with `coast_at_end_speed`.

---

## Volume-based cooling (h² × k model)

- **Volume-based cooling** (`volume_based_cooling`) — Optional cooling
  model that replaces the legacy fixed `slow_down_layer_time` threshold
  with a physics-derived per-layer minimum time. When enabled, the
  CoolingBuffer computes `min_time = h² × k` for every layer using the
  thickest bead observed (`max_layer_height`) and the per-material
  coefficient `cooling_time_per_cross_section`. Width-independent: the
  dominant heat-conduction path is vertical, so `line_width`/nozzle
  changes do not shift the cooling time. Adaptive/variable layer heights
  are supported automatically because each `;HEIGHT:` tag in the G-code
  is parsed individually.
  File: `src/libslic3r/GCode/CoolingBuffer.cpp` (`calculate_layer_slowdown`).

- **Cooling time per cross-section** (`cooling_time_per_cross_section`)
  — Material coefficient `k`, in s/mm². Despite the legacy name, the new
  formula multiplies it by `h²` (mm²), not by bead cross-section.
  Derivation:
  `k = -ln((T_target - T_amb) / (T_extrusion - T_amb)) × 0.405 / α_eff`
  where `0.405 ≈ 4/π²` is the leading Fourier coefficient for a 1D slab
  cooling and `α_eff` is the **effective** thermal diffusivity —
  empirically ~3× lower than the textbook α because of natural
  convection limits and ongoing contact with the hot underlying layer.

- **Empirical calibration points** — The default `k` values were
  recalibrated against direct measurements on a Ginger printer:
  - PLA: `h = 1.5 mm` reaches 50 °C in ~60 s → `k_PLA = 60/1.5² ≈ 26.7`
  - PETG: `h = 1.5 mm` reaches 80 °C in ~30 s → `k_PETG = 30/1.5² ≈ 13.3`
  Other amorphous polymers (ABS, ASA, HIPS) are scaled from PETG using
  `f = α_eff/α_nom ≈ 0.366`. Semi-crystalline PP uses crystallization
  temperature (~110 °C) as `T_target`, not Tg. See
  `src/libslic3r/PrintConfig.cpp` tooltip for the full derivation.

- **`max_layer_height`** — Member of `PerExtruderAdjustments` in
  `CoolingBuffer.cpp`. Tracks the **maximum** `h` observed in the layer's
  G-code (not the average) because the thickest bead dominates the
  cooling time required before the next layer can be safely supported.
  Reset to `0.f` per layer per extruder.

- **`;HEIGHT:` tag** — Inline comment emitted by the G-code writer for
  every extrusion segment carrying its layer height in mm. Parsed by
  `CoolingBuffer::parse_layer_gcode` to feed `max_layer_height`. Required
  for adaptive layer height support. If no tag is found in a layer (e.g.
  custom G-code only), the cooling formula falls back to the user-defined
  `slow_down_layer_time` as a conservative floor.

- **Future width correction** — The current model assumes `width ≥ height`
  so the slab approximation holds. For very wide beads a correction
  `min_time = h² × k × max(1, w/(2h))` is documented in the tooltip but
  intentionally not enabled. The config name was kept generic
  (`cooling_time_per_cross_section`) precisely to allow this evolution
  without a breaking schema change.

---

## File-naming conventions inside `resources/profiles/Ginger Additive/`

- `machine/Ginger G1 <D> nozzle.json` — Machine preset per nozzle diameter.
- `process/<D>mm Standard.json` — Process preset matched to nozzle.
- `process/<D>mm Vasemode.json` — Spiralize variant.
- `filament/*.json` — Material database (PLA, PETG, PP, ASA, glass-filled
  blends...). Includes both Ginger-branded and generic OEM profiles.
- `*_common.json` (`fdm_machine_common`, `fdm_process_common`,
  `fdm_filament_common`) — Inheritance roots. Most other presets `inherits`
  from these.

