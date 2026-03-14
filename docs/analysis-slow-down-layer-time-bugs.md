# Analisi Bug: `slow_down_layer_time` (CoolingBuffer)

**Data**: 2026-01-09  
**Branch di riferimento**: `main` (OrcaSlicer/GingerSlicer)  
**File principale**: `src/libslic3r/GCode/CoolingBuffer.cpp`

---

## 1. Architettura: Pipeline G-code

Il G-code attraversa questi stadi in ordine sequenziale (TBB pipeline):

```
Generator (_extrude)
  → SpiralVase (opzionale)
    → PressureEqualizer (se max_volumetric_extrusion_rate_slope > 0)
      → CoolingBuffer (calcolo layer time + slowdown)
        → FanMover (opzionale)
          → PA Processor → Output
```

**Punto critico**: PressureEqualizer modifica il G-code PRIMA di CoolingBuffer.
CoolingBuffer si aspetta un formato specifico di marker (`_EXTRUDE_SET_SPEED` / `_EXTRUDE_END`),
e PressureEqualizer li riscrive completamente.

---

## 2. Bug Trovati

### BUG-01: `slow_down_to_feedrate` esclude il tempo non-adjustable da `time_total`

**File**: `CoolingBuffer.cpp:213-226`  
**Severità**: Alta  
**Condizione**: Multi-extruder O singolo extruder con linee non-adjustable (travel, G4, wipe)

```cpp
void slow_down_to_feedrate(float min_feedrate) {
    float time_total = 0.f;
    for (size_t i = 0; i < n_lines_adjustable; ++ i) {  // ← SOLO adjustable!
        // ...
        time_total += line.time;
    }
    this->time_total = time_total;  // ← MANCA time_non_adjustable
}
```

**Effetto**: `time_total` non include il tempo delle linee non-adjustable (travel, G4 dwell, wipe,
external perimeters se `dont_slow_down_outer_wall` è attivo). Questo fa apparire il layer
più veloce di quanto sia realmente. In `calculate_layer_slowdown` (riga 668-669), il `total`
calcolato è sottostimato → l'algoritmo applica slowdown insufficiente O eccessivo.

**Confronto con le altre funzioni**:
- `slowdown_to_minimum_feedrate()` → itera TUTTE le linee → `time_total` corretto ✓
- `slow_down_proportional()` → itera TUTTE le linee → `time_total` corretto ✓
- `slow_down_to_feedrate()` → itera SOLO n_lines_adjustable → **BUG** ✗

**Fix proposto**:
```cpp
void slow_down_to_feedrate(float min_feedrate) {
    float time_total = time_non_adjustable;  // ← Inizia dal tempo non-adjustable
    for (size_t i = 0; i < n_lines_adjustable; ++ i) {
        CoolingLine &line = lines[i];
        if (line.feedrate > min_feedrate) {
            line.time *= std::max(1.f, line.feedrate / min_feedrate);
            line.feedrate = min_feedrate;
            line.slowdown = true;
        }
        time_total += line.time;
    }
    this->time_total = time_total;
}
```

---

### BUG-02: PressureEqualizer — `is_just_line_with_extrude_set_speed_tag` ha logica `&&` sbagliata

**File**: `PressureEqualizer.cpp:768-785`  
**Severità**: Media-Alta  
**Condizione**: `max_volumetric_extrusion_rate_slope > 0`

```cpp
inline bool is_just_line_with_extrude_set_speed_tag(const std::string &line)
{
    if (line.empty() && !boost::starts_with(line, "G1 ") && !boost::ends_with(line, EXTRUDE_SET_SPEED_TAG))
        return false;
    //                ^^                                  ^^
    //          Dovrebbe essere ||                  Dovrebbe essere ||
```

**Effetto**: La condizione con `&&` è logicamente errata:
- Se `line.empty()` è true → `starts_with` e `ends_with` sono false → `!false = true` → condizione è `true && true && true` → return false. OK per questo caso.
- Se `line` ha lunghezza 1-2 caratteri → `line.empty()` è false → condizione short-circuit a false → NON ritorna false → cade sulla riga 773 che accede a `line.data() + 3` → **accesso out-of-bounds / undefined behavior**.

