# Nodes

The same reference is available inside the editor: press F1 and search for the class name.

## TressFXHair

Inherits `Node3D`. One hair object: loads a `.tfx`, simulates it on the GPU, collides it with the
listed `TressFXCollisionMesh` nodes and draws it.

### Asset

| Property | Default | Meaning |
|---|---|---|
| `tfx_path` | `""` | The `.tfx` file with the guide strands. Reloads the asset. |
| `tfxbone_path` | `""` | Optional `.tfxbone` with the bone weights. Reloads. |
| `hair_skeleton` | none | `Skeleton3D` the hair is skinned to. Without one the hair follows this node. Reloads. |
| `num_follow_hairs` | `0` | Follow strands generated around every guide strand. Reloads. |
| `tip_separation` | `0.0` | How much further the follow strands spread towards the tip. |
| `follow_radius` | `0.012` | Spread of the follow strands around their guide, metres. Reloads. |
| `import_scale` | `1.0` | Multiplies the file's positions on load; `0.01` for centimetres. Reloads. |
| `material` | none | `ShaderMaterial` using `tressfx_strand.gdshader`. One per node. |
| `cast_hair_shadows` | `false` | Render the strands into shadow maps. Expensive, see [Performance](Performance.md). |

### LOD

| Property | Default | Meaning |
|---|---|---|
| `lod_enabled` | `false` | Drop strands with distance and widen the remaining fibers. |
| `lod_start_distance` | `1.0` | Distance at which strands start to drop out, metres. |
| `lod_end_distance` | `5.0` | Distance at which only `lod_percent` remain. |
| `lod_percent` | `0.5` | Fraction of strands left at the end distance, rounded to quarters. |
| `lod_width_multiplier` | `2.0` | Fiber width multiplier reached at the end distance. |

### Collision

| Property | Default | Meaning |
|---|---|---|
| `collision_meshes` | `[]` | `TressFXCollisionMesh` nodes the guide strands are pushed out of, all of them every frame. |
| `collision_skip_root_vertices` | `2` | Vertices at the root of each strand that never collide. |

### Simulation

All simulation parameters apply immediately, in the editor and at runtime.

| Property | Default | Meaning |
|---|---|---|
| `vsp_coeff` | `0.758` | Velocity shock propagation: how much of the root's rigid motion is copied onto the whole strand. |
| `vsp_accel_threshold` | `1.208` | Root acceleration above which the coefficient is forced to 1. |
| `local_stiffness` | `0.908` | How strongly each segment keeps its authored bend (curls, waves). |
| `local_iterations` | `2` | Iterations of the local shape constraint per frame. |
| `global_stiffness` | `0.408` | Pull of the first `global_range` of the strand towards the rest shape. |
| `global_range` | `0.308` | Fraction of the strand, from the root, the global constraint applies to. |
| `length_iterations` | `2` | Iterations of the segment length constraint per frame. |
| `damping` | `0.068` | Velocity damping. |
| `gravity` | `0.09` | Gravity acceleration. |
| `wind_direction` | `(1, 0, 0)` | Direction the wind blows towards. |
| `wind_magnitude` | `0.0` | Wind acceleration, gusting between 0.5x and 1.5x. |
| `clamp_position_delta` | `20.0` | Maximum distance a vertex may move in one frame. |

### Culling

| Property | Default | Meaning |
|---|---|---|
| `simulation_distance` | `0.0` | Beyond this distance from the camera the hair stops simulating (0 = never). |
| `simulate_offscreen` | `false` | Keep simulating while hidden or outside the camera's view. Off: unseen hair sleeps, and colliders no hair used last frame are not rebuilt. |

After a sleep longer than a second the hair snaps to its rest pose when it wakes, so a character
that moved while unseen does not have its hair fly across the level.

### Methods

