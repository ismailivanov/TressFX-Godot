# Tuning

## What the parameters do

TressFX is a position-based simulation: every frame the vertices are moved by gravity, wind and
inertia, and then a series of constraints pulls them back towards something sensible. The
constraints do not know about forces, they just move points, which is why a strong force and a
strong constraint can fight forever. Keep that picture in mind and the parameters make sense.

- **damping** removes velocity every frame. The AMD sample uses 0.068, which leaves fur
  twitching; 0.2 brings it fully to rest. Raise it first when something will not settle.
- **gravity** is an acceleration. It is the force the constraints fight, so it wants to be small:
  0.09 for fur, around 1 for hair that must hang. A strong gravity with stiff constraints jitters.
- **global_stiffness** over **global_range** pulls the first part of every strand (a fraction from
  the root) back to its authored rest shape. This is what keeps a parting, bangs, and the volume
  of a hairstyle. Too much and the hair is a helmet.
- **local_stiffness** keeps each segment's bend relative to the previous one: curls and waves.
  Values near 1 are unstable, the shader caps it at 0.95. **local_iterations** applies it more
  than once per frame.
- **length_iterations** keeps every segment at its rest length; 2 or 3 iterations is plenty.
- **vsp_coeff** (velocity shock propagation) copies the rigid motion of the root onto the whole
  strand. It is what makes hair turn with a head instead of lagging behind and stretching. High
  values (0.76) make the hair move as a block; low values (0.25) let a tail swing.
  **vsp_accel_threshold** forces the coefficient to 1 when the root accelerates violently, so a
  fast head turn cannot pull the hair apart.
- **wind_magnitude** is an acceleration like gravity, blowing towards **wind_direction**, gusting
  on its own. 1 to 3 bends the demo fur visibly. The wind acts on the part of the strand
  perpendicular to it, so hair aligned with the wind is left alone.
- **clamp_position_delta** limits how far a vertex may move per frame. A safety net for very long
  frames; leave it.

## Recipes

**Short fur** (the RatBoy demo, 8 vertices per strand): the AMD sample values with `damping` 0.2;
colliders at `num_cells_x` 80 with `push_limit` 0.25.

**Long straight hair** (16 or 32 vertices): `local_stiffness` 0.3, `global_stiffness` 0.2 over
`global_range` 0.5 so the parting survives, `gravity` 1, `damping` 0.2, `length_iterations` 3.

**Ponytail** (the ponytail demo, two nodes): the scalp part uses `local_stiffness` 0.4,
`global_stiffness` 0.3 over `global_range` 0.4, `gravity` 1, `damping` 0.2. The tail must not be
tied to the head: `global_stiffness` 0.06 over `global_range` 0.2 (roots only), `vsp_coeff` 0.25,
`local_stiffness` 0.35, `damping` 0.1, `gravity` 1.5.

## Colliders

**The rest pose must lie outside the colliders.** A strand whose root sits inside the collider is
pushed out at its base every frame; the local shape constraint keeps the relative bends, so the
whole strand swings outwards and the hair ends up standing on end. Two ways out: shrink the
collider so it sits just inside the visible skin, or raise `collision_skip_root_vertices` (the
long-hair recipe uses 5) so the first vertices are never collided. Do not use `collision_margin`
for this; it makes it worse.

**Colliders should be closed.** The distance field only knows inside from outside where the mesh
has an inside. An open mesh (a head with a hole at the neck, a shirt with no bottom) makes hair
kink along the open edges. Primitives are closed by construction.

**The field is only built near the surface.** Hair that ends up deep inside a collider is not
pushed; it is invisible to the field. That is what `push_limit` is for: a vertex a few cells deep
emerges over a few frames instead of teleporting to the surface and being pulled straight back by
the shape constraints, which shows as a two-state flicker. 0.25 cells per frame for a body, 1 for
small colliders.

**num_cells_x** trades memory and build time for precision. On the RatBoy body going from 50 to
80 cut resting jitter threefold. The grid is padded by 80% per side, so 80 cells across the mesh
is 208 cells per axis, 9 million cells, 36 MB. 128 is the maximum.

## Jitter

Vertices flickering back and forth at rest are the usual TressFX complaint. Where it comes from,
in order of likelihood:

1. Gravity too strong for the constraints. Lower `gravity` or raise `damping`.
2. Roots inside a collider (see above).
3. A vertex at the inner edge of the distance field, pushed out and pulled back every frame.
   Lower `push_limit`, raise `num_cells_x`.
4. Open colliders.

The demos have a `--jitter-check` flag that prints the share of vertices that still accelerate by
more than a millimetre per frame at rest; run it after each change:

```bash
godot --path . addons/tressfx/demo/ponytail.tscn -- --jitter-check --still
```

Some jitter is inherent: a few percent of vertices in a ponytail hanging over a collider is normal
and invisible with TAA on.

## LOD

`lod_enabled` deals the strands into four groups that fade out one after another between
`lod_start_distance` and `lod_end_distance`, while the shader widens the remaining fibers by up
to `lod_width_multiplier` so the hair does not thin out. `lod_percent` 0.25 keeps a quarter of
the strands at distance. The fade uses Godot's visibility ranges, so it is dithered and cheap.