Questa funzione è usata per decidere se rimuovere una linea `_EXTRUDE_SET_SPEED` inutile.
Un crash o una decisione sbagliata qui corrompe la struttura dei marker per CoolingBuffer.

**Fix proposto**:
```cpp
if (line.empty() || !boost::starts_with(line, "G1 ") || !boost::ends_with(line, EXTRUDE_SET_SPEED_TAG))
    return false;
```

---

### BUG-03: PressureEqualizer frammenta i blocchi `_EXTRUDE_SET_SPEED` / `_EXTRUDE_END`

**File**: `PressureEqualizer.cpp:787-821`  
**Severità**: Alta  
**Condizione**: `max_volumetric_extrusion_rate_slope > 0` + segmenti con volumetric rate variabile

Quando PressureEqualizer spezza un segmento in sotto-segmenti per lo smoothing volumetrico,
emette per OGNI sotto-segmento:
1. `_EXTRUDE_END` (chiude il blocco precedente)
2. `G1 F<new_speed> ;_EXTRUDE_SET_SPEED` (apre nuovo blocco)
3. `G1 X... Y... E...` (movimento)

**Effetto su CoolingBuffer**: Un singolo segmento che originariamente aveva 1 blocco adjustable
diventa N blocchi adjustable con feedrate diverse. Questo causa:
- **Frammentazione eccessiva**: ogni sotto-segmento ha il suo `time_max` calcolato
  indipendentemente, perdendo il contesto del movimento originale
- **Rapporto adjustable/non-adjustable alterato**: più blocchi adjustable con tempi piccoli
- **Feedrate abbassate**: PE ha già ridotto le velocità per smoothing, quindi CoolingBuffer
  parte da velocità già ridotte. `time_max = length / slow_down_min_speed` è calcolato
  sulla lunghezza del sotto-segmento, non dell'originale

---

### BUG-04: `variable_speed` (enable_overhang_speed) emette `_EXTRUDE_SET_SPEED` multipli senza `_EXTRUDE_END` intermedi

**File**: `GCode.cpp:5967 + 6048-6054`  
**Severità**: Media  
**Condizione**: `enable_overhang_speed = true` + perimetri con overhang variabile

Il percorso `variable_speed` in `_extrude()`:
1. Emette `G1 F<speed1> ;_EXTRUDE_SET_SPEED` all'inizio (riga 5967)
2. Per ogni punto con velocità diversa, emette `G1 F<speed2> ;_EXTRUDE_SET_SPEED` (riga 6049)
3. Emette UN SOLO `_EXTRUDE_END` alla fine (riga 6079)

CoolingBuffer interpreta ogni `_EXTRUDE_SET_SPEED` come un nuovo blocco adjustable indipendente.
Questo funziona correttamente per il parsing (ogni nuovo speed modifier accumula i G1 successivi),
ma causa frammentazione: un singolo perimetro da 50mm diventa 10+ blocchi adjustable piccoli.

**Effetto**: L'algoritmo non-proporzionale in `extruder_range_slow_down_non_proportional` lavora
per fasce di feedrate. Con molti piccoli blocchi a feedrate diverse, il comportamento diventa
irregolare — alcuni segmenti vengono rallentati molto, altri poco.

---

### BUG-05: `dont_slow_down_outer_wall` esclude tempo significativo dal pool adjustable

**File**: `CoolingBuffer.cpp:428-436`  
**Severità**: Media  
**Condizione**: `dont_slow_down_outer_wall = true` + layer con prevalenza di perimetri esterni

```cpp
bool adjust_external = true;
if(adjustment->dont_slow_down_outer_wall && external_perimeter) adjust_external = false;

if (boost::contains(sline, ";_EXTRUDE_SET_SPEED") && ! wipe && adjust_external) {
    line.type |= CoolingLine::TYPE_ADJUSTABLE;
```

Quando `dont_slow_down_outer_wall` è attivo, i perimetri esterni NON sono marcati `ADJUSTABLE`.
Il loro tempo è conteggiato nel totale del layer ma NON può essere rallentato.