| Method | Meaning |
|---|---|
| `reset_positions()` | Snap the hair back to its rest pose over the next two frames. Call after a teleport. |
| `get_strand_count()` | Drawn strands: guides times (`num_follow_hairs` + 1). |
| `get_guide_strand_count()` | Simulated guide strands, padded to a multiple of 64. |
| `get_vertex_count()` | Total hair vertices. |
| `get_vertices_per_strand()` | 4 to 64, from the file. |
| `get_cpu_time_usec()` | Main-thread time of the last frame, microseconds. |
| `capture_positions()` | Debug: queue a readback of the simulated positions (stalls the GPU). |
| `get_captured_positions()` | Debug: the last readback, four floats per vertex. |

The node is in the `tressfx_hair` group while it is in the tree.

## TressFXCollisionMesh

Inherits `Node3D`. A mesh turned into a signed distance field every frame it moves.

| Property | Default | Meaning |
|---|---|---|
| `tfxmesh_path` | `""` | A `.tfxmesh` file. Reloads. |
| `mesh` | none | Alternative: any `Mesh`, first surface; bone weights used when present. Reloads. |
| `import_scale` | `1.0` | Multiplies vertex positions on load. Reloads. |
| `hair_skeleton` | none | Skeleton driving the mesh (bone names of the `.tfxmesh`, or the skinned mesh's weights). Reloads. |
| `num_cells_x` | `50` | Cells across the mesh's bounding cube (4 to 128). The grid is padded by 80% per side. Reloads. |
| `collision_margin` | `0.0` | Hair is kept this many cells away from the surface. |
| `push_limit` | `1.0` | Most a vertex is pushed out per frame, in cells. |
| `follow_bone` | `""` | Bone whose motion moves the grid with the mesh. |

| Method | Meaning |
|---|---|
| `get_cell_count()` | Cells per axis, padding included. |
| `get_vertex_count()`, `get_triangle_count()` | Size of the loaded mesh. |
| `get_cpu_time_usec()` | Main-thread time of the last frame. |
| `debug_print_distances(points)` | Debug: read the field back and print the distance at the given world points. |

The node is in the `tressfx_collision` group while it is in the tree. Its distance field is only
rebuilt on frames where its pose changed and some hair simulated against it in the previous
frame, so a static collider, or one whose hair is asleep, costs nothing.

## TressFXStats

Inherits `Label`. A runtime overlay, inert in the editor. Drop it under a `CanvasLayer`; it
anchors itself to the top right and shows
the frame rate, viewport draw time, hair simulation and distance field GPU time, the main-thread
cost of every TressFX node, strand and cell counts, and the triangles drawn this frame.

| Property | Default | Meaning |
|---|---|---|
| `measure_gpu` | `true` | Capture `RenderingDevice` timestamps around the compute passes. |

## The strand material

`addons/tressfx/shaders/tressfx_strand.gdshader`, used through a `ShaderMaterial`:

| Parameter | Default | Meaning |
|---|---|---|
| `base_albedo` | white | Texture sampled at the strand's root UV (the body's albedo). |
| `base_color` | white | Multiplies the albedo. |
| `tip_color`, `tip_percentage` | white, `0` | Blend towards `tip_color` over the last `tip_percentage` of the strand. |
| `fiber_radius` | `0.0021` | Strand radius at the root, metres. |
| `fiber_ratio`, `thin_tip` | `0.463`, on | Tip radius as a fraction of the root radius. |
| `expand_pixels` | `0.71` | Extra width, in pixels, faded out for anti-aliasing. |
| `k_diffuse` | `0.07` | Kajiya-Kay diffuse. |
| `k_spec1`, `spec_exp1` | `0.0017`, `14.4` | Primary highlight, shifted towards the root, light coloured. |
| `k_spec2`, `spec_exp2` | `0.072`, `11.8` | Secondary highlight, shifted towards the tip, hair coloured. |
| `cone_angle_deg` | `10` | Highlight shift angle. |
| `root_shadow` | `0.5` | Darkening towards the root, stands in for hair self-shadowing. |
| `color_variation` | `0.3` | Per-strand brightness and tint jitter. |
| `tip_fade` | `0.25` | Fade over the last part of each strand. |

`positions`, `verts_per_strand`, `lod_start`, `lod_end` and `lod_width_multiplier` are written
by the node; leave them alone.
