# Troubleshooting

**`TressFXHair` is not in the Create Node dialog.**
The extension did not load. Check that `addons/tressfx/bin` contains a library for your platform
(`libtressfx.linux.template_debug.x86_64.so`, `libtressfx.windows.template_debug.x86_64.dll`,
`libtressfx.macos.template_debug.framework`). Debug libraries are used by the editor and debug
exports, release libraries by release exports. If your platform is missing, see
[Building](Building.md). Also check the editor output for a message about the `.gdextension`
file.

**Linux: `Can't open dynamic library ... version 'GLIBC_2.xx' not found`.**
The library was built on a system with a newer glibc than yours. The binaries of version 0.3.0
and earlier need glibc 2.43; later ones need 2.28, the same as Godot 4.7. Update the addon, or
build it from source on your machine (see [Building](Building.md)).

**The node exists but nothing is drawn.**
- The project must use the Forward+ or Mobile renderer. Under Compatibility (OpenGL) there is
  no `RenderingDevice` and the nodes stay silent.
- The output panel says *compute shader not loaded*: the shaders in `addons/tressfx/shaders` are
  not imported yet. Restart the editor or run `godot --headless --import` once.
- `tfx_path` is empty or wrong, or the file is not TressFX format version 4. Run the editor with
  `--verbose` to see the load message with the strand count.
- The hair is there but tiny or huge: the asset is in centimetres, set `import_scale` to 0.01.

**Thin strands flicker or look like noisy lines.**
Turn on TAA (*Project Settings > Rendering > Anti Aliasing > Use TAA*). If your material uses a
copy of the strand shader from a version before 0.2, it still renders with alpha-to-coverage,
which needs MSAA 4x; switch to the current shader or change its `render_mode` to
`world_vertex_coords, cull_disabled, depth_prepass_alpha`.

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

**`debug_print_distances()` prints "cells unset" for the whole field.**
The collider is not listed in any hair's `collision_meshes`; fields nobody uses are not built.

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

**The editor crashes when you click `positions` under the material's shader parameters.**
Versions 0.3.0 and earlier stored the simulation texture in the material, and Godot 4.7's texture
inspector crashes on it. The node now keeps it in an internal copy of the material and clears it
from your material when the scene loads, so the inspector no longer offers it. A scene or material
file saved by an old version keeps an empty texture entry until the material is saved again; it
is harmless.

**The output fills with `Dispatch amount of X compute groups (...) is larger than device limit (65535)`.**
Versions 0.3.0 and earlier dispatched large distance fields (`num_cells_x` 63 and above) and very
large hair assets in one go, more than many GPUs accept (65535 groups, the minimum Vulkan
guarantees and always the case under Direct3D 12). The collider then stopped working. Update the
addon; the work is now split into several dispatches.

**The scene file grew huge after saving.**
Only the node properties are saved. If the file contains the hair mesh data, an older version of
the addon was used; resave with the current one.
