#[compute]
#version 450

// Port of upstream TressFXBoneSkinning.hlsl (pass 0) and the SDF build half of
// TressFXSDFCollision.hlsl (passes 1-3). Dispatched by TressFXCollisionMesh every frame:
//   0 BoneSkinning                  (per mesh vertex)
//   1 InitializeSignedDistanceField (per cell)
//   2 ConstructSignedDistanceField  (per triangle, atomic min into the cells around it)
//   3 FinalizeSignedDistanceField   (per cell, undo the atomic-friendly bit flip)
// The hair side (CollideHairVerticesWithSdf) is tressfx_sdf_collide.glsl.

#define THREAD_GROUP_SIZE 64
layout(local_size_x = THREAD_GROUP_SIZE) in;

#define INITIAL_DISTANCE 1e10
#define GRID_MARGIN ivec3(1, 1, 1)

struct Vertex {
	vec4 position;
	vec4 normal;
};

struct BoneSkinningData {
	vec4 bone_index;
	vec4 bone_weight;
};

layout(set = 0, binding = 0, std430) restrict readonly buffer SkinBuf { BoneSkinningData g_skinning[]; };
layout(set = 0, binding = 1, std430) restrict readonly buffer InitialBuf { Vertex g_initial[]; };
layout(set = 0, binding = 2, std430) restrict buffer MeshBuf { Vertex g_mesh[]; }; // skinned, world space
layout(set = 0, binding = 3, std430) restrict readonly buffer IndexBuf { uint g_indices[]; };
layout(set = 0, binding = 4, std430) restrict buffer SdfBuf { uint g_sdf[]; }; // floats stored bit-flipped
layout(set = 0, binding = 5, std430) restrict readonly buffer BoneBuf { mat4 g_bone[]; };
// 3 per triangle: the triangle across edge (0,1), (1,2), (2,0); -1 on a boundary.
layout(set = 0, binding = 7, std430) restrict readonly buffer AdjBuf { int g_adjacent[]; };

layout(set = 0, binding = 6, std140) uniform Params {
	vec4 origin;  // xyz grid origin (world)
	vec4 grid;    // x cell size, y collision margin (world units)
	ivec4 cells;  // xyz cells per axis, w triangle count
	ivec4 counts; // x vertex count
} p;

layout(push_constant, std430) uniform PC {
	int pass;
	int pad0;
	int pad1;
	int pad2;
} pc;

#define g_cell_size p.grid.x
#define g_margin p.grid.x // upstream MARGIN = cell size

// Float -> uint that orders like the float's magnitude under atomicMin (FloatFlip3).
uint float_flip(float f) {
	uint u = floatBitsToUint(f);
	return (u << 1) | (u >> 31);
}

uint float_unflip(uint u) {
	return (u >> 1) | (u << 31);
}

ivec3 sdf_coords(vec3 pos) {
	return ivec3((pos - p.origin.xyz) / g_cell_size);
}

vec3 sdf_cell_position(ivec3 c) {
	return vec3(c) * g_cell_size + p.origin.xyz;
}

int sdf_cell_index(ivec3 c) {
	return p.cells.x * p.cells.y * c.z + p.cells.x * c.y + c.x;
}

float distance_point_to_edge(vec3 pt, vec3 x0, vec3 x1, out vec3 n) {
	vec3 x10 = x1 - x0;
	float t = clamp(dot(x1 - pt, x10) / dot(x10, x10), 0.0, 1.0);
	vec3 a = pt - (t * x0 + (1.0 - t) * x1);
	float d = length(a);
	n = a / (d + 1e-30);
	return d;
}

float distance_point_to_edge2(vec3 pt, vec3 x0, vec3 x1, out vec3 point_on_edge) {
	vec3 x10 = x1 - x0;
	float t = clamp(dot(x1 - pt, x10) / dot(x10, x10), 0.0, 1.0);
	point_on_edge = t * x0 + (1.0 - t) * x1;
	return length(pt - point_on_edge);
}

