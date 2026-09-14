#[compute]
#version 450

// Port of upstream TressFXSimulation.hlsl. All kernels live in one file and are picked by
// the push constant `pass`; the CPU dispatches them in upstream order:
//   0 IntegrationAndGlobalShapeConstraints (per vertex)
//   1 CalculateStrandLevelData             (per strand)
//   2 VelocityShockPropagation             (per vertex)
//   3 LocalShapeConstraints                (per strand, dispatched local_iterations times)
//   4 LengthConstraintsWindAndCollision    (per vertex)
//   5 UpdateFollowHairVertices             (per vertex)
//   6 write-out to the render texture      (per vertex, guide + follow, after SDF collision)
// Only guide strands are simulated; follow strands are offset copies (pass 5).
// Capsule collision is compiled out upstream too; SDF collision is tressfx_sdf_collide.glsl.

#define THREAD_GROUP_SIZE 64
layout(local_size_x = THREAD_GROUP_SIZE) in;

struct StrandLevelData {
	vec4 skinning_quat;
	vec4 vsp_quat;
	vec4 vsp_translation; // w = vsp coefficient
};

struct BoneSkinningData {
	vec4 bone_index;
	vec4 bone_weight;
};

layout(set = 0, binding = 0, std430) restrict buffer PosBuf { vec4 g_pos[]; };
layout(set = 0, binding = 1, std430) restrict buffer PrevBuf { vec4 g_prev[]; };
layout(set = 0, binding = 2, std430) restrict buffer PrevPrevBuf { vec4 g_prev_prev[]; };
layout(set = 0, binding = 3, std430) restrict buffer StrandBuf { StrandLevelData g_strand[]; };
layout(set = 0, binding = 4, std430) restrict readonly buffer InitialBuf { vec4 g_initial[]; };
layout(set = 0, binding = 5, std430) restrict readonly buffer RestLenBuf { float g_rest_length[]; };
layout(set = 0, binding = 6, std430) restrict readonly buffer FollowBuf { vec4 g_follow_offset[]; };
layout(set = 0, binding = 7, std430) restrict readonly buffer SkinBuf { BoneSkinningData g_skinning[]; };
layout(set = 0, binding = 8, std430) restrict readonly buffer BoneBuf { mat4 g_bone[]; }; // world space

layout(set = 0, binding = 9, std140) uniform Params {
	vec4 wind0;
	vec4 wind1;
	vec4 wind2;
	vec4 wind3;
	vec4 shape;         // damping, local stiffness, global stiffness, global range
	vec4 grav_time_tip; // gravity magnitude, time step, tip separation
	ivec4 sim_ints;     // length iterations, local iterations
	ivec4 counts;       // strands per thread group, follow hairs per guide, verts per strand, total vertices
	vec4 vsp;           // coefficient, accel threshold
	vec4 misc;          // reset positions, clamp position delta
} p;

// Final positions for rendering, one texel per vertex, wrapped by image width.
layout(set = 0, binding = 10, rgba32f) restrict writeonly uniform image2D out_pos;

layout(push_constant, std430) uniform PC {
	int pass;
	int pad0;
	int pad1;
	int pad2;
} pc;

shared vec4 shared_pos[THREAD_GROUP_SIZE];
shared float shared_len[THREAD_GROUP_SIZE];

#define g_damping p.shape.x
#define g_local_stiffness p.shape.y
#define g_global_stiffness p.shape.z
#define g_global_range p.shape.w
#define g_gravity p.grav_time_tip.x
#define g_time_step p.grav_time_tip.y
#define g_tip_separation p.grav_time_tip.z
#define g_length_iterations p.sim_ints.x
#define g_strands_per_group uint(p.counts.x)
#define g_follow_per_guide uint(p.counts.y)
#define g_verts_per_strand uint(p.counts.z)

// ---------------------------------------------------------------- helpers

bool is_movable(vec4 particle) {
	return particle.w > 0.0;
}

vec2 constraint_multiplier(vec4 p0, vec4 p1) {
	if (is_movable(p0)) {
		return is_movable(p1) ? vec2(0.5, 0.5) : vec2(1.0, 0.0);
	}
	return is_movable(p1) ? vec2(0.0, 1.0) : vec2(0.0, 0.0);
}

