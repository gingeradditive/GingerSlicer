# Extrusion Dynamics — Constant Bead Cross-Section on Screw Extruders

Research notes backing the pellet-ERS design in GingerSlicer: why the bead
cross-section drifts at every flow change, what physics governs it, what the
industry does about it, and how our slicer-side compensation maps onto that
theory.

> Context: Ginger G1 — single screw Ø20 mm, compression ratio 2:1, length
> 230 mm, nozzles 1–8 mm. Material: pellets (rPETG, PLA, PP…).

---

## 1. Problem statement

A constant cross-section bead requires the **nozzle outflow** to track the
geometric demand at every instant:

```
Q_nozzle(t) = A_bead · v_toolhead(t)
```

Every travel, seam, corner and speed change forces `Q` to change. A screw
extruder cannot follow instantaneously: the melt between screw and nozzle
behaves as a **pressurized reservoir** that must charge/discharge before the
outflow settles. The result is the classic transient signature:

- **start of path**: under-extrusion (material goes into charging the
  reservoir instead of the bead);
- **end of path / deceleration**: over-extrusion (the reservoir keeps
  discharging into the bead);
- both stack at the **seam**, which is where a layer ends and starts.

This is not specific to Ginger: it is *the* documented quality limiter of
Big Area Additive Manufacturing (BAAM) and every screw-based system.

## 2. Physical model (lumped, first order)

Two lumped elements dominate:

**Capacitance (melt reservoir).** The molten volume `V_melt` between the
screw metering zone and the nozzle stores material by compression:
`C = dV/dP ≈ V_melt / K_bulk`, with polymer melt bulk modulus
`K_bulk ≈ 1–2 GPa`. Screw/barrel mechanical compliance and entrapped gas
add to `C`.

**Resistance (nozzle + die channel).** For a Newtonian melt,
Hagen–Poiseuille gives `R = 8ηL/(πr⁴)`: the outflow driven by reservoir
pressure is `Q_nozzle = P/R`. Polymer melts are shear-thinning (power-law
index n ≈ 0.3–0.5), which softens the exponent but keeps the trend: **R
rises steeply as the nozzle shrinks**.

Mass balance on the reservoir yields a **first-order lag**:

```
τ · dQ_nozzle/dt + Q_nozzle = Q_screw        with   τ = R·C
```

Consequences:

- `τ` is a **property of machine + material + nozzle**, not of the print.
- After a step change, the outflow covers 63% of the gap in τ and ~95% in
  3τ. The defective zone at a path start/end is therefore
  **`L_defect ≈ 3·τ·v`** long — measurable with calipers on any print.
- Shear thinning makes `R` (hence τ) mildly flow-dependent — the residual
  that a linear model cannot capture (covered by the % trims).

### Nozzle scaling: from d⁴ to ~d²

For a power-law melt (`η ∝ γ̇ⁿ⁻¹`), redoing Poiseuille gives
`R ∝ 1/r^(3n+1)`: the Newtonian `r⁴` (r² from the cross-section, r² from
the parabolic velocity profile) is softened by shear thinning.

| n (power-law index) | R exponent | τ ratio, 1 mm vs 8 mm nozzle |
|---|---|---|
| 1 (Newtonian) | 4 | 4096× |
| 0.5 | 2.5 | ~180× |
| **0.35 (typical PETG)** | **~2** | **~70×** |

Practical rule for the G1: **τ scales roughly with the inverse square of
the nozzle diameter.** If τ(8 mm) ≈ 0.05 s, expect τ(1 mm) in the order of
seconds — which is why the small nozzles show the worst seams and feedrate
ramps alone were never enough. **Calibrate τ per nozzle.**

Order-of-magnitude check for the Ginger G1 (screw Ø20×230 mm): melt volume
~15–30 cm³ with K_bulk ≈ 1.5 GPa gives C ≈ 10–20 mm³/MPa; at
Q ≈ 150 mm³/s and 30–80 bar nozzle pressure, τ = R·C ≈ **0.05–0.3 s** for
the large nozzles — consistent with the BAAM measurement.

### Experimental validation from the literature