// Upstream SignedDistancePointToTriangle2: when the nearest feature is a vertex or an edge,
// the sign comes from that feature's pseudo-normal instead of the face plane. Upstream ships
// this but builds its SDF with the plane-sign version, which flips sign in concave creases
// (neck, armpits) and makes the hair there jitter.
float signed_distance_point_to_triangle2(vec3 pt, vec3 x0, vec3 x1, vec3 x2, vec3 nf,
		vec3 vn0, vec3 vn1, vec3 vn2, vec3 en01, vec3 en12, vec3 en20) {
	vec3 x02 = x0 - x2;
	float l0 = length(x02) + 1e-30;
	x02 /= l0;
	vec3 x12 = x1 - x2;
	float l1 = dot(x12, x02);
	x12 -= l1 * x02;
	float l2 = length(x12) + 1e-30;
	x12 /= l2;
	vec3 px2 = pt - x2;

	float b = dot(x12, px2) / l2;
	float a = (dot(x02, px2) - l1 * b) / l0;
	float c = 1.0 - a - b;
	float tol = 1e-8;
	if (a >= -tol && b >= -tol && c >= -tol) {
		float d = length(pt - (a * x0 + b * x1 + c * x2));
		return dot(pt - x0, nf) < 0.0 ? -d : d;
	}

	vec3 normals[6] = vec3[6](vn0, vn1, vn2, en01, en12, en20);
	vec3 nearest[6];
	nearest[0] = x0;
	nearest[1] = x1;
	nearest[2] = x2;
	float dist[6];
	dist[0] = length(pt - x0);
	dist[1] = length(pt - x1);
	dist[2] = length(pt - x2);
	dist[3] = distance_point_to_edge2(pt, x0, x1, nearest[3]);
	dist[4] = distance_point_to_edge2(pt, x1, x2, nearest[4]);
	dist[5] = distance_point_to_edge2(pt, x0, x2, nearest[5]);
	int best = 0;
	for (int j = 1; j < 6; j++) {
		if (dist[j] < dist[best]) {
			best = j;
		}
	}
	return dot(pt - nearest[best], normals[best]) < 0.0 ? -dist[best] : dist[best];
}

// Face normal oriented like the mesh's vertex normals, so triangle winding (Godot meshes are
// clockwise, Maya exports counter-clockwise) can't flip the SDF sign.
vec3 face_normal(int tri) {
	uint i0 = g_indices[tri * 3];
	uint i1 = g_indices[tri * 3 + 1];
	uint i2 = g_indices[tri * 3 + 2];
	vec3 a = g_mesh[i0].position.xyz;
	vec3 n = normalize(cross(g_mesh[i1].position.xyz - a, g_mesh[i2].position.xyz - a));
	vec3 vn = g_mesh[i0].normal.xyz + g_mesh[i1].normal.xyz + g_mesh[i2].normal.xyz;
	return dot(n, vn) < 0.0 ? -n : n;
}

// Signed distance from pt to triangle (x0, x1, x2); positive on the side its normal points to.
float signed_distance_point_to_triangle(vec3 pt, vec3 x0, vec3 x1, vec3 x2) {
	vec3 x02 = x0 - x2;
	float l0 = length(x02) + 1e-30;
	x02 /= l0;
	vec3 x12 = x1 - x2;
	float l1 = dot(x12, x02);
	x12 -= l1 * x02;
	float l2 = length(x12) + 1e-30;
	x12 /= l2;
	vec3 px2 = pt - x2;

	float b = dot(x12, px2) / l2;
	float a = (dot(x02, px2) - l1 * b) / l0;
	float c = 1.0 - a - b;

	vec3 n_tri = cross(x1 - x0, x2 - x0);
	float d;
	float tol = 1e-8;
	if (a >= -tol && b >= -tol && c >= -tol) {
		d = length(pt - (a * x0 + b * x1 + c * x2));
	} else {
		vec3 n;
		d = distance_point_to_edge(pt, x0, x1, n);
		d = min(d, distance_point_to_edge(pt, x1, x2, n));
		d = min(d, distance_point_to_edge(pt, x0, x2, n));
	}
	return dot(pt - x0, n_tri) < 0.0 ? -d : d;
}

