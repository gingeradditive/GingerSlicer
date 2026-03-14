# Analisi: Discrepanza Layer Time tra CoolingBuffer e GCodeProcessor

## Sommario

Identificati e risolti 2 bug che causavano il malfunzionamento del `slow_down_layer_time`
nel profilo 1.8mm Standard (non-vasemode). Il layer time target di 50s veniva raggiunto
dal CoolingBuffer internamente ma il G-code output produceva solo ~26s secondo GCodeProcessor.

---

## Bug Trovati

### BUG-10: SpiralVase duplica il tag LAYER_CHANGE nella transition gcode
**File**: `src/libslic3r/GCode/SpiralVase.cpp`

**Sintomo**: In vasemode, GCodeProcessor vedeva il doppio dei layer rispetto al reale,
dimezzando il tempo per-layer riportato nella UI.

**Causa**: `SpiralVase::process_layer()` copiava tutte le righe G-code nella
`transition_gcode`, incluso il tag `;LAYER_CHANGE`. Questo causava un tag duplicato
nell'output, che GCodeProcessor interpretava come un cambio layer aggiuntivo.

**Fix**: Filtrare `;LAYER_CHANGE` e `; CHANGE_LAYER` dalla transition gcode.

**Impatto**: Vasemode 3.0mm, 5.0mm, 8.0mm ora mostrano layer time corretto (~50s).

---

### BUG-11: CoolingBuffer phantom E distance da reset relative-E tardivo
**File**: `src/libslic3r/GCode/CoolingBuffer.cpp`

**Sintomo**: Per il profilo 1.8mm Standard, CoolingBuffer accumulava ~334mm di distanza
per layer 3 invece dei reali ~170mm, raddoppiando la stima. Il slowdown risultante
produceva una velocità troppo alta, e GCodeProcessor riportava ~26s invece del target 50s.

**Causa root**: In `parse_layer_gcode()`, il reset di `current_pos[3]` (E relativo)
avveniva **dopo** la copia in `new_pos`:

```cpp
// PRIMA (bug):
std::vector<float> new_pos(current_pos);  // new_pos[3] = E_prev
// ... parsing (no E on F-only line) ...
if (use_relative_e_distances)
    current_pos[3] = 0.f;                 // reset current, ma new_pos invariato
float dif[3] = new_pos[3] - current_pos[3];  // = E_prev - 0 = E_prev (FANTASMA!)
```

Le righe `G1 F3060;_EXTRUDE_SET_SPEED` (solo feedrate, nessun E) ereditavano il valore E
della riga precedente in `new_pos[3]`. Dopo il reset di `current_pos[3]` a 0, la differenza
`dif[3] = E_prev` veniva erroneamente trattata come movimento dell'estrusore, assegnando
una lunghezza fantasma (~3.7mm) alla CoolingLine del speed-modifier.

Questa lunghezza fantasma si sommava alla lunghezza reale dell'estrusione accumulata
(~3.5mm), producendo ~7.2mm per entry invece di ~3.5mm → esattamente 2x.

**Fix**: Spostare il reset di `current_pos[3]` **prima** dell'inizializzazione di `new_pos`:

```cpp
// DOPO (fix):
if ((line.type & TYPE_G92) == 0 && use_relative_e_distances)
    current_pos[3] = 0.f;                 // reset PRIMA della copia
std::vector<float> new_pos(current_pos);  // new_pos[3] = 0
```

**Impatto**: 1.8mm Standard layer 3 ora 50.7s (era 26s). Nessuna regressione su vasemode.

---

## Diagnostica Aggiunta

Aggiunta infrastruttura `m_cooling_debug` in CoolingBuffer che scrive commenti diagnostici
nel G-code output (visibili con `grep DEBUG_COOLING plate_1.gcode`):

- `;DEBUG_COOLING extruder=... time_total=... time_maximum=...` — parametri per-extruder
- `;DEBUG_COOLING SLOWDOWN_NON_PROP ...` — dettagli algoritmo slowdown
- `;DEBUG_COOLING RESULT time_after_slowdown=...` — tempi post-slowdown
- `;DEBUG_COOLING LINE[N] feedrate=... length=...` — prime 5 righe adjustable
- `;DEBUG_COOLING APPLY gap_f_overrides=...` — conteggio override feedrate nei gap

Questi commenti non influenzano lo slicing e possono essere rimossi in futuro.

---

## Risultati Verificati

| Profilo | Layer 3 Prima | Layer 3 Dopo | Target |
|---------|--------------|-------------|--------|
| 1.8mm Standard | 26s | **50.7s** ✓ | 50s |
| 3.0mm Vasemode | 25s (split) | **50.0s** ✓ | 50s |
| 5.0mm Vasemode | — | **50.0s** ✓ | 50s |
| 8.0mm Vasemode | — | **50.0s** ✓ | 50s |

## File Modificati

| File | Tipo | Bug |
|------|------|-----|
| `src/libslic3r/GCode/SpiralVase.cpp` | fix | BUG-10 |
| `src/libslic3r/GCode/CoolingBuffer.cpp` | fix + diag | BUG-11 |
| `src/libslic3r/GCode/CoolingBuffer.hpp` | diag | — |