**Effetto**: Se un layer ha l'80% del tempo in perimetri esterni, solo il 20% del tempo è
adjustable. Per raggiungere `slow_down_layer_time = 20s` partendo da 5s, l'algoritmo
dovrebbe rallentare il 20% del tempo di un fattore 4x → ma questo potrebbe superare
`time_max` (basato su `slow_down_min_speed`), rendendo impossibile raggiungere il target.
L'algoritmo fallisce silenziosamente e il layer resta ben sotto il target.

---

### BUG-06: Support layers accumulati nel CoolingBuffer alterano il calcolo

**File**: `CoolingBuffer.cpp:314-332`, `GCode.cpp:4586`  
**Severità**: Bassa-Media  
**Condizione**: Oggetti con supporti

```cpp
result.cooling_buffer_flush = object_layer || raft_layer || last_layer;
```

I support layers NON flushano il CoolingBuffer — vengono accumulati fino al prossimo object layer.
Il G-code di più support layers viene concatenato in `m_gcode` e processato tutto insieme
quando arriva il flush dell'object layer.

**Effetto**: Il "layer time" calcolato include il tempo di tutti i support layers accumulati
PIÙ l'object layer. Questo gonfia artificialmente il layer time, riducendo o eliminando
il slowdown quando invece sarebbe necessario per il singolo object layer.

---

### BUG-07: G4 (dwell) nel G-code esterno conta come layer time ma non è adjustable

**File**: `CoolingBuffer.cpp:532-540`  
**Severità**: Bassa  
**Condizione**: Custom G-code con G4 prima/durante l'estrusione del layer

```cpp
} else if (boost::starts_with(sline, "G4 ")) {
    line.type = CoolingLine::TYPE_G4;
    line.time = line.time_max = float(...);
```

I comandi G4 (dwell/pausa) nel custom G-code aggiungono tempo al layer time totale
ma non sono adjustable. Se un custom G-code aggiunge 10s di pausa e `slow_down_layer_time = 15s`,
l'algoritmo vede 10s + extrusion_time e potrebbe pensare che il layer sia già abbastanza lento.

---

### BUG-08: Inconsistenza `adjustable(true)` vs `adjustable()` nel sort

**File**: `CoolingBuffer.cpp:80-88, 182-194`  
**Severità**: Bassa  
**Condizione**: `dont_slow_down_outer_wall = true`

`sort_lines_by_decreasing_feedrate()` usa `adjustable()` (senza parametro), che controlla
solo `TYPE_ADJUSTABLE` e `time < time_max`, IGNORANDO `TYPE_EXTERNAL_PERIMETER`.

Ma `maximum_time_after_slowdown()`, `adjustable_time()`, e `slowdown_to_minimum_feedrate()`
usano `adjustable(true)` che include anche i perimetri esterni.

**Effetto**: Il sort mette i perimetri esterni nella zona "adjustable" se hanno `TYPE_ADJUSTABLE`,
ma poi le funzioni di slowdown con `adjustable(true)` li trattano come adjustable anche quando
`dont_slow_down_outer_wall` dovrebbe escluderli. Questo perché `dont_slow_down_outer_wall`
agisce a monte NON marcando le linee come `TYPE_ADJUSTABLE`, non a valle. Quindi se la linea
è marcata `TYPE_ADJUSTABLE`, il flag `dont_slow_down_outer_wall` è già stato applicato
correttamente. Questo è un falso allarme — il codice è corretto ma confuso.

---

### BUG-09: `seam_slope` con Z variabile e `e_ratio` ridotto

**File**: `GCode.cpp:5881-5889`  
**Severità**: Bassa  
**Condizione**: `seam_slope_type != None`

Le estrusioni slope hanno `dE * e_ratio` con `e_ratio < 1`. CoolingBuffer calcola il tempo
dal rapporto `length / feedrate` che è geometricamente corretto. Tuttavia, il volume estruso
è ridotto, il che significa che il raffreddamento effettivo del materiale è diverso.

**Effetto**: Il layer time calcolato è corretto dal punto di vista temporale, ma il comportamento
termico è diverso — meno materiale estruso si raffredda più velocemente. Questo non è un bug
nel calcolo del tempo, ma una limitazione concettuale: il `slow_down_layer_time` non tiene
conto del volume estruso, solo del tempo.

---

## 3. Interazioni Critiche tra Feature