// ---------------------------------------------------------------- kernels

void bone_skinning(uint id) {
	if (id >= uint(p.counts.x)) {
		return;
	}
	BoneSkinningData sd = g_skinning[id];
	mat4 m = g_bone[int(sd.bone_index.x)] * sd.bone_weight.x;
	float weight_sum = sd.bone_weight.x;
	for (int i = 1; i < 4; i++) {
		if (sd.bone_weight[i] > 0.0) {
			m += g_bone[int(sd.bone_index[i])] * sd.bone_weight[i];
			weight_sum += sd.bone_weight[i];
		}
	}
	m /= weight_sum;
	g_mesh[id].position = vec4((m * vec4(g_initial[id].position.xyz, 1.0)).xyz, 1.0);
	g_mesh[id].normal = vec4((m * vec4(g_initial[id].normal.xyz, 0.0)).xyz, 0.0);
}

void initialize_sdf(uint id) {
	if (id >= uint(p.cells.x * p.cells.y * p.cells.z)) {
		return;
	}
	g_sdf[id] = float_flip(INITIAL_DISTANCE);
}

void construct_sdf(uint id) {
	if (id >= uint(p.cells.w)) {
		return;
	}
	uint i0 = g_indices[id * 3u];
	uint i1 = g_indices[id * 3u + 1u];
	uint i2 = g_indices[id * 3u + 2u];
	vec3 t0 = g_mesh[i0].position.xyz;
	vec3 t1 = g_mesh[i1].position.xyz;
	vec3 t2 = g_mesh[i2].position.xyz;

	// Pseudo-normals: skinned vertex normals, edge normals from the two adjacent faces.
	vec3 vn0 = normalize(g_mesh[i0].normal.xyz);
	vec3 vn1 = normalize(g_mesh[i1].normal.xyz);
	vec3 vn2 = normalize(g_mesh[i2].normal.xyz);
	vec3 nf = face_normal(int(id));
	int a01 = g_adjacent[id * 3u];
	int a12 = g_adjacent[id * 3u + 1u];
	int a20 = g_adjacent[id * 3u + 2u];
	vec3 en01 = a01 >= 0 ? normalize(nf + face_normal(a01)) : nf;
	vec3 en12 = a12 >= 0 ? normalize(nf + face_normal(a12)) : nf;
	vec3 en20 = a20 >= 0 ? normalize(nf + face_normal(a20)) : nf;

	vec3 aabb_min = min(t0, min(t1, t2)) - vec3(g_margin);
	vec3 aabb_max = max(t0, max(t1, t2)) + vec3(g_margin);
	ivec3 grid_min = clamp(sdf_coords(aabb_min) - GRID_MARGIN, ivec3(0), p.cells.xyz - 1);
	ivec3 grid_max = clamp(sdf_coords(aabb_max) + GRID_MARGIN, ivec3(0), p.cells.xyz - 1);

	for (int z = grid_min.z; z <= grid_max.z; z++) {
		for (int y = grid_min.y; y <= grid_max.y; y++) {
			for (int x = grid_min.x; x <= grid_max.x; x++) {
				ivec3 c = ivec3(x, y, z);
				float d = signed_distance_point_to_triangle2(sdf_cell_position(c), t0, t1, t2, nf,
						vn0, vn1, vn2, en01, en12, en20);
				atomicMin(g_sdf[sdf_cell_index(c)], float_flip(d));
			}
		}
	}
}

void finalize_sdf(uint id) {
	if (id >= uint(p.cells.x * p.cells.y * p.cells.z)) {
		return;
	}
	g_sdf[id] = float_unflip(g_sdf[id]);
}

void main() {
	uint id = gl_GlobalInvocationID.x;
	switch (pc.pass) {
		case 0: bone_skinning(id); break;
		case 1: initialize_sdf(id); break;
		case 2: construct_sdf(id); break;
		case 3: finalize_sdf(id); break;
	}
}
