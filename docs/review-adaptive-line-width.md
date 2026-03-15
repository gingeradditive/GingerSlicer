# Review: Adaptive Line Width (commit 4cdd0ce794)

## Sommario

| Aspetto | Stato |
|---------|-------|
| Formula geometrica | ✅ Corretta |
| Generazione bande | ✅ Consistente |
| Integrazione config | ✅ Funzionante |
| Test con classic + overhang model | ✅ Produce larghezze compensate |
| **Compatibilità Arachne** | ❌ **NON implementata** |
| Larghezze compensate fisicamente realistiche | ⚠️ Troppo ampie |

---

## BUG-01: Feature non raggiungibile con impostazioni default

**Severità: CRITICA**

La feature è implementata solo in `process_classic()` (PerimeterGenerator.cpp, righe 185-258).
Il default di `wall_generator` è `Arachne` (PrintConfig.cpp, riga 6043).
Il profilo G1_1.8mm_Standard non sovrascrive questo default.

**Conseguenza**: con impostazioni default, `process_arachne()` viene chiamato e la feature
adaptive line width non viene mai eseguita. L'opzione appare nel pannello UI ma non ha effetto.

**Verifica**:
- `wall_generator=arachne` + `adaptive_line_width=true` → stesse larghezze di baseline
- `wall_generator=classic` + `adaptive_line_width=true` → 12 larghezze distinte (funziona)

**Fix necessario**: implementare la compensazione multi-banda anche in `process_arachne()`,
o almeno documentare che la feature richiede `wall_generator=classic`.

---

## BUG-02: Larghezze compensate fisicamente irrealistiche

**Severità: MEDIA**

Con nozzle 1.8mm e layer_height 0.6mm, le bande producono:

| Banda | Offset (mm) | Ratio | W_compensato (mm) | Moltiplicatore |
|-------|-------------|-------|-------------------|----------------|
| 0 | -0.95 | — | 1.90 (base) | 1.00x |
| 1 | -0.16 | — | 1.90 (base) | 1.00x |
| 2 | 0.63 | 1.05 | 2.755 | 1.45x |
| 3 | 1.42 | 2.37 | 4.882 | 2.57x |
| 4 | 2.21 | 3.68 | 7.252 | 3.82x |
| 5 | 3.00 | 5.00 | 9.500 (clamped) | 5.00x |

Una linea di 7.25mm da un nozzle di 1.8mm è 4x il diametro — la fisica dell'estrusione
non può produrre una linea così larga con deposizione uniforme. Il filamento si spanderebbe
in modo incontrollato.

**Raccomandazione**: abbassare `MAX_LINE_WIDTH_MULTIPLIER` per questa feature a ~2.0x
(max 3.6mm per nozzle 1.8mm), oppure usare un clamp dedicato:
```cpp
float max_width = std::min((float)(extrusion_width * 2.0), (float)(nozzle_diameter * 3.0));
```

---

## Problemi minori

### mm3_per_mm scaling lineare (approssimazione)
```cpp
double compensated_mm3_per_mm = extrusion_mm3_per_mm * (compensated_width / extrusion_width);
```
Il volume per mm dipende dall'area della sezione trasversale (rettangolo + semicerchi),
che non scala linearmente con la larghezza. Per larghezze molto maggiori della base,
questo sovrastima leggermente il volume (<5% per width < 2x base).

### Clamp usa extrusion_width invece di nozzle_diameter
Il validatore globale in PrintConfig.cpp usa `nozzle_diameter * MAX_LINE_WIDTH_MULTIPLIER`.
Il clamp locale usa `extrusion_width * MAX_LINE_WIDTH_MULTIPLIER`. Inconsistenza minore.

---

## Test eseguiti

### 1. Cilindro verticale (test_cylinder.stl) — nessun overhang
- Adaptive ON (Arachne): identico a baseline → feature non attiva ✓
- Adaptive OFF: identico → no regression ✓
- Layer times: corretti dopo fix BUG-11 ✓

### 2. Modello overhang (overhang test.stl)
- **Adaptive ON + Arachne**: identico a baseline → feature non attiva (BUG-01)
- **Adaptive ON + Classic**: 12 larghezze distinte (1.61–7.25mm) → feature funzionante ✓
- **Baseline Classic**: 9 larghezze (1.61–2.14mm)
- Nessun crash o errore di slicing ✓

### Larghezze G-code a confronto

**Baseline (classic, adaptive OFF)**:
```
1.61195 : 1    1.7187 : 1    1.72093 : 86    1.72097 : 21
1.9 : 112      1.90227 : 1   1.91007 : 1     2.01851 : 1    2.14124 : 1
```

**Adaptive ON (classic)**:
```
1.61195 : 1    1.7187 : 1    1.72093 : 86    1.72097 : 21
1.9 : 231      1.90227 : 1   1.91007 : 1     2.01851 : 1    2.14124 : 1
2.755 : 238    4.8816 : 171  7.25167 : 53    ← NEW compensated widths
```

---

---

## Fix applicati

### FIX BUG-01: Supporto Arachne

**File**: `src/libslic3r/PerimeterGenerator.cpp`

1. **`process_arachne()`**: Aggiunta generazione band series (`m_lower_polygons_series`,
   `m_upper_polygons_series`, etc.) quando `adaptive_line_width=true`
2. **`traverse_extrusions()`**: Implementata logica multi-banda con `clip_extrusion()`.
   Per ogni banda, i segmenti di estrusione vengono clippati e le coordinate Z (larghezza
   junction) vengono scalate dal fattore di compensazione geometrica.

### FIX BUG-02: Clamp larghezza realistica

**File**: `src/libslic3r/PerimeterGenerator.cpp`

- Aggiunto `ADAPTIVE_MAX_WIDTH_MULTIPLIER = 2.0f` (era `MAX_LINE_WIDTH_MULTIPLIER = 5.0`)
- Applicato sia in `traverse_loops()` (classic) che in `traverse_extrusions()` (Arachne)

### Verifica fix

| Config | Larghezze | Max width | Multiplier |
|--------|-----------|-----------|------------|
| Arachne baseline (OFF) | 9 | 2.14mm | — |
| **Arachne + adaptive ON** | **46** | **3.67mm** | **1.93x** ✓ |
| Classic baseline (OFF) | 9 | 2.14mm | — |
| **Classic + adaptive ON** | **11** | **3.80mm** | **2.00x** ✓ |
| Classic + adaptive (PRIMA fix) | 12 | 7.25mm | 3.82x ✗ |

---

## Raccomandazioni residue

1. **[BASSO]** Usare il calcolo corretto dell'area della sezione per `compensated_mm3_per_mm`
2. **[BASSO]** Rendere `adaptive_width_bands` configurabile