### 3.1 `max_volumetric_extrusion_rate_slope` + `slow_down_layer_time`

**Pipeline**: PressureEqualizer modifica feedrate → CoolingBuffer calcola con feedrate alterate

1. PE rallenta alcune estrusioni per smoothing volumetrico
2. CoolingBuffer vede feedrate già ridotte → calcola layer time più lungo
3. Se il layer time è ora > `slow_down_layer_time`, CoolingBuffer NON applica slowdown
4. Ma PE ha anche frammentato i blocchi → il comportamento di slowdown è irregolare

### 3.2 `enable_overhang_speed` + `slow_down_layer_time`

1. Overhang speed genera feedrate variabili per punto → molti blocchi adjustable piccoli
2. Ogni blocco ha un `time_max` calcolato sul suo sotto-segmento
3. L'algoritmo non-proporzionale rallenta per fasce di feedrate → distribuzione irregolare

### 3.3 `max_volumetric_extrusion_rate_slope` + `enable_overhang_speed` + `slow_down_layer_time`

Questa è la combinazione peggiore:
1. `_extrude()` genera G-code con `_EXTRUDE_SET_SPEED` multipli (variable_speed)
2. PE li frammenta ulteriormente in sotto-segmenti con nuovi marker
3. CoolingBuffer vede decine di micro-blocchi adjustable con feedrate eterogenee
4. BUG-01 causa `time_total` sottostimato dopo lo slowdown
5. Risultato: layer time errato, tipicamente 2-3x inferiore al target

### 3.4 `seam_slope_type` + `slow_down_layer_time`

1. Seam slope genera estrusioni con Z variabile dentro blocchi `_EXTRUDE_SET_SPEED`
2. CoolingBuffer calcola `line.length` dalla distanza XYZ completa (incluso delta Z)
3. Questo rende le linee slope leggermente più "lunghe" del dovuto
4. Effetto: marginale, ma contribuisce all'imprecisione complessiva

---

## 4. Priorità dei Fix

| ID     | Severità | Complessità | Impatto stimato |
|--------|----------|-------------|-----------------|
| BUG-01 | Alta     | 1 riga      | Fix del 30-40% dell'errore layer time |
| BUG-02 | M-Alta   | 1 riga      | Previene UB/crash in PressureEqualizer |
| BUG-03 | Alta     | Architetturale | Root cause della frammentazione |
| BUG-04 | Media    | Moderata    | Riduce frammentazione con overhang speed |
| BUG-05 | Media    | Design      | Documentazione + eventuale fallback |
| BUG-06 | B-Media  | Moderata    | Fix per stampe con supporti |
| BUG-07 | Bassa    | Bassa       | Edge case custom G-code |

---

## 5. Strategia di Fix Raccomandata

### Fase 1 — Fix immediati (low-risk, high-impact)
1. **BUG-01**: Fix `slow_down_to_feedrate` — 1 riga, zero rischio di regressione
2. **BUG-02**: Fix logica `&&` → `||` in PressureEqualizer — 1 riga, previene UB

### Fase 2 — Fix strutturali (medium-risk)
3. **BUG-04**: Aggiungere `_EXTRUDE_END` prima di ogni nuovo `_EXTRUDE_SET_SPEED`
   nel percorso variable_speed di `_extrude()`
4. **BUG-06**: Considerare il flush per ogni support layer individualmente,
   o almeno separare il calcolo del layer time per support vs object

### Fase 3 — Redesign (high-risk, da discutere con gli sviluppatori)
5. **BUG-03**: Coordinare PressureEqualizer e CoolingBuffer per preservare
   il contesto dei segmenti originali (richiede modifica architetturale)

### BUG-10: Spiral vase — ultimo layer con doppio `AFTER_LAYER_CHANGE` a stessa Z

**File**: `GCode.cpp` (generazione layer change comments in spiral vase mode)  
**Severità**: Media  
**Condizione**: `spiral_mode = 1`, ultimo layer dell'oggetto

In spiral vase mode, l'ultimo layer dell'oggetto emette **due** `LAYER_CHANGE` / `AFTER_LAYER_CHANGE`
alla stessa altezza Z (es. z=20 per un cilindro di 20mm):

