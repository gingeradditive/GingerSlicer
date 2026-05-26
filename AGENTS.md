# Repository Guidelines

## Project Structure & Module Organization
OrcaSlicer’s C++17 sources live in `src/`, split by feature modules and platform adapters. User assets, icons, and printer presets are in `resources/`; translations stay in `localization/`. Tests sit in `tests/`, grouped by domain (`libslic3r/`, `sla_print/`, etc.) with fixtures under `tests/data/`. CMake helpers reside in `cmake/`, and longer references in `doc/` and `SoftFever_doc/`. Automation scripts belong in `scripts/` and `tools/`. Treat everything in `deps/` and `deps_src/` as vendored snapshots—do not modify without mirroring upstream tags.

## Build, Test, and Development Commands
Use out-of-source builds:
- `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release` configures dependencies and generates build files.
- `cmake --build build --target OrcaSlicer --config Release` compiles the app; add `--parallel` to speed up.
- `cmake --build build --target tests` then `ctest --test-dir build --output-on-failure` runs automated suites.
Platform helpers such as `build_linux.sh`, `build_release_macos.sh`, and `build_release_vs2022.bat` wrap the same flow with toolchain flags. Use `build_release_macos.sh -sx` when reproducing macOS build issues, and `scripts/DockerBuild.sh` for reproducible container builds.

## Coding Style & Naming Conventions
`.clang-format` enforces 4-space indents, a 140-column limit, aligned initializers, and brace wrapping for classes and functions. Run `clang-format -i <file>` before committing; the CMake `clang-format` target is available when LLVM tools are on your PATH. Prefer `CamelCase` for classes, `snake_case` for functions and locals, and `SCREAMING_CASE` for constants, matching conventions in `src/`. Keep headers self-contained and align include order with the IWYU pragmas.

## Testing Guidelines
Unit tests rely on Catch2 (`tests/catch2/`). Name specs after the component under test—for example `tests/libslic3r/TestPlanarHole.cpp`—and tag long-running cases so `ctest -L fast` remains useful. Cover new algorithms with deterministic fixtures or sample G-code stored in `tests/data/`. Document manual printer validation or regression slicer checks in your PR when automated coverage is insufficient.

## Commit & Pull Request Guidelines
The history favors concise, sentence-style subject lines with optional issue references, e.g., `Fix grid lines origin for multiple plates (#10724)`. Squash fixups locally before opening a PR. Complete `.github/pull_request_template.md`, include reproduction steps or screenshots for UI changes, and mention impacted presets or translations. Link issues via `Closes #NNNN` when applicable, and call out dependency bumps or profile migrations for maintainer review.

## Security & Configuration Tips
Follow `SECURITY.md` for vulnerability reporting. Keep API tokens and printer credentials out of tracked configs; use `sandboxes/` for experimental settings. When touching third-party code in `deps_src/`, record the upstream commit or release in your PR description and run the relevant platform build script to confirm integration.

## Module Map (Ginger-specific)
Use this as a routing table before searching. Localize first, then read narrowly.

### Configuration & schema
- `src/libslic3r/PrintConfig.hpp` / `PrintConfig.cpp`: source of truth for every parameter. Definitions are macro-generated via `PRINT_CONFIG_CLASS_DERIVED_*`. Adding a parameter requires touching both files (declaration + `ConfigOptionDef` with label/tooltip/range/mode).
- `src/libslic3r/Config.hpp` / `Config.cpp`: generic `DynamicConfig` / `ConfigOptionDef` plumbing. Rarely modified.
- `src/libslic3r/Preset.hpp` / `Preset.cpp`: `Preset`, `PresetCollection`, inheritance resolution, serialization (system vs user), diff vs parent.
- `src/libslic3r/PresetBundle.hpp` / `PresetBundle.cpp`: load/save bundles, vendor profiles, filament library, project-embedded presets. Entry point `load_vendor_configs_from_json`.

### Pellet-specific code paths
- `src/libslic3r/GCode/PressureEqualizer.hpp` / `.cpp`: pellet ERS (Extrusion Rate Smoothing). Key fields `m_pellet_ers_mode`, `m_pellet_ers_travel_threshold`, `m_pellet_ers_ramp_profile`, `m_pellet_ers_deceleration_slope`, `m_pellet_ers_min_rate`. Mini-pass logic and ramp segments are tagged via `GCodeLine::pellet_ramp`.
- `src/libslic3r/PrintConfig.hpp` lines around `pellet_ers_*`, `pellet_modded_printer`, `use_active_pellet_feeding`, `extruder_rotation_volume`, `mixing_stepper_rotation_volume`.
- `resources/profiles/Ginger Additive/`: vendor profiles. Nozzle sizes 1.0 / 1.8 / 3.0 / 5.0 / 8.0 mm; process profiles 0.60 / 0.90 / 1.30 / 1.50 / 2.50 / 4.00 mm. All files share a single `version` that must be bumped together (see CI rules below).