// Quaternion from a rigid transform. Indexing copied from upstream; with GLSL's
// column-major m[col][row] it yields the same result as upstream's row-vector convention.
vec4 quat_from_matrix(mat4 m) {
	vec4 q;
	float trace = m[0][0] + m[1][1] + m[2][2];
	if (trace > 0.0) {
		float r = sqrt(trace + 1.0);
		q.w = 0.5 * r;
		r = 0.5 / r;
		q.x = (m[1][2] - m[2][1]) * r;
		q.y = (m[2][0] - m[0][2]) * r;
		q.z = (m[0][1] - m[1][0]) * r;
	} else {
		int i = 0, j = 1, k = 2;
		if (m[1][1] > m[0][0]) { i = 1; j = 2; k = 0; }
		if (m[2][2] > m[i][i]) { i = 2; j = 0; k = 1; }
		float r = sqrt(m[i][i] - m[j][j] - m[k][k] + 1.0);
		float qq[3];
		qq[i] = 0.5 * r;
		r = 0.5 / r;
		q.w = (m[j][k] - m[k][j]) * r;
		qq[j] = (m[j][i] + m[i][j]) * r;
		qq[k] = (m[k][i] + m[i][k]) * r;
		q.xyz = vec3(qq[0], qq[1], qq[2]);
	}
	return q;
}

vec4 inverse_quat(vec4 q) {
	float len_sq = dot(q, q);
	if (len_sq < 0.001) {
		return vec4(0.0, 0.0, 0.0, 1.0);
	}
	return vec4(-q.xyz, q.w) / len_sq;
}

vec3 rotate_by_quat(vec4 q, vec3 v) {
	vec3 uv = cross(q.xyz, v);
	vec3 uuv = cross(q.xyz, uv);
	return v + uv * (2.0 * q.w) + uuv * 2.0;
}

vec4 normalize_quat(vec4 q) {
	float n = dot(q, q);
	if (n < 1e-10) {
		return vec4(q.xyz, 1.0);
	}
	return q / sqrt(n);
}

// Quaternion rotating unit vector u onto unit vector v.
vec4 quat_from_two_unit_vectors(vec3 u, vec3 v) {
	float r = 1.0 + dot(u, v);
	vec3 n;
	if (r < 1e-7) {
		r = 0.0;
		n = abs(u.x) > abs(u.z) ? vec3(-u.y, u.x, 0.0) : vec3(0.0, -u.z, u.y);
	} else {
		n = cross(u, v);
	}
	return normalize_quat(vec4(n, r));
}

void apply_distance_constraint(inout vec4 p0, inout vec4 p1, float target) {
	vec3 delta = p1.xyz - p0.xyz;
	float dist = max(length(delta), 1e-7);
	delta *= 1.0 - target / dist;
	vec2 m = constraint_multiplier(p0, p1);
	p0.xyz += m.x * delta;
	p1.xyz -= m.y * delta;
}

vec3 integrate(vec3 cur, vec3 old) {
	vec3 force = g_gravity * vec3(0.0, -1.0, 0.0);
	float decay = exp(-g_damping * g_time_step * 60.0);
	return cur + decay * (cur - old) + force * g_time_step * g_time_step;
}

vec3 apply_bone_skinning(vec3 v, BoneSkinningData sd, out vec4 bone_quat) {
	mat4 m = g_bone[int(sd.bone_index.x)] * sd.bone_weight.x;
	float weight_sum = sd.bone_weight.x;
	for (int i = 1; i < 4; i++) {
		if (sd.bone_weight[i] > 0.0) {
			m += g_bone[int(sd.bone_index[i])] * sd.bone_weight[i];
			weight_sum += sd.bone_weight[i];
		}
	}
	m /= weight_sum;
	bone_quat = quat_from_matrix(m);
	return (m * vec4(v, 1.0)).xyz;
}

// Upstream CalcIndicesInVertexLevelMaster: one thread per guide vertex.
void vertex_indices(uint lid, uint gid, out uint global_strand, out uint local_strand,
		out uint global_vertex, out uint local_vertex) {
	local_strand = lid % g_strands_per_group;
	global_strand = (gid * g_strands_per_group + local_strand) * (g_follow_per_guide + 1u);
	local_vertex = (lid - local_strand) / g_strands_per_group;
	global_vertex = global_strand * g_verts_per_strand + local_vertex;
}

// Upstream CalcIndicesInStrandLevelMaster: one thread per guide strand.
void strand_indices(uint lid, uint gid, out uint global_strand, out uint root_vertex) {
	global_strand = (THREAD_GROUP_SIZE * gid + lid) * (g_follow_per_guide + 1u);
	root_vertex = global_strand * g_verts_per_strand;
}

// Tangent packed as 12+12 bit octahedral into an exact float (< 2^24), so the vertex
// shader needs a single texel fetch per vertex. Decoded in tressfx_strand.gdshader.
float pack_tangent(vec3 t) {
	t /= abs(t.x) + abs(t.y) + abs(t.z);
	vec2 sgn = vec2(t.x >= 0.0 ? 1.0 : -1.0, t.y >= 0.0 ? 1.0 : -1.0);
	vec2 e = t.z >= 0.0 ? t.xy : (1.0 - abs(t.yx)) * sgn;
	vec2 o = floor(clamp(e * 0.5 + 0.5, 0.0, 1.0) * 4095.0);
	return o.x * 4096.0 + o.y;
}

