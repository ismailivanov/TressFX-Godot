# Getting Started

## 1. Install

1. Copy the `addons/tressfx` folder into your project (or install the addon from the Godot Asset
   Store). There is no plugin to enable: Godot loads `addons/tressfx/tressfx.gdextension` on
   its own.
2. Restart the editor once so the compute shaders in `addons/tressfx/shaders` are imported.
3. Check that it worked: open the *Create New Node* dialog and search for `TressFXHair`. If the
   class is missing, see [Troubleshooting](Troubleshooting.md).

The addon ships its three demo scenes in `addons/tressfx/demo` (about 150 MB of textures and
models). Delete that folder if you do not want it in your project.

## 2. Project settings

Nothing is required. The strands are alpha-blended over a depth pre-pass, which gives soft edges
with or without multisampling. Two settings in *Project Settings > Rendering > Anti Aliasing* are
still worth knowing:

- **Use TAA** is recommended. Without it, strands thinner than a pixel flicker from frame to
  frame.
- **MSAA 3D** is optional. 4x softens the strand edges a little more and costs GPU time across
  the whole scene; leave it off if your game does not use it.

## 3. Your first hair

1. Add a `TressFXHair` node anywhere in the scene.
2. Set **tfx_path** to a `.tfx` file. The hair appears in its rest pose and starts simulating.
   Try `addons/tressfx/demo/ponytail/ponytail_hair_head.tfx` with **import_scale** `0.01`
   (that asset is in centimetres).
3. Set **num_follow_hairs** to 2 or 3. Follow strands are cheap copies of the guide strands
   and give the hair its volume.
4. Create a material: in **material** choose *New ShaderMaterial*, then in its **shader** slot
   *Load* `addons/tressfx/shaders/tressfx_strand.gdshader`. The shader parameters that appear
   are the look: `base_color`, `tip_color`, `fiber_radius` (metres; 0.001 to 0.002 for hair),
   `k_diffuse`, the two specular terms, `root_shadow`, `color_variation`.

Use one material per hair node. The node writes its simulation texture and the vertex count into
the material's parameters, so two hair nodes cannot share one.

## 4. Attaching the hair to a character

Give the node a **hair_skeleton** (the character's `Skeleton3D`) and a **tfxbone_path** (the
`.tfxbone` file exported together with the `.tfx`). Each guide strand is then skinned to up to
four bones by name, exactly like the character's mesh, and follows the animation.

Where the hair node sits in the tree does not matter in this case: the simulation runs in world
space using the skeleton's bone transforms. Without a skeleton the hair follows the node's own
transform, which is what a bust or a helmet on a prop needs.

## 5. Keeping the hair out of the body

1. Add a `TressFXCollisionMesh` as a child of whatever moves with the body part (for a bust,
   the bust node; for a skinned character, anywhere, plus a **hair_skeleton** and a
   **follow_bone**).
2. Give it a **mesh**. A `CapsuleMesh` or `SphereMesh` is the best start; a low-poly body proxy
   works too; a skinned mesh uses its bone weights. Or point **tfxmesh_path** at a `.tfxmesh`.
3. Position and size it so it sits just inside the visible surface. The editor draws every
   collider as an orange shape: parts that stick out of the body show through, and selecting the
   collider (or a hair that lists it) shows the whole shape on top of the body. The
   **Hair Colliders** button in the 3D editor's toolbar hides and shows the shapes.
4. Add it to the hair's **collision_meshes** list.

The collider is turned into a signed distance field on the GPU and rebuilt whenever it moves.
**num_cells_x** (cells across the mesh) controls precision: 32 for a hand, 48 to 80 for a head
or body. The demos use a capsule for the head, a sphere for the back of the skull, and capsules
for neck and torso.

Two rules save a lot of tuning: the hair's rest pose must lie *outside* the colliders, and
colliders should be closed meshes. See [Tuning](Tuning.md) for why.

## 6. Seeing what it costs

Add a `CanvasLayer` with a `TressFXStats` node under it. It shows the frame rate, the GPU time of
the simulation and of the distance field builds, the main-thread cost of the TressFX nodes, and
the strand and triangle counts. Turn it off in shipped builds; it is a debugging overlay.

## 7. Working in the editor

The hair simulates in the editor exactly as it does in the game. Move the skeleton, scrub an
animation in the Animation panel, or drag the bust, and the hair reacts. Every parameter in the
Inspector applies immediately; the ones that rebuild the asset (`tfx_path`, `tfxbone_path`,
`hair_skeleton`, `num_follow_hairs`, `follow_radius`, `import_scale`) take a few milliseconds.

When a `.tfx` or `.tfxbone` is written again (by the [Blender add-on](Blender.md), for
example), the node reloads it within a second or two.

After about five seconds without any change to the pose or the parameters the editor simulation
goes to sleep, so an open scene with hair does not keep the GPU busy. Any change wakes it up.

## 8. At runtime

- Call `reset_positions()` after teleporting a character. The next two frames snap the hair
  back to its rest pose; without it the hair would fly across the level and swing for a while.
- `wind_direction` and `wind_magnitude` can be animated from a script. The wind gusts on its own
  between half and one and a half times the magnitude.
- All simulation parameters can be changed at runtime, every frame if you like; they are
  uploaded with the frame's parameters.

```gdscript
@onready var hair: TressFXHair = $Hair

func teleport(to: Vector3) -> void:
	global_position = to
	hair.reset_positions()

func set_storm(strength: float) -> void:
	hair.wind_direction = Vector3(1, 0, 0.3)
	hair.wind_magnitude = strength # 1 to 3 bends the demo fur visibly
```
