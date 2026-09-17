# Hair from Blender

This page takes a hairstyle from Blender to Godot: the hair is groomed with Blender's hair tools,
the character goes to Godot as a glTF file, and the hair as a `.tfx` (the strands) plus a
`.tfxbone` (which bones each strand follows), plus `.tfxmesh` files for the collision shapes. The exporter, *Blender to Godot Hair*, ships with
the addon as `addons/tressfx/tools/tressfx_blender_export.py`.

Requirements: Blender 4.2 or later (tested with 5.2) and Blender's own hair system, the
*Curves* object. Older particle hair can be converted, see step 2.

A finished example ships with the addon: `addons/tressfx/demo/blender_hair/source/blender_hair.blend`
holds a head with a spiked mohawk (one hair object) and two collider meshes (the head, the neck
and shoulders), and `addons/tressfx/demo/blender_hair.tscn` is the same character in Godot. Open the `.blend`, change
the hair, press **Export to Godot** and the demo picks up the new files.

## 1. Install the exporter

1. In Blender open *Edit > Preferences > Add-ons*.
2. Open the drop-down menu at the top right, choose *Install from Disk* and pick
   `tressfx_blender_export.py`.
3. Tick *Blender to Godot Hair* if it is not ticked already.

The add-on also has its own repository,
[Blender to Godot Hair](https://github.com/ismailivanov/Blender-to-Godot-Hair), with release zips
that install as a Blender extension.

The 3D Viewport's sidebar (press *N*) now has a **TressFX** tab, and *File > Export* has a
*TressFX Hair (.tfx)* entry. To try the add-on without installing it, open the file in Blender's
Text Editor and press *Run Script*; it stays until Blender is closed.

The TressFX tab follows the whole workflow in three boxes: **1. Hair** shows what is selected and
the next step (add hair, sculpt, pick the surface), **2. Colliders** marks the meshes the hair
must stay out of, **3. Export to Godot** writes everything.

## 2. Groom the hair

Start from a character that is ready for Godot: a head mesh with a UV map and, if it is
animated, either an Armature modifier with vertex groups (a skinned character) or a *Bone*
parent (a character made of rigid parts). Blender works in metres, so does Godot; keep the
scene's *Unit Scale* at 1.

1. Select the head mesh and press **Add Hair to ...** in the TressFX tab (or choose
   *Add > Curves > Empty Hair*). Blender creates a hair object whose **surface** is the head,
   switches to Sculpt Mode and picks the Add brush. The exporter reads UVs and bone weights
   from the surface; the tab shows it under *Surface*.
2. Sculpt the hair (the tab's **Sculpt Hair** button enters Sculpt Mode). The main brushes:
   - **Add** plants new curves where you click on the scalp. Set *Curve Length* and *Count* in
     the tool header before painting. **Density** adds or removes curves to reach a spacing.
   - **Comb**, **Snake Hook** and **Puff** shape the hair; **Smooth** removes kinks.
   - **Grow / Shrink** changes the length, **Delete** removes curves.
   - Turn on the collision toggle (the physics icon in the header) so combed hair stays above
     the scalp.
3. Keep an eye on the curve count (*Overlays > Statistics*). Every curve becomes a
   *guide strand*, and `TressFXHair` adds follow strands around each guide in Godot
   (`num_follow_hairs`). A few thousand guides with two or three follow strands each is a full
   head of hair. Short fur can use more guides with fewer vertices each.
4. Hair modifiers from the *Add Modifier > Hair* menu (*Clump Hair Curves*, *Curl Hair Curves*,
   *Frizz Hair Curves*, *Braid Hair Curves*, *Trim Hair Curves* and the rest) are baked into the
   export: the exporter reads the hair as you see it in the viewport. *Interpolate Hair Curves*
   and *Duplicate Hair Curves* work too, but they multiply the curve count; the follow strands
   do the same job for less.
5. Parts of a hairstyle that must move differently (bangs, a ponytail, a braid) belong in
   separate hair objects. Each one is exported to its own `.tfx` and becomes its own
   `TressFXHair` node with its own stiffness and gravity. To split a hair object, select the
   curves in Edit Mode and press *P* (*Separate*).

**Particle hair from an older file.** Select the mesh and press **Convert Particle Hair** in the
TressFX tab. It converts the particle systems to hair objects, sets their surface and UV map,
and hides the particle hair. (Blender's own *Convert to Curves* in the particle system menu
leaves the surface empty; set it in the tab's *Surface* field.)

## 3. Mark the colliders

The hair needs something to stay out of: the head, the neck, the shoulders. In Godot a sphere or
a capsule does the job (step 5), but you can also model the collision shape in Blender and
export it with the hair.

1. Model a **closed, low-poly** mesh that sits just inside the skin, a few hundred to a few
   thousand faces. A copy of the body with a *Decimate* modifier and *Shrink/Fatten* applied is a
   quick start. Leave out the parts the hair never reaches.
2. Skin it like the character (an Armature modifier with the same vertex groups, *Ctrl+P > With
   Automatic Weights*, or a *Data Transfer* of the weights), or parent it to a bone.
3. Select it and tick **... keeps hair out** in the **2. Colliders** box. The mesh turns into a
   wireframe and is no longer rendered or put into the `.glb`.

The hair's rest pose must start **outside** the colliders: keep them a few millimetres under the
scalp.

## 4. Export

In the **3. Export to Godot** box:

1. **Folder**: a folder inside your Godot project. `//` means the folder of the `.blend` file.
2. **Vertices**: 8 for short fur, 16 for most hair, 32 for long or curly hair. Each curve is
   resampled to this many evenly spaced points.
3. **Bone Weights**: writes a `.tfxbone` next to each `.tfx` when the head is moved by an
   armature.
4. **Colliders**: writes every marked collider as `<object name>.tfxmesh`.
5. **Character**: also writes the scene as a `.glb`, named after the `.blend` unless you type a
   name. Colliders and other objects disabled for rendering are left out.
6. Press **Export to Godot**. Every visible hair object is written as `<object name>.tfx`; the
   box lists the files and the result of the last export.

To export the character yourself, use *File > Export > glTF 2.0*, keep *+Y Up* and turn on
**Apply Modifiers** (*Mesh* section). The hair was placed on the head as Blender shows it, with
its Subdivision and other modifiers; without them the head in Godot has a different shape and
the hair floats. *File > Export > TressFX Hair (.tfx)* exports only the active hair object.

What the exporter writes:

- The strands in the character's **rest pose**. The armature is switched to *Rest Position* for
  the export and back afterwards, so you can export in the middle of an animation.
- Positions in Blender world space turned Y-up, the same space the glTF exporter uses, so hair
  and character line up in Godot without any offset.
- A **root UV** per strand, from the surface's UV map. The strand shader uses it to tint each
  strand with the colour of the skin under it (`base_albedo`).
- For each strand, the **bone weights** of the surface at the root, blended from the three
  nearest vertices and reduced to the four strongest bones. A root on vertices without any
  bone weight is bound to the nearest bone and reported in a warning. When the head hangs
  from a bone (a *Bone* parent, no Armature modifier), every strand follows that bone.
- Bone names as Godot's importer writes them: `:` and `/` become `_`, so Mixamo's
  `mixamorig:Head` is written as `mixamorig_Head`.
- For each collider, its triangles and vertices in the same space and rest pose, each vertex with
  the four strongest bones of its vertex groups (or the parent bone).

The status bar shows the strand and bone count, or a warning when something is missing.

## 5. Set it up in Godot

1. Add the imported character to your scene.
2. Add a `TressFXHair` node. Set **tfx_path** to the `.tfx`. Leave **import_scale** at 1.
3. **Animated character**: set **tfxbone_path** to the `.tfxbone` and **hair_skeleton** to the
   character's `Skeleton3D`. If the node picker does not list the skeleton, right-click the
   character in the Scene dock and enable *Editable Children*. The hair node can sit anywhere
   in the tree.
   **Static character** (a bust, a statue): leave both empty and make the hair node a child of
   the character with no offset of its own. The hair then follows the character's transform.
4. Give the node a material, as in [Getting Started](Getting-Started.md): a `ShaderMaterial`
   with `addons/tressfx/shaders/tressfx_strand.gdshader`. Put the character's albedo texture in
   `base_albedo` to colour the roots like the skin under them.
5. Set **num_follow_hairs** to 2 or 3.
6. Add the colliders and list them in the hair's **collision_meshes**:
   - **From Blender**: a `TressFXCollisionMesh` per `.tfxmesh`, with **tfxmesh_path** set. For
     an animated character also set its **hair_skeleton** (it can sit anywhere); for a static
     one make it a child of the character with no offset, like the hair.
   - **Primitives**: add a `BoneAttachment3D` next to the character, turn on *Use External
     Skeleton*, pick the skeleton and the head bone, and put a `TressFXCollisionMesh` with a
     `SphereMesh` under it, moved to the centre of the head. See
     [Getting Started](Getting-Started.md#5-keeping-the-hair-out-of-the-body).

   The editor draws the colliders in orange; select the hair to see all of its colliders on top
   of the body. The **Hair Colliders** button in the 3D toolbar hides and shows them.

Then press play, or scrub an animation in the editor, and tune the look and the motion with
[Tuning](Tuning.md).

Keep both programs open while you work: after **Export to Godot**, the Godot editor reimports the
`.glb`, every `TressFXHair` reloads its `.tfx` and `.tfxbone` and every `TressFXCollisionMesh`
its `.tfxmesh` within a second or two. A new hair object or collider needs its own node.

## Troubleshooting

**The hair floats next to the character or is rotated.** The character was exported without
*Apply Modifiers* or with *+Y Up* turned off, or it was moved in Blender after the hair was
exported. Export both again with the **Export to Godot** button. For a static character, check
that the hair node has no offset relative to the character.

**Output: "bone 'X' from hair.tfxbone not found in the skeleton".** A bone was renamed after
the hair export, or the character comes from another file. Export the hair again from the file
the character was exported from.

**The hair does not follow the head.** `hair_skeleton` or `tfxbone_path` is empty, or the
export warned that there was no surface mesh: set *Object Data Properties > Surface* on the hair
object and export again. Check that the surface mesh has vertex groups named after the bones.

**The hair stands out from the head at the start.** The colliders are larger than the head; see
[Troubleshooting](Troubleshooting.md).

**Export says "has no curves".** The hair object is empty: plant hair with the Add brush first.

**File > Export says "Select a hair object".** The active object is not a hair (Curves) object.
Click the hair in the viewport or the Outliner first, or use the TressFX tab, which exports all
hair objects at once. For particle hair, convert it as described in step 2.

**The hair stands out or jumps at the start.** Its rest pose is inside a collider. Select the
hair in Godot: the orange shapes that cover strands are too big. Shrink the collider in Blender
and export again.
