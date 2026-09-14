#[compute]
#version 450

// Port of upstream CollideHairVerticesWithSdf (TressFXSDFCollision.hlsl): one thread per
// guide hair vertex, pushes vertices inside the collision margin out along the SDF gradient.
// Dispatched by TressFXHair after the simulation, once per collision mesh, before the follow
// hairs are regenerated from the guides. (Upstream collides follow vertices too, but they are
// rebuilt from the guide every frame anyway, and at the edge of the SDF's narrow band that
// alternates between pushed and not pushed - visible flicker.)
// Set 0 belongs to the collision mesh, set 1 to the hair.

#define THREAD_GROUP_SIZE 64
layout(local_size_x = THREAD_GROUP_SIZE) in;

#define INITIAL_DISTANCE 1e10

layout(set = 0, binding = 0, std430) restrict readonly buffer SdfBuf { uint g_sdf[]; }; // float bits
layout(set = 0, binding = 1, std140) uniform Params {
	vec4 origin;
	vec4 grid;    // x cell size, y collision margin, z max push per frame (world units)
	ivec4 cells;
	ivec4 counts;
} p;

layout(set = 1, binding = 0, std430) restrict buffer PosBuf { vec4 g_pos[]; };
layout(set = 1, binding = 1, std430) restrict buffer PrevBuf { vec4 g_prev[]; };

layout(push_constant, std430) uniform PC {
	int num_guide_vertices;
	int verts_per_strand;
	int follow_per_guide;
	int skip_root_vertices;
} pc;

#define g_cell_size p.grid.x
#define g_collision_margin p.grid.y

float trilinear(float d[8], vec3 t) {
	float a = mix(mix(d[0], d[1], t.x), mix(d[2], d[3], t.x), t.y);
	float b = mix(mix(d[4], d[5], t.x), mix(d[6], d[7], t.x), t.y);
	return mix(a, b, t.z);
}

// The 8 cell corners around pos and the position inside the cell; false outside the grid
// or where the SDF was never written (it is only built near the surface).
bool gather(vec3 pos, out float d[8], out vec3 t) {
	ivec3 c = ivec3((pos - p.origin.xyz) / g_cell_size);
	if (any(lessThan(c, ivec3(0))) || any(greaterThanEqual(c, p.cells.xyz - 2))) {
		return false;
	}
	int base = p.cells.x * p.cells.y * c.z + p.cells.x * c.y + c.x;
	int sy = p.cells.x;
	int sz = p.cells.x * p.cells.y;
	int idx[8] = int[8](base, base + 1, base + sy, base + sy + 1,
			base + sz, base + sz + 1, base + sz + sy, base + sz + sy + 1);
	for (int j = 0; j < 8; j++) {
		d[j] = uintBitsToFloat(g_sdf[idx[j]]);
		if (d[j] == INITIAL_DISTANCE) {
			return false;
		}
	}
	t = (pos - (vec3(c) * g_cell_size + p.origin.xyz)) / g_cell_size;
	return true;
}

void main() {
	uint gid = gl_GlobalInvocationID.x;
	if (gid >= uint(pc.num_guide_vertices)) {
		return;
	}
	// Guide vertex index in the interleaved guide/follow layout.
	uint n = uint(pc.verts_per_strand);
	if (gid % n < uint(pc.skip_root_vertices)) {
		return;
	}
	uint id = (gid / n) * uint(pc.follow_per_guide + 1) * n + gid % n;
	vec4 v = g_pos[id];

	float d[8];
	vec3 t;
	if (!gather(v.xyz, d, t)) {
		return;
	}
	float dist = trilinear(d, t);
	if (dist >= g_collision_margin) {
		return;
	}

	// Gradient by finite differences inside this cell only (forward, or backward when the
	// probe would cross the cell boundary).
	float h = 0.1 * g_cell_size;
	vec3 dir = vec3(t.x + h < 1.0 ? 1.0 : -1.0, t.y + h < 1.0 ? 1.0 : -1.0, t.z + h < 1.0 ? 1.0 : -1.0);
	vec3 nd = vec3(
		trilinear(d, t + vec3(h * dir.x, 0.0, 0.0)),
		trilinear(d, t + vec3(0.0, h * dir.y, 0.0)),
		trilinear(d, t + vec3(0.0, 0.0, h * dir.z)));
	vec3 normal = normalize(dir * (nd - vec3(dist)) / h);

	// Upstream projects all the way out in one frame. At the inner edge of the SDF's narrow
	// band that teleports a vertex several cells and the shape constraints pull it straight
	// back: a two-state flicker. Limiting the push per frame lets deep vertices emerge over a
	// few frames instead.
	float push = min(g_collision_margin - dist, p.grid.z);
	vec3 projected = v.xyz + normal * push;
	g_pos[id].xyz = projected;
	g_prev[id].xyz = projected;
}
