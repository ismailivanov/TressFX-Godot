# Performance

## What costs what

Almost everything is GPU time; the main thread spends a few microseconds per node packing bone
matrices and recording the compute passes.

| Part | Depends on | Scales with resolution |
|---|---|---|
| Simulation | guide strands x vertices per strand, iterations | no |
| Distance field build | collider triangles, `num_cells_x`^3, only when the collider moved | no |
| Collision | guide vertices x number of colliders | no |
| Drawing | total strands (guides + follows) x vertices, screen coverage, shadows | yes |

Only the guide strands are simulated. Follow strands are offset copies, so `num_follow_hairs`
multiplies the drawing cost, not the simulation cost.

## Measured

RTX 4060 Ti, 1152x648, MSAA 4x, TAA, vsync off (`godot --disable-vsync -- --bench`):

| Scene | Strands | ms/frame |
|---|---|---|
| RatBoy: fur + mohawk, 3 skinned colliders, 10 M cells, animated | 82,000 | 4.07 |
| Ponytail: 2 nodes, 4 primitive colliders, 5.5 M cells | 20,000 | 1.78 |

Breakdown on RatBoy: simulation 1.15 ms, distance field builds 0.92 ms, viewport draw 2.1 ms of
which the hair is most, TressFX main-thread 0.05 ms. A single character with a 20,000-strand
hairstyle costs about 1 to 2 ms on a mid-range GPU at 1080p.

Loading: the 75,000-strand fur takes 40 ms, the 13,000-vertex body collider 13 ms.

## Triangles

Every hair vertex becomes two ribbon vertices, so a strand of `n` vertices is `2 (n - 1)`
triangles. The ponytail demo draws 20,224 strands of 16 vertices: 606,720 triangles. That is in
line with how TressFX was shipped in games (the 2013 Tomb Raider used about 20,000 strands).

Shadows multiply it: with `cast_hair_shadows` on, a directional light renders the hair again into
every shadow cascade, and the same scene draws 2.5 million triangles per frame instead of 0.67.
Keep hair shadows off unless the shot needs them, and if you turn them on, reduce the light's
shadow cascades or use a shadow-casting proxy.

## Reducing the cost

- **Fewer vertices per strand.** 8 for short fur, 16 for most hair. This halves both simulation
  and drawing.
- **Fewer follow hairs.** Try 2 or 3 with a slightly larger `fiber_radius` before going to 4.
- **LOD.** `lod_enabled` drops three quarters of the strands a few metres away with no visible
  change.
- **Coarser colliders where it does not show.** `num_cells_x` 32 for hands, 48 for a head. The
  build cost is per cell (cubic) and per triangle.
- **Static colliders are free.** A collider that does not move is not rebuilt, and neither is
  one that no hair used in the previous frame.
- **Unseen hair is free.** By default a hair stops simulating while it is hidden or outside the
  camera's view; `simulation_distance` also stops it beyond a distance. Both are per node, see
  [Nodes](Nodes.md). A crowd of characters therefore only pays for the ones on screen.
- **MSAA.** The hair needs MSAA 4x to look right, and MSAA is a cost on the whole scene. If the
  game does not already use it, that is the largest single cost the hair adds.

## Memory

Per hair node, roughly: the ribbon vertex buffer (12 bytes per ribbon vertex, two per hair
vertex; the ribbons carry no attributes, everything is derived from the vertex index), the index
buffer (24 bytes per hair vertex), the positions texture (16 bytes per hair vertex) and the
simulation buffers (about 80 bytes per hair vertex). The RatBoy fur with 604,000 vertices takes
about 85 MB of GPU memory; a typical 20,000-strand hairstyle of 16 vertices takes about 45 MB. Colliders take 4 bytes per
distance field cell (36 MB at `num_cells_x` 80).
