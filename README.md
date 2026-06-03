<div align="center">

<picture>
  <img alt="OrcaSlicer logo" src="resources/images/OrcaSlicer.png" width="15%" height="15%">
</picture>

# GingerSlicer

**GingerSlicer** is an open-source 3D slicer based on OrcaSlicer and tailored for large-format pellet extrusion workflows.

It is configured around Ginger Additive hardware, pellet materials, wide nozzles, realistic throughput estimation, and the practical constraints of FGF (Fused Granulate Fabrication).

[Download](https://www.gingeradditive.com/pages/downloads) · [Releases](https://github.com/gingeradditive/GingerSlicer/releases) · [Ginger Additive](https://www.gingeradditive.com/) · [OrcaSlicer Wiki](https://github.com/OrcaSlicer/OrcaSlicer/wiki)

</div>

## What this project is

GingerSlicer keeps the foundation of OrcaSlicer, a powerful open-source slicer for FFF/FDM 3D printers, and extends it for pellet-based additive manufacturing.

The project focuses on predictable material flow, large nozzle behavior, pellet-specific machine profiles, and workflows where extrusion is driven by volume rather than filament length.

## Why GingerSlicer exists

Pellet printers behave differently from filament printers:

- **Screw-based extrusion**  
  Material flow depends on screw rotation volume, melt behavior, pressure buildup, and material viscosity.

- **Large nozzles**  
  Ginger profiles cover large-format nozzle workflows, including 1.0 mm to 8.0 mm nozzle classes.

- **Slow pressure response**  
  Pellet extruders cannot rely on classic filament retraction alone; flow ramps and decompression behavior matter.

- **Material variability**  
  Pellets and recycled materials can require stronger per-material tuning than standard filament spools.

GingerSlicer brings these assumptions directly into slicer profiles, settings, and G-code post-processing logic.

## Key features

- **Pellet-focused profiles**  
  Includes Ginger Additive vendor profiles for machine, process, material, and nozzle combinations.

- **Rotation volume workflow**  
  Uses pellet-oriented extrusion calibration concepts, where material output is expressed as volume per screw revolution.

- **Extrusion Rate Smoothing**  
  Extends flow smoothing for pellet extruders so volumetric changes can be ramped more gradually across extrusion paths and travel boundaries.

- **Volume-based cooling**  
  Supports a cooling model based on layer height and material-specific coefficients for more realistic layer-time control on thick pellet layers.

- **Multiline infill**  
  Supports splitting infill into multiple parallel lines, useful for large nozzles where a single oversized bead can over-extrude or blob.

- **Large-format print preparation**  
  Provides slicing tools, layer previews, material estimates, and print-time estimates suitable for understanding pellet printing behavior before and during production.

- **OrcaSlicer feature base**  
  Inherits OrcaSlicer’s calibration tools, process settings, supports, adaptive slicing features, network-printer integrations, and broad FFF/FDM slicing capabilities.

## Downloads

Stable builds are distributed through Ginger Additive and GitHub releases.

- **Ginger Additive downloads**  
  [https://www.gingeradditive.com/pages/downloads](https://www.gingeradditive.com/pages/downloads)

- **GitHub releases**  
  [https://github.com/gingeradditive/GingerSlicer/releases](https://github.com/gingeradditive/GingerSlicer/releases)

Version `3.0.0` provides installers/packages for Windows, macOS, and Ubuntu 24.04 according to the public Ginger Additive download page.

## License

GingerSlicer is distributed under the GNU Affero General Public License v3.0. See [`LICENSE.txt`](LICENSE.txt).

Some optional components and inherited integrations may have additional licensing considerations, including the optional Bambu networking plugin referenced by the upstream project.