void write_out(uint gv, vec3 pos, float packed_tangent) {
	int w = imageSize(out_pos).x;
	imageStore(out_pos, ivec2(int(gv) % w, int(gv) / w), vec4(pos, packed_tangent));
}

// ---------------------------------------------------------------- kernels

void integration_and_global_shape(uint lid, uint gid) {
	uint gs, ls, gv, lv;
	vertex_indices(lid, gid, gs, ls, gv, lv);

	vec4 initial = g_initial[gv];
	vec4 bone_quat;
	initial.xyz = apply_bone_skinning(initial.xyz, g_skinning[gs], bone_quat);

	vec4 current = g_pos[gv];
	vec4 old = g_prev[gv];
	if (p.misc.x != 0.0) { // teleported / first frames
		current = initial;
		old = initial;
	}

	vec4 np = is_movable(current) ? vec4(integrate(current.xyz, old.xyz), current.w) : initial;

	if (g_global_stiffness > 0.0 && g_global_range != 0.0 && is_movable(np)
			&& float(lv) < g_global_range * float(g_verts_per_strand)) {
		np.xyz += g_global_stiffness * (initial.xyz - np.xyz);
	}

	g_prev_prev[gv] = g_prev[gv];
	g_prev[gv] = current;
	g_pos[gv] = np;
}

void calculate_strand_level_data(uint lid, uint gid) {
	uint gs, root;
	strand_indices(lid, gid, gs, root);

	vec4 old_old1 = g_prev_prev[root + 1];
	vec4 old0 = g_prev[root];
	vec4 old1 = g_prev[root + 1];
	vec4 new0 = g_pos[root];
	vec4 new1 = g_pos[root + 1];

	// Rigid motion of the first two (pinned) vertices since last step.
	vec3 u = normalize(old1.xyz - old0.xyz);
	vec3 v = normalize(new1.xyz - new0.xyz);
	vec4 rot = quat_from_two_unit_vectors(u, v);
	vec3 trans = new0.xyz - rotate_by_quat(rot, old0.xyz);

	float vsp_coeff = p.vsp.x;
	float accel = length(new1 - 2.0 * old1 + old_old1);
	if (accel > p.vsp.y) {
		vsp_coeff = 1.0;
	}
	g_strand[gs].vsp_quat = rot;
	g_strand[gs].vsp_translation = vec4(trans, vsp_coeff);

	vec4 bone_quat;
	apply_bone_skinning(g_initial[root].xyz, g_skinning[gs], bone_quat);
	g_strand[gs].skinning_quat = bone_quat;
}

void velocity_shock_propagation(uint lid, uint gid) {
	uint gs, ls, gv, lv;
	vertex_indices(lid, gid, gs, ls, gv, lv);
	if (lv < 2u) {
		return;
	}
	vec4 q = g_strand[gs].vsp_quat;
	vec4 t = g_strand[gs].vsp_translation;
	float c = t.w;
	vec3 pn = g_pos[gv].xyz;
	vec3 po = g_prev[gv].xyz;
	g_pos[gv].xyz = (1.0 - c) * pn + c * (rotate_by_quat(q, pn) + t.xyz);
	g_prev[gv].xyz = (1.0 - c) * po + c * (rotate_by_quat(q, po) + t.xyz);
}

void local_shape_constraints(uint lid, uint gid) {
	uint gs, root;
	strand_indices(lid, gid, gs, root);

	// 1.0 makes things unstable sometimes (upstream).
	float stiffness = 0.5 * min(g_local_stiffness, 0.95);
	vec4 bone_quat = g_strand[gs].skinning_quat;

	for (uint lv = 1u; lv < g_verts_per_strand - 1u; lv++) {
		uint gv = root + lv;
		vec4 pos = g_pos[gv];
		vec4 pos_plus = g_pos[gv + 1];
		vec4 pos_minus = g_pos[gv - 1];

		vec3 bind = rotate_by_quat(bone_quat, g_initial[gv].xyz);
		vec3 bind_plus = rotate_by_quat(bone_quat, g_initial[gv + 1].xyz);
		vec3 bind_minus = rotate_by_quat(bone_quat, g_initial[gv - 1].xyz);

		vec3 last_vec = pos.xyz - pos_minus.xyz;
		vec3 vec_bind = bind_plus - bind;
		vec3 last_vec_bind = bind - bind_minus;
		vec4 rot_global = quat_from_two_unit_vectors(normalize(last_vec_bind), normalize(last_vec));

		vec3 target_plus = rotate_by_quat(rot_global, vec_bind) + pos.xyz;
		vec3 del = stiffness * (target_plus - pos_plus.xyz);
		if (is_movable(pos)) {
			pos.xyz -= del;
		}
		if (is_movable(pos_plus)) {
			pos_plus.xyz += del;
		}
		g_pos[gv].xyz = pos.xyz;
		g_pos[gv + 1].xyz = pos_plus.xyz;
	}
}

