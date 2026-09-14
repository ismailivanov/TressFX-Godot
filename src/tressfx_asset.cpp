// SPDX-License-Identifier: MIT

#include "tressfx_asset.h"

#include "tressfx_gpu.h"
#include "tressfx_skin.h"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/random_number_generator.hpp>
#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/templates/local_vector.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <string.h>

namespace godot {

// Upstream GetTangentVectors: a vector orthogonal to p_n.
static Vector3 _tangent0(const Vector3 &p_n) {
	if (Math::abs(p_n.z) > 0.707f) {
		return Vector3(0, -p_n.z, p_n.y).normalized();
	}
	return Vector3(-p_n.y, p_n.x, 0).normalized();
}

bool TressFXAsset::load(const String &p_path, int p_num_follow_hairs, float p_tip_separation, float p_scale, float p_follow_radius) {
	if (!_load_hair_data(p_path)) {
		return false;
	}
	if (p_scale != 1.0f) {
		float *pos = positions.ptrw();
		for (int i = 0; i < num_total_vertices; i++) {
			pos[i * 4] *= p_scale;
			pos[i * 4 + 1] *= p_scale;
			pos[i * 4 + 2] *= p_scale;
		}
	}
	_generate_follow_hairs(p_num_follow_hairs, p_tip_separation, p_follow_radius);
	_process();
	return true;
}

bool TressFXAsset::_load_hair_data(const String &p_path) {
	Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::READ);
	ERR_FAIL_COND_V_MSG(f.is_null(), false, "TressFX: cannot open " + p_path);

	float version = f->get_float();
	ERR_FAIL_COND_V_MSG(version < 4.0f, false, p_path + String(": .tfx version ") + String::num(version, 1) + String(" is too old, 4.0 or newer is required."));
	int strands_in_file = f->get_32();
	num_verts_per_strand = f->get_32();
	uint32_t offset_positions = f->get_32();
	uint32_t offset_strand_uv = f->get_32();
	ERR_FAIL_COND_V_MSG(strands_in_file <= 0 || num_verts_per_strand <= 2 || TRESSFX_THREAD_GROUP_SIZE % num_verts_per_strand != 0, false,
			p_path + String(": vertices per strand must be 4, 8, 16, 32 or 64."));
	const int n = num_verts_per_strand;

	// Pad the strand count to a multiple of the thread group size (upstream always adds a group).
	num_guide_strands = (strands_in_file - strands_in_file % TRESSFX_THREAD_GROUP_SIZE) + TRESSFX_THREAD_GROUP_SIZE;
	num_total_strands = num_guide_strands;
	num_total_vertices = num_guide_strands * n;

	f->seek(offset_positions);
	positions = f->get_buffer((int64_t)strands_in_file * n * 16).to_float32_array();
	ERR_FAIL_COND_V_MSG(positions.size() != strands_in_file * n * 4, false, p_path + String(" is truncated."));
	positions.resize(num_total_vertices * 4);
	strand_uv.resize(num_total_strands * 2);
	strand_uv.fill(0.0f); // Zeros when the file has no strand UVs.
	if (offset_strand_uv > 0) {
		f->seek(offset_strand_uv);
		PackedFloat32Array uv = f->get_buffer((int64_t)strands_in_file * 8).to_float32_array();
		if (uv.size() == strands_in_file * 2) {
			memcpy(strand_uv.ptrw(), uv.ptr(), uv.size() * sizeof(float));
		}
	}

	// Padding strands are copies of the last real strand.
	float *pos = positions.ptrw();
	float *uv = strand_uv.ptrw();
	const int last = (strands_in_file - 1) * n * 4;
	for (int s = strands_in_file; s < num_guide_strands; s++) {
		memcpy(pos + s * n * 4, pos + last, n * 4 * sizeof(float));
		uv[s * 2] = uv[(strands_in_file - 1) * 2];
		uv[s * 2 + 1] = uv[(strands_in_file - 1) * 2 + 1];
	}

	follow_root_offsets.resize(num_total_strands * 4);
	follow_root_offsets.fill(0.0f);
	return true;
}