### Settings UI
- `src/slic3r/GUI/Tab.hpp` / `Tab.cpp`: imperative construction of setting pages (Print / Filament / Printer). To add a UI line: `add_options_page` → `new_optgroup` → `append_single_option_line("opt_key")`.
- `src/slic3r/GUI/ConfigManipulation.hpp` / `.cpp`: cross-field toggles and validations. `toggle_print_fff_options` is the central dispatcher.
- `src/slic3r/GUI/Field.hpp` / `.cpp`: concrete widget types (`TextCtrl`, `CheckBox`, `Choice`, `ColourPicker`...).
- `src/slic3r/GUI/OG_CustomCtrl.hpp` / `.cpp`: custom-drawn options group, the visual unifier across platforms.
- `src/slic3r/GUI/ParamsPanel.hpp` / `.cpp`: hosts the Print/Filament/Printer tabs in the main window.

### Slicing pipeline (read-only mental model)
- `src/libslic3r/Print.cpp` → `PrintObject.cpp` → `PrintObjectSlice.cpp` → `LayerRegion.cpp` → `PerimeterGenerator.cpp` → `GCode.cpp` (375k+ lines, the orchestrator) → `GCode/PressureEqualizer.cpp` (post-process pass).
- `src/libslic3r/GCode/CoolingBuffer.cpp`: layer-time estimation and cooling logic. Useful for layer-time-aware analysis features.

### Profiles distribution & CI
- `.github/workflows/check_profiles.yml`: validates profiles on PR; runs `OrcaSlicer_profile_validator` + `scripts/orca_extra_profile_check.py` + `scripts/check_profile_version_bump.py`.
- `scripts/check_profile_version_bump.py`: requires **every** profile JSON to have `version` strictly greater than `origin/main`. Modifying one file means bumping all 42.
- `scripts/pack_profiles.sh`: packages OTA bundles uploaded to the `nightly-builds` GitHub release.
- `src/slic3r/Utils/PresetUpdater.cpp`: client-side OTA fetch and install of bundles.

## Agent Workflow Tips
- **Localize before reading.** Use `code_search`/`grep_search` to find the 1–3 relevant files; never read large files (`Tab.cpp` 316k, `Plater.cpp` 631k, `GCode.cpp` 375k) end-to-end.
- **Prefer `.hpp` first** for orientation, then jump to `.cpp` definitions with `grep_search` on the symbol.
- **Bumping profile versions:** when changing any file under `resources/profiles/Ginger Additive/`, bump `version` in all 42 JSON files together (see `scripts/check_profile_version_bump.py`).
- **Adding a parameter end-to-end** touches: `PrintConfig.hpp` (declaration) → `PrintConfig.cpp` (`def` with label/tooltip/range/mode) → `Preset.cpp` (`s_*_options` list for category) → `Tab.cpp` (`append_single_option_line`) → `ConfigManipulation.cpp` (toggle/visibility) → optional `resources/profiles/.../*.json` (defaults).
- **C++ symbol search:** if `compile_commands.json` exists (run `scripts/gen_compile_commands.ps1`), prefer LSP-driven tools (clangd/Serena) over regex. Without it, fall back to `grep_search` on declarations.
- **clangd gotcha — config options are macro-generated.** Symbols declared inside `PRINT_CONFIG_CLASS_DEFINE(...)` (e.g. `adaptive_layer_height`, `pellet_ers_mode`, `layer_height`, all `((ConfigOption*, name))` entries in `PrintConfig.hpp`) are produced by Boost.PP-style macros that clangd cannot expand. `Shift+F12` / find-references returns nothing for these. Use `grep_search` directly with the option name as a literal string. This is not a bug in our setup — it is an intrinsic limit of LSP indexing on this macro pattern.
- **Domain glossary:** see `docs/ginger/GLOSSARY.md` for pellet-specific terms (ERS, ramp profile, deceleration slope, virtual retract, etc.).
