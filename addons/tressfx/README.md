# TressFX for Godot

Real-time hair and fur for Godot 4.7: a native (GDExtension, C++) port of
[AMD TressFX 4.1](https://github.com/GPUOpen-Effects/TressFX). Strands are simulated on the GPU
with compute shaders, skinned to an animated `Skeleton3D`, kept out of signed-distance-field
colliders and drawn as camera-facing ribbons with Marschner-style lighting.

- **Nodes:** `TressFXHair`, `TressFXCollisionMesh`, `TressFXStats` (all documented in the editor's
  Help, F1).
- **Simulation:** velocity shock propagation, global and local shape constraints, length
  constraints, gravity and gusting wind, per-frame SDF collision, follow hairs. All on the GPU.
- **Rendering:** thin-tip ribbons expanded in the vertex shader, body-albedo strand colour,
  Kajiya-Kay diffuse plus two shifted Marschner highlights, root darkening, per-strand colour
  variation, distance LOD, optional shadow casting.
- **Editor:** the hair simulates live in the editor. Move the skeleton, scrub an animation or
  change a parameter and it reacts immediately; when nothing changes it goes to sleep.
- **Cost:** loading a 75k-strand groom takes 40 ms; the main-thread cost per frame is a few
  microseconds per node. Everything else is GPU time.

## Requirements

- Godot 4.7 or later, **Forward+** or **Mobile** renderer (the Compatibility renderer has no
  `RenderingDevice`).
- Prebuilt binaries for Linux x86_64, Windows x86_64 and macOS. Other platforms build from
  source, see below.
- MSAA 4x and TAA are strongly recommended (`rendering/anti_aliasing/quality/msaa_3d = 2`,
  `use_taa = true`). The strands use alpha-to-coverage, and TAA turns the dithered edges into a
  soft volume. Without it thin hair reads as noisy strings.

## Installation

1. Copy `addons/tressfx` into your project (or install it from the Asset Library). The
   `.gdextension` file is picked up automatically; there is no plugin to enable. The demo scenes
   and their assets live in `addons/tressfx/demo` (about 150 MB); delete that folder if you do
   not need them.
2. Make sure `addons/tressfx/bin` contains the library for your platform. Release builds of the
   addon ship them; from a source checkout run `scons` first (see *Building from source*).
3. Restart the editor once so the compute shaders in `addons/tressfx/shaders` are imported.

## Quick start

1. Add a `TressFXHair` node and set `tfx_path` to a `.tfx` file. The hair appears in its rest
   pose and starts simulating.
2. For a skinned character set `hair_skeleton` to the `Skeleton3D` and `tfxbone_path` to the
   matching `.tfxbone` file. Without a skeleton the hair follows the node rigidly (fine for a
   bust or a prop).
3. Add a `TressFXCollisionMesh` for the body: either a `.tfxmesh`, or any `Mesh` (a `CapsuleMesh`
   for a head is a good start; a skinned mesh uses its bone weights). List it in the hair's
   `collision_meshes`.
4. Make a `ShaderMaterial` with `addons/tressfx/shaders/tressfx_strand.gdshader` and assign it to
   `material`. Colour, fiber radius, and the lighting terms live there. One material per hair
   node, the node writes its simulation texture into it.
5. Optional: drop a `TressFXStats` label under a `CanvasLayer` for an on-screen performance
   readout.

The ribbon meshes carry no attributes; the strand shader derives everything from the vertex index,
which keeps GPU memory at about 12 bytes per ribbon vertex.

Everything in the Inspector applies immediately; the properties that need the asset rebuilt
(`tfx_path`, `tfxbone_path`, `hair_skeleton`, `num_follow_hairs`, `follow_radius`,
`import_scale`) reload it in a few milliseconds.

## Assets

- **`.tfx`** guide strands, 4 to 64 vertices per strand (a power of two), TressFX file format
  version 4. Positions are in metres; set `import_scale` to `0.01` for a centimetre asset.
- **`.tfxbone`** four bone indices and weights per guide strand, with the bone names, for
  skinning.
- **`.tfxmesh`** a text collision mesh with bone names, skinned vertices and triangles.

AMD ships a Maya exporter (`upstream/tool/Maya` in the TressFX repository). This repository adds:

- `addons/tressfx/tools/ma_to_tfx.py` reads every `nurbsCurve` from a Maya ASCII file,
  resamples it and writes a `.tfx` (`--split-tail-z` splits a hairstyle into two files, see the
  ponytail demo).
- For Blender there are community add-ons:
  [deathbravo/BlenderTressFxTfxExporter](https://github.com/deathbravo/BlenderTressFxTfxExporter),
  [treviasxk/BlenderExportTressFX](https://github.com/treviasxk/BlenderExportTressFX).

Per-strand parameter groups were removed in TressFX 4, so a hairstyle whose parts must behave
differently is split into several `.tfx` files with one `TressFXHair` node each.

## Tuning

TressFX's shape constraints are position based. A strong `gravity` fights them and the hair never
settles; the AMD sample values are `gravity` 0.09 and `damping` 0.068, and `damping` 0.2 brings
fur fully to rest.

- **Short fur** (RatBoy): sample values, `damping` 0.2, colliders at `num_cells_x` 80 with
  `push_limit` 0.25.
- **Long hair**: `local_stiffness` around 0.3, `global_stiffness` 0.2 over `global_range` 0.5 so
  the authored parting survives, `gravity` 1, `damping` 0.2, and `collision_skip_root_vertices`
  around 5 for a groom rooted inside the head.
- **Ponytail** (Ruby): the tail must not be tied to the head: `global_stiffness` 0.06 over
  `global_range` 0.2, `vsp_coeff` 0.25 (velocity shock propagation copies the head's motion
  rigidly onto the strand; 0.76 makes a helmet), `local_stiffness` 0.35, `damping` 0.1,
  `gravity` 1.5.
- **Wind:** `wind_magnitude` is an acceleration in the same units as gravity; 1 to 3 bends the
  demo fur visibly. It gusts between half and one and a half times the value.

A groom's rest pose must lie outside its colliders: a strand pushed at its base every frame swings
outwards as a whole and ends up standing on end. Keep `collision_margin` at 0 and raise
`collision_skip_root_vertices` instead. Colliders should be closed meshes; hair kinks along open
edges. The SDF only exists in a narrow band around the surface, so fur rooted on a limb that rests
deep inside the body can still creep; a finer `num_cells_x` helps.

## Rendering

- `alpha_to_coverage` plus MSAA instead of TressFX's ShortCut/PPLL order-independent
  transparency: an opaque pass, no sorting.
- The deep hair shadow map is replaced by `root_shadow`, a root-darkening AO term in the material.
- `cast_hair_shadows` is off by default; a million ribbon triangles in every shadow cascade is
  the single biggest cost the hair can add.
- Distance LOD (`lod_enabled`): strands are dealt into four meshes that Godot's visibility ranges
  fade out with distance while the shader widens the remaining fibers.
- The strand colour comes from `base_albedo` sampled at the strand's root UV (the body texture),
  times `base_color`, with an optional `tip_color`.

## Performance

RTX 4060 Ti, 1152x648, MSAA 4x, TAA, vsync off (`godot --disable-vsync -- --bench`):

| Scene | ms/frame |
|---|---|
| RatBoy: 82k strands, 3 skinned SDF colliders (10 M cells), animated | 4.07 |
| Ponytail: 20k strands, 4 primitive colliders (5.5 M cells) | 1.78 |

Breakdown on RatBoy (the `TressFXStats` overlay): hair simulation 1.15 ms GPU, SDF builds
0.92 ms GPU, viewport draw 2.1 ms GPU of which the hair is most, TressFX main-thread cost
0.05 ms. Only the draw scales with resolution. Loading: the 75k-strand fur takes 40 ms, the
13k-vertex body collider 13 ms.

Static colliders are free: a `TressFXCollisionMesh` only rebuilds its distance field when its
pose changes and a hair used it in the previous frame. Hair that is hidden, outside the camera's
view or farther than `simulation_distance` stops simulating (`simulate_offscreen` keeps it
running).

## Editor

The hair simulates in the editor exactly as in the game, so a scene can be tuned live. After
about five seconds without any change to the pose or the parameters the simulation sleeps; any
change wakes it. Inspector edits that reload the asset are native and take milliseconds, and the
compute pipelines are shared by every hair and collider in the scene, so editing does not
recompile anything.

Set `TressFXStats.measure_gpu` (default on) to see the GPU time of the simulation and the SDF
builds; run the editor or game with `--verbose` to log asset loading.

## Demos

- `addons/tressfx/demo/ponytail.tscn` AMD's TressFX 3.1 ponytail (Maya source converted with
  `ma_to_tfx.py`) on the Ruby head, primitive colliders, no skeleton. Drag with the mouse to
  turn the head.
- `addons/tressfx/demo/main.tscn` AMD's RatBoy: fur and mohawk skinned to the animated skeleton, three SDF
  colliders from `.tfxmesh` files. Flags after `--`: `--no-fur`, `--no-mohawk`,
  `--no-collision`, `--no-sdf`, `--hide-hair`, `--no-anim`, `--wind=<f>`, `--bench` (with
  `--disable-vsync`), `--jitter-check` (prints the share of vertices that still jitter),
  `--shot=<dir>` (screenshots).

## Not ported from TressFX

ShortCut and PPLL order-independent transparency, the deep hair shadow map, marching-cubes SDF
visualisation, and capsule collision (compiled out upstream as well). Collision differs from
upstream in three deliberate ways, all to remove jitter: the SDF sign uses vertex and edge
pseudo-normals, only guide strands collide (follow strands are rebuilt from them afterwards), and
the push per frame is capped (`push_limit`).

## License

MIT. See `addons/tressfx/LICENSE.md` for the AMD TressFX and godot-cpp notices. The ponytail demo
asset carries its own license in `addons/tressfx/demo/ponytail/LICENSE.txt`.

Source code, build instructions and the class reference sources live one level up, in the
repository this addon is built from (`src/`, `SConstruct`, `doc_classes/`).