ORNL/Cincinnati identified the BAAM extruder exactly this way: step changes
in screw RPM, bead cross-section measured with a laser profilometer, MATLAB
system identification. Best fit: **first order, `H(s) = K/(τs+1)`, with
τ ≈ 0.06–0.09 s** across seven runs (BAAM-scale extruder, large nozzle).
They explicitly note that a first-principles model is not feasible (melt
temperature, residence time, shear history, pellet geometry all enter) and
recommend empirical identification — which is what our sweep tool does.

## 3. Compensation strategies in industry & literature

| Approach | Who | Notes |
|---|---|---|
| **Feedforward lead filter** `(τz·s+1)/(τp·s+1)` on the screw command, τz cancels the plant pole | ORNL / Cincinnati BAAM (now standard on CI BAAM) | Identified τ ≈ 0.1 s, production values hand-tuned (τz = 0.03, τp = 0.001). Removed corner bulging/thinning. |
| **State-space shaping** of spindle accel/decel | Univ. of Tennessee (thesis) | Consistent bead at variable speed. |
| **Inverse rheological model feedforward + Smith predictor + pressure feedback** | 2026 MDPI study on PP-GF screw extrusion | Needs a melt pressure transducer in the loop; cuts overshoot 23%→17% and halves speed fluctuation. |
| **Mechanical bypass**: diversion valve ("Posiverter"), polymer exhaust | ORNL, patent US 11,097,473 | Cuts the transient off physically at start/stop — hardware solution to the same τ. |
| **Pressure advance / linear advance** `E_cmd = E + K·dE/dt` | Klipper / Marlin / RepRapFirmware / Duet (filament world) | Same first-order inversion, derived from the filament-as-spring model; K is their τ·gain. |
| **Empirical slicer scripts** (flow vs segment length) | Community (Small-Area-Flow-Comp) | Filament-oriented, no dynamic model. |
| **Proprietary flow-adjust scripts** | QBIG 3D (GLBS large-format pellet printers) | Known to exist, not public. |

**Open-source landscape**: no open-source slicer implements screw-dynamics
compensation (the stock Orca/Prusa "pressure equalizer" only bounds dQ/dt;
firmware pressure advance models the *filament spring*, which does not exist
on a screw machine). GingerSlicer's pellet ERS + τ compensation is, to our
knowledge, the first open implementation.

## 4. Mapping onto the GingerSlicer implementation

The theory above maps 1:1 onto what `PressureEqualizer` does in pellet mode:

1. **Command shaping (ERS ramps)** — bounds `|dQ/dt| ≤ slope` so the screw
   is never asked to do the impossible. The **Sqrt profile is the
   slope-exact shape** (constant dQ/dt along the ramp — the kinematic law);
   the configured slope should approximate the screw's real tracking limit,
   calibrated per nozzle with the sweep tool.
2. **First-order inversion (τ compensation)** — `pellet_ers_pressure_tau`
   scales the extruded amount per ramp segment:
   `E_scale = 1 ± τ·slope/Q(x)`. This is the discrete, slicer-side
   equivalent of ORNL's lead-filter numerator (`u + τ·du/dt`) and of
   firmware pressure advance — but firmware-agnostic and applied exactly
   where the G-code already gets rewritten. During ramp-up the extra
   material charges the reservoir; during ramp-down the stored pressure
   supplies the bead.
3. **Residual trim** — `pellet_ers_rampup_flow` / `pellet_ers_rampdown_flow`
   (%) multiply on top, absorbing what the linear model misses
   (shear-thinning τ(Q), screw feed non-linearity, temperature drift).
   Default 100% = pure model.
4. **Identification** — the parameter sweep (GUI or `--sweep`) is our
   equivalent of ORNL's system identification: print a seam tower sweeping
   τ, read the height where the seam defect vanishes. BAAM's measured
   τ ≈ 0.06–0.09 s (large nozzle) suggests the expected order of magnitude;
   expect substantially larger values on the 1–2 mm nozzles.

### Saturation condition

The ramp-down compensation `1 − τ·s/Q` goes **negative** when
`τ·slope > Q`: the model would demand suction, which the E-scaling clamps
at 5%. The compensation is therefore complete only while

```
τ · slope_eff ≤ min_rate
```

(e.g. min_rate 50 mm³/s and slope 50 mm³/s² → τ usable up to ~1 s). If the
calibrated τ exceeds this bound, the ramp tail stays under-compensated:
raise `pellet_ers_min_rate` or lower the slope. Verified empirically with
the invariant checker at τ = 2 s (clamped tail, residual over-extrusion).

