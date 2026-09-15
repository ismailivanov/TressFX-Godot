# Troubleshooting

**`TressFXHair` is not in the Create Node dialog.**
The extension did not load. Check that `addons/tressfx/bin` contains a library for your platform
(`libtressfx.linux.template_debug.x86_64.so`, `libtressfx.windows.template_debug.x86_64.dll`,
`libtressfx.macos.template_debug.framework`). Debug libraries are used by the editor and debug
exports, release libraries by release exports. If your platform is missing, see
[Building](Building.md). Also check the editor output for a message about the `.gdextension`
file.

**The node exists but nothing is drawn.**
- The project must use the Forward+ or Mobile renderer. Under Compatibility (OpenGL) there is
  no `RenderingDevice` and the nodes stay silent.
- The output panel says *compute shader not loaded*: the shaders in `addons/tressfx/shaders` are
  not imported yet. Restart the editor or run `godot --headless --import` once.
- `tfx_path` is empty or wrong, or the file is not TressFX format version 4. Run the editor with
  `--verbose` to see the load message with the strand count.
- The hair is there but tiny or huge: the asset is in centimetres, set `import_scale` to 0.01.

**The hair is a flat sheet, or looks like strings.**
MSAA is off. Set *MSAA 3D* to 4x and turn on TAA in the project settings.

**The hair stands on end or floats away from the head.**
The rest pose sits inside a collider. Shrink the collider until it lies just inside the skin,
or raise `collision_skip_root_vertices`. See [Tuning](Tuning.md), *Colliders*.

**The hair goes through the body.**
- The collider is not in the hair's `collision_meshes`.
- The collider is too coarse: raise `num_cells_x`.
- The hair moved faster than the field can push it out: lower `clamp_position_delta` or raise
  `push_limit`.
- The strand is deep inside the collider where the field is not built; the hair has to start
  outside.

**The hair explodes or flies across the level.**
The character was teleported. Call `reset_positions()` after a teleport. If it happens on
load, the skeleton's rest pose and the `.tfx` do not match (wrong skeleton, wrong bone names,
wrong scale).

**Vertices flicker at rest.**
See [Tuning](Tuning.md), *Jitter*.

**The hair does not follow the animation.**
`hair_skeleton` is not set, `tfxbone_path` is empty, or the bone names in the `.tfxbone` do not
match the skeleton. Missing bones are reported as warnings in the output panel; every strand on a
missing bone falls back to bone 0.

**The editor is slow with the scene open.**
The hair simulates in the editor and sleeps after five seconds without changes. If it never sleeps,
something changes every frame: an AnimationPlayer set to autoplay, or a non-zero
`wind_magnitude` (wind gusts continuously). Hair shadows (`cast_hair_shadows`) are also expensive
in the editor viewport.

**Two hair nodes share a material and one of them looks wrong.**
Each node writes its own simulation texture into its material; give every node its own
`ShaderMaterial` (duplicate the resource).

**The scene file grew huge after saving.**
Only the node properties are saved. If the file contains the hair mesh data, an older version of
the addon was used; resave with the current one.