void TressFXAsset::_generate_follow_hairs(int p_num_follow, float p_tip_separation, float p_radius) {
	num_follow_per_guide = p_num_follow > 0 ? p_num_follow : 0;
	if (num_follow_per_guide == 0) {
		return;
	}
	const int n = num_verts_per_strand;
	const int stride = num_follow_per_guide + 1;
	num_total_strands = num_guide_strands * stride;
	num_total_vertices = num_total_strands * n;

	PackedFloat32Array guide_positions = positions;
	PackedFloat32Array guide_uv = strand_uv;
	positions = PackedFloat32Array();
	positions.resize(num_total_vertices * 4);
	strand_uv = PackedFloat32Array();
	strand_uv.resize(num_total_strands * 2);
	follow_root_offsets.resize(num_total_strands * 4);
	follow_root_offsets.fill(0.0f);

	// Upstream uses rand(); a fixed seed makes the groom identical on every load.
	Ref<RandomNumberGenerator> rng;
	rng.instantiate();
	rng->set_seed(0);

	const float *gp = guide_positions.ptr();
	const float *gu = guide_uv.ptr();
	float *pos = positions.ptrw();
	float *uv = strand_uv.ptrw();
	float *off = follow_root_offsets.ptrw();

	for (int g = 0; g < num_guide_strands; g++) {
		const int gs = g * stride; // Index of the guide strand in the interleaved layout.
		const int gv = gs * n * 4;
		memcpy(pos + gv, gp + g * n * 4, n * 4 * sizeof(float));
		uv[gs * 2] = gu[g * 2];
		uv[gs * 2 + 1] = gu[g * 2 + 1];
		off[gs * 4 + 3] = gs;

		const Vector3 root(gp[g * n * 4], gp[g * n * 4 + 1], gp[g * n * 4 + 2]);
		const Vector3 v1(gp[g * n * 4 + 4], gp[g * n * 4 + 5], gp[g * n * 4 + 6]);
		const Vector3 dir = (v1 - root).normalized();
		const Vector3 t0 = _tangent0(dir);
		const Vector3 t1 = dir.cross(t0);

		for (int j = 0; j < num_follow_per_guide; j++) {
			const int fs = gs + j + 1;
			uv[fs * 2] = uv[gs * 2];
			uv[fs * 2 + 1] = uv[gs * 2 + 1];
			const Vector3 o = t0 * rng->randf_range(-p_radius, p_radius) + t1 * rng->randf_range(-p_radius, p_radius);
			off[fs * 4] = o.x;
			off[fs * 4 + 1] = o.y;
			off[fs * 4 + 2] = o.z;
			off[fs * 4 + 3] = gs;
			const int fv = fs * n * 4;
			for (int k = 0; k < n; k++) {
				const float factor = p_tip_separation * ((float)k / n) + 1.0f;
				pos[fv + k * 4] = pos[gv + k * 4] + o.x * factor;
				pos[fv + k * 4 + 1] = pos[gv + k * 4 + 1] + o.y * factor;
				pos[fv + k * 4 + 2] = pos[gv + k * 4 + 2] + o.z * factor;
				pos[fv + k * 4 + 3] = pos[gv + k * 4 + 3];
			}
		}
	}
}

bool TressFXAsset::load_bone_data(const String &p_path, const TressFXSkin &p_skin) {
	Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::READ);
	ERR_FAIL_COND_V_MSG(f.is_null(), false, "TressFX: cannot open " + p_path);

	const uint32_t num_bones = f->get_32();
	ERR_FAIL_COND_V_MSG(num_bones > 100000, false, p_path + String(" is not a .tfxbone file."));
	LocalVector<int> engine_ids;
	engine_ids.resize(num_bones);
	for (uint32_t i = 0; i < num_bones; i++) {
		f->get_32(); // Bone index in the file, sequential.
		const uint32_t name_len = f->get_32(); // Includes the null terminator.
		ERR_FAIL_COND_V_MSG(name_len == 0 || name_len > 1024, false, p_path + String(" is corrupt."));
		const String bone_name = f->get_buffer(name_len - 1).get_string_from_ascii();
		f->get_8();
		const int id = p_skin.find_bone(bone_name);
		if (id < 0) {
			WARN_PRINT("TressFX: bone '" + bone_name + String("' from ") + p_path.get_file() + String(" not found in the skeleton."));
		}
		engine_ids[i] = id > 0 ? id : 0;
	}

	const int strands_in_file = f->get_32();
	ERR_FAIL_COND_V_MSG(strands_in_file < 0 || strands_in_file > num_guide_strands, false, p_path + String(" has more strands than the .tfx file."));
	const int stride = num_follow_per_guide + 1;
	float entry[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
	float *skinning = bone_skinning.ptrw();
	for (int i = 0; i < strands_in_file; i++) {
		f->get_32(); // Strand index, sequential.
		for (int j = 0; j < 4; j++) {
			const uint32_t bone = f->get_32();
			const float weight = f->get_float();
			// -1 (stored unsigned) means no bone; its weight is 0 anyway.
			entry[j] = bone < num_bones ? (float)engine_ids[bone] : 0.0f;
			entry[4 + j] = weight;
		}
		memcpy(skinning + (size_t)i * stride * 8, entry, sizeof(entry));
	}
	// Padding strands reuse the last entry (upstream does the same).
	for (int i = strands_in_file; i < num_guide_strands; i++) {
		memcpy(skinning + (size_t)i * stride * 8, entry, sizeof(entry));
	}
	return true;
}

void TressFXAsset::_process() {
	const int n = num_verts_per_strand;
	// Default skinning: every strand fully on bone 0 (replaced by load_bone_data).
	bone_skinning.resize(num_total_strands * 8);
	bone_skinning.fill(0.0f);
	float *skinning = bone_skinning.ptrw();
	for (int s = 0; s < num_total_strands; s++) {
		skinning[s * 8 + 4] = 1.0f;
	}
	// Upstream also computes tangents and thickness coefficients here; neither is used by the
	// simulation (tangents come out of the last compute pass), so they are skipped.
	rest_lengths.resize(num_total_vertices);
	float *rest = rest_lengths.ptrw();
	const float *pos = positions.ptr();
	for (int s = 0; s < num_total_strands; s++) {
		const int base = s * n;
		for (int i = 0; i < n - 1; i++) {
			const float *a = pos + (base + i) * 4;
			const float *b = a + 4;
			rest[base + i] = Vector3(a[0], a[1], a[2]).distance_to(Vector3(b[0], b[1], b[2]));
		}
		rest[base + n - 1] = 0.0f;
	}
}

float TressFXAsset::rest_radius() const {
	float r2 = 0.0f;
	const float *pos = positions.ptr();
	for (int i = 0; i < num_total_vertices; i += 16) {
		r2 = MAX(r2, Vector3(pos[i * 4], pos[i * 4 + 1], pos[i * 4 + 2]).length_squared());
	}
	return Math::sqrt(r2);
}

} // namespace godot