### Known limits / future work

- The model is linear: `τ` calibrated at one flow regime will be slightly
  off at very different flows (shear thinning). The % trims cover this; a
  flow-dependent `τ(Q)` would be the next refinement.
- Residence time and melt temperature drift change `K` and `τ` slowly
  during a print (documented on BAAM); a slicer cannot see this — closing
  the loop needs a melt pressure transducer + firmware feedback (the MDPI
  approach) and is out of slicer scope.
- Corners: BAAM compensates corner speed changes too. Our ERS currently
  ramps at polyline boundaries and internal flow changes; toolhead
  acceleration within a straight (planner-level speed changes invisible to
  the slicer) is not compensated.

## 5. Identification protocol — single-layer circle scan

The ORNL identification (straight beads + laser profilometer + system id)
translates to a cheap, precise protocol for the G1:

**Test artifact**: one circle, Ø150 mm, **1 layer, single wall**, seam at a
known angular position. One polyline → exactly one ramp-down + travel +
ramp-up, all at the seam; the remaining ~470 mm of circumference is steady
state and serves as the width reference.

**Measurement**: scan the printed ring (flatbed scanner ≥600 DPI ≈
0.04 mm/px is enough for mm-scale beads; a profilometer adds height info).
Extract bead width vs arc position: at constant speed and layer height the
width deviation is proportional to the flow deviation
(`w ≈ Q/(v·h)`), so the scan is a direct plot of the extrusion transient.

**What to read from it**:

1. **Transient length** `L` (under-extruded arc after the seam,
   over-extruded arc before it) → first τ estimate without any sweep:
   `τ ≈ L/(3·v)`.
2. **Amplitude** of the width error → how much compensation is missing
   (or overshooting, if τ is set too high).
3. **Asymmetry** between the start and end transients → whether a separate
   deceleration slope / different trims are justified.

**Iteration**: slice the same circle N times with different τ via the CLI
(`--sweep` does not help on a 1-layer part; batch separate slices instead,
e.g. τ = 0, 0.05, 0.1, 0.2, 0.4), print them side by side, scan, pick the
τ that flattens the width profile, refine around it. The residual profile
at the best τ is the input for the % trims.

This complements the multi-layer seam tower (which sweeps τ along Z on a
cylinder): the circle gives a clean, quantitative single-transient
measurement; the tower gives a fast visual bracket over a whole range.

## 6. Sources

- ORNL / Cincinnati — *Extrusion Control for High Quality Printing on BAAM
  Systems*: <https://www.osti.gov/pages/servlets/purl/1530113> (first-order
  identification, lead filter, Posiverter)
- UTK thesis — *Dynamic Extruder Control for Polymer Printing in BAAM*:
  <https://trace.tennessee.edu/utk_gradthes/3805/>
- Georgia Tech — *Extruder Dynamics and Control in Large Scale Additive
  Manufacturing*: <https://repository.gatech.edu/server/api/core/bitstreams/64c78160-2728-405a-b11a-8f07ea2847c7/content>
- MDPI Materials 2026 — *Precise Pressure Control for Screw Extrusion 3D
  Printing of PP-GF…*: <https://www.mdpi.com/1996-1944/19/7/1453>
- Patent US 11,097,473 — *Polymer exhaust for eliminating extruder
  transients*: <https://image-ppubs.uspto.gov/dirsearch-public/print/downloadPdf/11097473>
- *Investigating pressure advance algorithms for filament-based melt
  extrusion AM*: <https://www.researchgate.net/publication/334370719>
- Duet3D research — *Extrusion Behaviour and Pressure Advance*:
  <https://www.duet3d.com/blog/duet3d-research-extrusion-behaviour-and-pressure-advance>
- Klipper docs — *Pressure advance*: <https://www.klipper3d.org/Pressure_Advance.html>
- Dyze Design — *Flow to RPM factor for pellet extruders*:
  <https://dyzedesign.com/2024/05/flow-to-rpm-factor-optimize-your-3d-printing-with-pellet-extruders/>
- Melt conveying modeling (single screw, shear thinning):
  <https://pmc.ncbi.nlm.nih.gov/articles/PMC8912815/>
- Community flow script (filament): <https://github.com/Alexander-T-Moss/Small-Area-Flow-Comp>
