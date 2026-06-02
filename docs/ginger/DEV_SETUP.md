# GingerSlicer — Developer Setup

Onboarding for new contributors. The goal: get a build going, and (optionally)
make code navigation work in your editor without fighting configuration.

> TL;DR — To **build**, you only need Visual Studio 2022 + the prebuilt deps.
> Code navigation works out of the box in **Visual Studio**. Editors that use
> **clangd** (VS Code, Cursor, Neovim) or **AI agents** (Claude Code, Serena
> MCP) need one extra step: generate `compile_commands.json`.

---

## 1. Build (all editors)

Prerequisites: Visual Studio 2022 with the **Desktop development with C++**
workload, CMake, and Git.

Dependencies are prebuilt and vendored under `deps/build/OrcaSlicer_dep/`. If
they are missing, build them once:

```bat
build_release_vs2022.bat -d
```

Then the canonical build:

```bat
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target GingerSlicer --config Release --parallel 16
```

Output: `build/src/Release/GingerSlicer.dll`, loaded by
`GingerSlicer_app_gui.exe`.

Fast inner loop while editing `src/libslic3r/`:

```bat
cmake --build build --target libslic3r --config Release --parallel 16
```

GUI-only changes (`src/slic3r/GUI/*`) live in `libslic3r_gui` and require the
full `GingerSlicer` target to compile and link.

---

## 2. Code navigation

### Option A — Visual Studio (nothing to configure)

Open the generated solution under `build/` (or the folder). VS uses its own
IntelliSense engine reading the `.vcxproj` files. No `compile_commands.json`,
no clangd. This is the simplest path.

### Option B — VS Code / Cursor / Neovim / AI agents (clangd)

clangd needs a **compilation database**. Generate it once (and after any
CMake change that adds/removes files):

```powershell
powershell -ExecutionPolicy Bypass -File scripts/gen_compile_commands.ps1
```

> The script requires **Ninja** (`winget install Ninja-build.Ninja`). It loads
> the MSVC Developer environment automatically. It writes
> `build-cdb/compile_commands.json` and copies it to the repo root. Both are
> git-ignored (they contain machine-specific absolute paths).

The committed **`.clangd`** file (repo root) already points clangd at
`build-cdb/`. The one thing `.clangd` cannot set is `--query-driver`, which
clangd needs to learn MSVC/Windows SDK system headers from `cl.exe`. Pass it
via your editor:

- **VS Code / Cursor:** install the *clangd* extension, then copy the keys
  from `docs/ginger/vscode-settings.sample.json` into your (git-ignored)
  `.vscode/settings.json`. The important flag is
  `--query-driver=**/cl.exe`.
- **Neovim / other LSP clients:** add the same arguments to your clangd
  launch config.

Without `--query-driver`, clangd reports thousands of false
`'...' file not found` errors. With it, those disappear.

### clangd limitation you cannot fix

Config options declared via the `PRINT_CONFIG_CLASS_DEFINE(...)` macros in
`src/libslic3r/PrintConfig.hpp` (e.g. `fill_multiline`, `pellet_ers_mode`,
`layer_height`) are macro-generated. clangd cannot expand them, so
go-to-definition / find-references returns nothing. Use a plain text search
for the option name instead. This is intrinsic to the macro pattern, not a
setup problem.

---

## 3. Environment gotchas (Windows)

- **`pwsh` may not be installed.** Many Windows machines only have Windows
  PowerShell 5.1. Run `.ps1` scripts with
  `powershell -ExecutionPolicy Bypass -File scripts/<name>.ps1`. The `pwsh`
  invocations shown in some script headers are interchangeable with
  `powershell`.
- **Formatting:** run `clang-format -i <file>` before committing (config in
  `.clang-format`). The CMake `clang-format` target is available when LLVM
  tools are on PATH.

---

## 4. More context

- `AGENTS.md` — module map, build commands, profile CI, agent workflow tips.
- `docs/ginger/GLOSSARY.md` — pellet + multiline-infill domain terminology.
- `docs/ginger/CLIPPER2_MIGRATION.md` — Clipper2 multiline infill migration log.