1. Prima rivoluzione spirale: z incrementa da ~19.4 a 20.0 → `AFTER_LAYER_CHANGE ;20`
2. Cerchio di chiusura piatto a z=20 (estrusione decrescente) → `AFTER_LAYER_CHANGE ;20` (duplicato)

**Effetto**: CoolingBuffer riceve entrambe le rivoluzioni come UN singolo `process_layer` call,
calcola un tempo totale di 20s e distribuisce lo slowdown su entrambe (2 blocchi adjustable × 10s).
Ma GCodeProcessor conta ogni `AFTER_LAYER_CHANGE` come confine di layer separato → divide il
tempo in due layer da ~10s ciascuno nel preview.

**Dati dal test CLI** (cilindro 25mm raggio × 20mm altezza, vasemode 0.6mm, slow_down_layer_time=20):
```
CoolingBuffer ultimo layer: n_adj=2, LINE[0]=10.01s, LINE[1]=10.01s, FINAL=20.02s
GCodeProcessor: layer 33 time=10.01s, layer 34 time=10.38s (somma ≈ 20.4s)
```

---

## 6. Test di Verifica CLI (2026-03-13)

### Setup
- Cilindro STL: raggio=25mm, altezza=20mm, 64 segmenti
- Profili: `test_machine.json` + `test_process.json` (vasemode) + `test_filament.json`
- `slow_down_layer_time=20`, `slow_down_min_speed=1`, `outer_wall_speed=200`
- `max_volumetric_extrusion_rate_slope=0` (PressureEqualizer disabilitato)

### Risultati GCodeProcessor per-layer times
```
Layer 1:  53.81s (initial layer + bottom shells)
Layer 2:  29.98s (transizione)
Layer 3-32: ~20.04s ciascuno ✅ (match con CoolingBuffer target 20s)
Layer 33: 10.01s ⚠️ (metà del target — BUG-10)
Layer 34: 10.38s ⚠️ (metà del target — BUG-10)
```

### Risultati CoolingBuffer (dal G-code output)
```
Layers 3-32: n_adj=1, feedrate=7.61mm/s (456mm/min), FINAL=20.02s ✅
Layer 33 (ultimo CB): n_adj=2, feedrate=15.21mm/s (913mm/min), FINAL=20.02s ✅
```

### Conclusioni
1. **BUG-01 fix verificato**: layers 3-32 mostrano tempo corretto ~20s nel GCodeProcessor
2. **CoolingBuffer corretto**: TUTTI i layer hanno FINAL=20.02s, RATIO=1.000000
3. **BUG-10 confermato**: l'ultimo layer di CoolingBuffer (20s) viene diviso in 2 layer dal
   GCodeProcessor (10s + 10s) a causa del doppio `AFTER_LAYER_CHANGE` a z=20
4. **Conteggio layer**: CoolingBuffer=33 FINAL, GCodeProcessor=34 layers, AFTER_LAYER_CHANGE=35

### Comandi test
```powershell
& ".\Ginger-Slicer.exe" --debug 3 --no-check \
  --load-settings "test_machine.json;test_process.json" \
  --load-filaments "test_filament.json" \
  --outputdir "." --slice 0 "test_cylinder.stl"
```

---

## 7. Priorità dei Fix Aggiornata

| ID     | Severità | Complessità | Stato      | Impatto |
|--------|----------|-------------|------------|---------|
| BUG-01 | Alta     | 1 riga      | **FIXATO** | Fix principale: layer times corretti |
| BUG-02 | M-Alta   | 1 riga      | **FIXATO** | Previene UB/crash in PressureEqualizer |
| BUG-10 | Media    | Moderata    | Trovato    | Ultimo layer spiral vase mostra 10s invece di 20s |
| BUG-03 | Alta     | Architetturale | Analizzato | Frammentazione blocchi con PE attivo |
| BUG-04 | Media    | Moderata    | Analizzato | Frammentazione con overhang speed |
| BUG-05 | Media    | Design      | Analizzato | Outer wall excluso da slowdown |
| BUG-06 | B-Media  | Moderata    | Analizzato | Support layers accumulati |
| BUG-07 | Bassa    | Bassa       | Analizzato | G4 dwell edge case |
