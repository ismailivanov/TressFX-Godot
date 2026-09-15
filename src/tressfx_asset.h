// SPDX-License-Identifier: MIT

#pragma once

#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/string.hpp>

namespace godot {

struct TressFXSkin;

// A .tfx hair asset in memory: port of upstream TressFXAsset (LoadHairData, GenerateFollowHairs,
// ProcessAsset). Flat float arrays, one float4 per vertex, guide and follow strands interleaved
// (guide, its follow strands, next guide, ...).
struct TressFXAsset {
	int num_verts_per_strand = 0;
	int num_guide_strands = 0;
	int num_follow_per_guide = 0;
	int num_total_strands = 0;
	int num_total_vertices = 0;

	PackedFloat32Array positions; // float4 per vertex, w = 0 pins the vertex
	PackedFloat32Array strand_uv; // float2 per strand
	PackedFloat32Array follow_root_offsets; // float4 per strand, w = guide strand index
	PackedFloat32Array rest_lengths; // float per vertex, 0 on the last vertex of a strand
	PackedFloat32Array bone_skinning; // per strand: float4 bone indices, float4 weights

	// p_scale converts the file's units to metres (0.01 for a centimetre asset); p_follow_radius
	// is the spread of follow hairs around their guide, in metres.
	bool load(const String &p_path, int p_num_follow_hairs, float p_tip_separation, float p_scale, float p_follow_radius);
	// Loads a .tfxbone (upstream LoadBoneData) into bone_skinning; bone names are looked up in p_skin.
	bool load_bone_data(const String &p_path, const TressFXSkin &p_skin);
	// Bounding sphere of the rest pose, sampled: centre in model space, returns the radius.
	float rest_bounds(Vector3 &r_center) const;

private:
	bool _load_hair_data(const String &p_path);
	void _generate_follow_hairs(int p_num_follow, float p_tip_separation, float p_radius);
	void _process();
};

} // namespace godot
