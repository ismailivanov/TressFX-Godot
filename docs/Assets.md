# Assets

TressFX uses three file types. All three come from AMD's own tools, and this page also lists what
this addon adds to produce them.

## `.tfx`: the guide strands

A binary file (TressFX file format version 4) with a 160-byte header followed by the vertices:

- number of strands and vertices per strand;
- for every vertex a `float4`: x, y, z and an *inverse mass* in `w`. `w = 0` pins the vertex
  (the first two vertices of every strand are pinned to the scalp), `w = 1` lets it move;
- optionally one `float2` per strand, the texture coordinate on the body used to colour the
  strand from the body's albedo texture.

Rules the addon enforces on load:

- **Vertices per strand** must be 4, 8, 16, 32 or 64. 16 suits most hair, 8 is enough for short
  fur, 32 for long hair with curls.
- The strand count is padded up to a multiple of 64 (the compute thread group size); padding
  strands are copies of the last real strand and cost almost nothing.
- Positions are in **metres**. For an asset authored in centimetres set `import_scale` to
  `0.01` on the node; the positions are scaled on load, before the follow hairs are generated.
- At most 16 million vertices per node.

## `.tfxbone`: skinning weights

For every guide strand, four bone indices and four weights, plus the list of bone names. On load
each name is looked up in the node's `hair_skeleton` with `Skeleton3D.find_bone()`, so the names
in the file must match the names in the imported skeleton exactly. A missing bone prints a
warning and falls back to bone 0.

Without a `.tfxbone` (or without a skeleton) the whole hairstyle is attached to a single
transform: the node's own.

## `.tfxmesh`: a collision mesh

A text file with bone names, then vertices (position, normal, four bone indices, four weights),
then triangles. `TressFXCollisionMesh` reads it directly. You do not need this format: any Godot
`Mesh` works as a collider, and primitives (`CapsuleMesh`, `SphereMesh`) are usually better than
a real body mesh because they are closed and smooth.

## Producing the files

### Maya

AMD ships a Maya exporter with TressFX (`tool/Maya` in the TressFX repository). It writes all
three formats from a groom made of curves and a skinned mesh.

### Maya ASCII curves without Maya

`addons/tressfx/tools/ma_to_tfx.py` reads every `nurbsCurve` from a `.ma` file, resamples each
curve by arc length and writes a `.tfx` (no strand UVs, no bones):

```bash
python3 addons/tressfx/tools/ma_to_tfx.py hair.ma hair.tfx 16
python3 addons/tressfx/tools/ma_to_tfx.py hair.ma hair.tfx 16 --split-tail-z=-2.5
```

The third argument is the vertices per strand. `--split-tail-z` writes two files, `hair_head.tfx`
and `hair_tail.tfx`: strands whose tip lies below the given z go to the tail. That is how the
ponytail demo gets a stiff scalp part and a loose tail, each with its own node and settings.
Units are kept as they are in the file; set `import_scale` on the node.

### Blender

Two community add-ons export a Blender particle-hair system to `.tfx`:
[deathbravo/BlenderTressFxTfxExporter](https://github.com/deathbravo/BlenderTressFxTfxExporter)
and [treviasxk/BlenderExportTressFX](https://github.com/treviasxk/BlenderExportTressFX). Both are
unfinished and neither writes a `.tfxbone`; for a rigid head (a bust, a helmet) that is enough.

### Writing your own

The format is small enough to write from any tool, including a Godot script. The header is:

| Offset | Type | Field |
|---|---|---|
| 0 | float | version, write `4.0` |
| 4 | uint32 | number of strands |
| 8 | uint32 | vertices per strand |
| 12 | uint32 | offset of the vertex positions, `160` |
| 16 | uint32 | offset of the strand UVs, `0` if none |
| 20 | 3 x uint32 | reserved, `0` |
| 32 | 32 x uint32 | reserved, `0` |

followed at offset 160 by `strands * vertices_per_strand` float4 values. Make the first two
vertices of every strand `w = 0` and the rest `w = 1`.

## One hairstyle, several parts

TressFX 4 has no per-strand parameter groups: every strand in a node shares the same stiffness,
damping and gravity. A hairstyle whose parts must behave differently (a stiff scalp and a loose
tail, bangs and a mane) is split into several `.tfx` files, one `TressFXHair` node each. All of
them can share the same colliders and the same material settings.
