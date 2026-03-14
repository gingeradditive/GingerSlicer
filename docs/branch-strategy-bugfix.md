# Strategia Branch per Bug Fix e Feature Testing

## Obiettivo
Gestire i fix di `slow_down_layer_time` e altre feature sperimentali in modo ordinato,
pronto per essere presentato agli sviluppatori di OrcaSlicer/GingerSlicer.

---

## Struttura Branch

```
main (upstream GingerSlicer)
 │
 ├── slicing-sperimentale-pulito          ← feature: adaptive layer height + line width
 │
 ├── fix/cooling-buffer-layer-time        ← BUG-01 + BUG-02 (fix immediati, 1 riga ciascuno)
 │
 ├── fix/pressure-equalizer-markers       ← BUG-03 (frammentazione PE → CoolingBuffer)
 │
 ├── fix/variable-speed-extrude-markers   ← BUG-04 (_EXTRUDE_END mancanti in variable_speed)
 │
 └── test/cooling-integration             ← branch di integrazione per test combinati
      (merge di tutti i fix + slicing-sperimentale-pulito)
```

---

## Workflow per ogni Fix

### 1. Creare il branch dal main pulito
```bash
git checkout main
git pull upstream-readonly main
git checkout -b fix/cooling-buffer-layer-time
```

### 2. Implementare il fix con commit atomici
Ogni commit deve:
- Risolvere UN SOLO bug (referenziare BUG-XX dall'analisi)
- Avere un messaggio chiaro: `fix: CoolingBuffer slow_down_to_feedrate excludes non-adjustable time (BUG-01)`
- Includere un commento nel codice che spiega il perché del fix

### 3. Testare in isolamento
- Sliceare un modello di test con le impostazioni problematiche
- Confrontare il layer time nel G-code output prima/dopo il fix
- Documentare i risultati nel commit message o in un file di test

### 4. Push e PR-ready
```bash
git push origin fix/cooling-buffer-layer-time
```

### 5. Branch di integrazione per test combinati
```bash
git checkout main
git checkout -b test/cooling-integration
git merge fix/cooling-buffer-layer-time
git merge fix/pressure-equalizer-markers
git merge fix/variable-speed-extrude-markers
git merge slicing-sperimentale-pulito
```

---

## Convenzioni Commit

```
fix: <descrizione breve> (BUG-XX)

<Descrizione dettagliata del problema>

Causa: <root cause>
Fix: <cosa cambia>
Test: <come verificare>
```

Esempio:
```
fix: CoolingBuffer slow_down_to_feedrate excludes non-adjustable time (BUG-01)

slow_down_to_feedrate() only summed adjustable lines into time_total,
missing time_non_adjustable. This caused calculate_layer_slowdown() to
underestimate total layer time, resulting in insufficient or erratic
slowdown when slow_down_layer_time threshold was set.

Causa: Loop iterava solo fino a n_lines_adjustable
Fix: Inizializzare time_total con time_non_adjustable
Test: Sliceare modello con layer < 10mm perimetro, slow_down_layer_time=20s,
      verificare che il layer time nel G-code sia >= 20s
```

---

## Come Condividere con gli Sviluppatori

### Preparare patch esportabili
```bash
# Esportare i fix come patch per condivisione
git format-patch main --output-directory patches/ -o patches/cooling-fixes/
```

### Per ogni fix condiviso:
1. **Riferimento all'analisi**: includere `docs/analysis-slow-down-layer-time-bugs.md`
2. **Before/After**: mostrare layer time nel G-code prima e dopo il fix
3. **Scope minimo**: ogni patch tocca il minor numero di file possibile

### Ordine consigliato dei fix:
1. **BUG-01** + **BUG-02** insieme (2 righe, zero rischio)
2. **BUG-04** separato (variable_speed markers) → più invasivo
3. **BUG-03** separato (PE fragmentation) → richiede discussione architetturale

---

## Modelli di Test Consigliati

| Test | Impostazioni | Cosa Verificare |
|------|-------------|-----------------|
| Cilindro Ø5mm | slow_down_layer_time=20s | Layer time >= 20s nel G-code |
| Overhang 45-75° | + enable_overhang_speed | Layer time stabile, non erratico |
| Cilindro Ø5mm | + max_volumetric_extrusion_rate_slope=5 | Layer time non 3x inferiore |
| Multi-perimetro | + dont_slow_down_outer_wall | Slowdown applicato a inner walls |
| Con supporti | slow_down_layer_time=15s | Layer time per-layer, non accumulato |