void length_constraints_wind_and_collision(uint lid, uint gid) {
	uint gs, ls, gv, lv;
	vertex_indices(lid, gid, gs, ls, gv, lv);
	uint spg = g_strands_per_group;
	uint n = g_verts_per_strand;

	shared_pos[lid] = g_pos[gv];
	shared_len[lid] = g_rest_length[gv];
	barrier();

	// Wind: four cone corners mixed per strand so neighbours differ a bit. The component of
	// the wind perpendicular to the strand acts as an acceleration (same units as gravity).
	// Upstream uses -cross(cross(v, w), v) with the unnormalised segment v: that scales with
	// segment length², vanishing in metre units, and points against the wind.
	if (any(notEqual(p.wind0.xyz, vec3(0.0)))) {
		if (lv >= 2u && lv < n - 1u) {
			float a = float(gs % 20u) / 20.0;
			vec3 w = a * p.wind0.xyz + (1.0 - a) * p.wind1.xyz + a * p.wind2.xyz + (1.0 - a) * p.wind3.xyz;
			vec3 v = normalize(shared_pos[lid].xyz - shared_pos[lid + spg].xyz);
			vec3 force = cross(cross(v, w), v);
			shared_pos[lid].xyz += force * g_time_step * g_time_step;
		}
	}
	barrier();

	// Length constraints: even edges then odd edges, Jacobi style across the group.
	uint a = n / 2u;
	uint b = (n - 1u) / 2u;
	for (int it = 0; it < g_length_iterations; it++) {
		uint si = 2u * lv * spg + ls;
		if (lv < a) {
			apply_distance_constraint(shared_pos[si], shared_pos[si + spg], shared_len[si]);
		}
		barrier();
		if (lv < b) {
			apply_distance_constraint(shared_pos[si + spg], shared_pos[si + 2u * spg], shared_len[si + spg]);
		}
		barrier();
	}

	// Clamp velocity, rewrite history. (Upstream scales by clamp²/speed²; this is the intent.)
	vec4 old = g_prev[gv];
	vec3 delta = shared_pos[lid].xyz - old.xyz;
	float speed_sq = dot(delta, delta);
	float clamp_delta = p.misc.y;
	if (speed_sq > clamp_delta * clamp_delta) {
		delta *= clamp_delta / sqrt(speed_sq);
		g_prev[gv].xyz = shared_pos[lid].xyz - delta;
	}

	g_pos[gv] = shared_pos[lid];
}

void update_follow_hair_vertices(uint lid, uint gid) {
	uint gs, ls, gv, lv;
	vertex_indices(lid, gid, gs, ls, gv, lv);
	vec4 pos = g_pos[gv];
	for (uint i = 0u; i < g_follow_per_guide; i++) {
		uint fv = gv + g_verts_per_strand * (i + 1u);
		uint fs = gs + i + 1u;
		float factor = g_tip_separation * (float(lv) / float(g_verts_per_strand)) + 1.0;
		g_pos[fv].xyz = pos.xyz + factor * g_follow_offset[fs].xyz;
	}
}

// Runs last (after SDF collision) over every vertex, guide and follow: final position plus
// the upstream tangent (towards the next vertex, last vertex uses the previous one).
void write_out_pass(uint id) {
	if (id >= uint(p.counts.w)) {
		return;
	}
	uint n = g_verts_per_strand;
	vec4 pos = g_pos[id];
	vec3 t = id % n == n - 1u ? pos.xyz - g_pos[id - 1].xyz : g_pos[id + 1].xyz - pos.xyz;
	write_out(id, pos.xyz, pack_tangent(normalize(t)));
}

void main() {
	uint lid = gl_LocalInvocationID.x;
	uint gid = gl_WorkGroupID.x;
	switch (pc.pass) {
		case 0: integration_and_global_shape(lid, gid); break;
		case 1: calculate_strand_level_data(lid, gid); break;
		case 2: velocity_shock_propagation(lid, gid); break;
		case 3: local_shape_constraints(lid, gid); break;
		case 4: length_constraints_wind_and_collision(lid, gid); break;
		case 5: update_follow_hair_vertices(lid, gid); break;
		case 6: write_out_pass(gl_GlobalInvocationID.x); break;
	}
}
