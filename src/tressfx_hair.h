// SPDX-License-Identifier: MIT

#pragma once

#include "tressfx_asset.h"
#include "tressfx_collision_mesh.h"
#include "tressfx_gpu.h"
#include "tressfx_skin.h"

#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/texture2drd.hpp>
#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/templates/pair.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>

namespace godot {

// Render-thread half of a TressFXHair: the simulation buffers and the output texture. Filled on
// the main thread, then handed to the render thread, which owns it until destroy() runs.
struct TressFXHairGPU {
	Ref<RDShaderSPIRV> sim_spirv;
	Ref<RDShaderSPIRV> collide_spirv;
	PackedByteArray positions;
	PackedByteArray rest_lengths;
	PackedByteArray follow_root_offsets;
	PackedByteArray bone_skinning;
	int num_guide_strands = 0;
	int num_total_strands = 0;
	int num_verts_per_strand = 0;
	int num_total_vertices = 0;
	int num_follow_per_guide = 0;

	RenderingDevice *rd = nullptr;
	RID uniform_set;
	RID params_ubo;
	RID bones_buffer;
	RID positions_buffer;
	RID previous_buffer;
	RID image;
	LocalVector<RID> rids;
	HashMap<uint64_t, Pair<RID, RID>> collision_sets; // SDF buffer id -> uniform sets 0 and 1
	PackedFloat32Array captured_positions;

	static void init(int64_t p_self);
	static void simulate(int64_t p_self, const PackedByteArray &p_params, const PackedByteArray &p_bones, const PackedInt64Array &p_colliders);
	static void capture(int64_t p_self);
	static void destroy(int64_t p_self);

private:
	const Pair<RID, RID> &_collision_sets_for(TressFXCollisionGPU *p_collider);
};

class TressFXHair : public Node3D {
	GDCLASS(TressFXHair, Node3D)

	static const int LOD_BUCKETS = 4;
	// Editor only: stop simulating after this many frames without pose or parameter changes.
	static const int EDITOR_IDLE_FRAMES = 300;

	String tfx_path;
	String tfxbone_path;
	uint64_t skeleton_id = 0;
	int num_follow_hairs = 0;
	float tip_separation = 0.0f;
	float follow_radius = 0.012f;
	float import_scale = 1.0f;
	Ref<ShaderMaterial> material;
	bool cast_hair_shadows = false;
	bool lod_enabled = false;
	float lod_start_distance = 1.0f;
	float lod_end_distance = 5.0f;
	float lod_percent = 0.5f;
	float lod_width_multiplier = 2.0f;
	TypedArray<TressFXCollisionMesh> collision_meshes;
	int collision_skip_root_vertices = 2;
	float vsp_coeff = 0.758f;
	float vsp_accel_threshold = 1.208f;
	float local_stiffness = 0.908f;
	int local_iterations = 2;
	float global_stiffness = 0.408f;
	float global_range = 0.308f;
	int length_iterations = 2;
	float damping = 0.068f;
	float gravity = 0.09f;
	Vector3 wind_direction = Vector3(1, 0, 0);
	float wind_magnitude = 0.0f;
	float clamp_position_delta = 20.0f;
	float simulation_distance = 0.0f;
	bool simulate_offscreen = false;

	TressFXAsset asset;
	bool loaded = false;
	TressFXSkin skin;
	TressFXHairGPU *gpu = nullptr;
	Ref<Texture2DRD> texture;
	bool texture_bound = false;
	Ref<ShaderMaterial> active_material;
	Ref<ImageTexture> strand_uv_texture; // One texel per strand: its coordinate on the body albedo.
	LocalVector<MeshInstance3D *> mesh_instances; // One per LOD bucket, internal children.
	int frame = 0;
	int idle_frames = 0;
	int skipped_frames = 0;
	PackedByteArray last_params;
	PackedByteArray last_bones;
	Vector3 last_center;
	Vector3 bound_center; // Rest-pose centroid in skeleton (or node) space.
	float bound_radius = 1.0f;
	uint64_t last_cpu_usec = 0;

	void _load();
	void _reload();
	void _init_gpu();
	void _unload_gpu();
	void _step(double p_delta);
	void _apply_material();
	void _apply_lod();
	void _make_meshes();
	PackedByteArray _pack_params(float p_delta) const;
	void _wind_corners(float *r_out) const;
	bool _inputs_changed(const PackedByteArray &p_params, const PackedByteArray &p_bones);
	bool _should_simulate(const Vector3 &p_center) const;
	GeometryInstance3D::ShadowCastingSetting _shadow_setting() const;

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	void set_tfx_path(const String &p_path);
	String get_tfx_path() const;
	void set_tfxbone_path(const String &p_path);
	String get_tfxbone_path() const;
	void set_hair_skeleton(Node *p_skeleton);
	Skeleton3D *get_hair_skeleton() const;
	void set_num_follow_hairs(int p_count);
	int get_num_follow_hairs() const;
	void set_tip_separation(float p_separation);
	float get_tip_separation() const;
	void set_follow_radius(float p_radius);
	float get_follow_radius() const;
	void set_import_scale(float p_scale);
	float get_import_scale() const;
	void set_material(const Ref<ShaderMaterial> &p_material);
	Ref<ShaderMaterial> get_material() const;
	void set_cast_hair_shadows(bool p_enabled);
	bool get_cast_hair_shadows() const;

	void set_lod_enabled(bool p_enabled);
	bool get_lod_enabled() const;
	void set_lod_start_distance(float p_distance);
	float get_lod_start_distance() const;
	void set_lod_end_distance(float p_distance);
	float get_lod_end_distance() const;
	void set_lod_percent(float p_percent);
	float get_lod_percent() const;
	void set_lod_width_multiplier(float p_multiplier);
	float get_lod_width_multiplier() const;

	void set_collision_meshes(const TypedArray<TressFXCollisionMesh> &p_meshes);
	TypedArray<TressFXCollisionMesh> get_collision_meshes() const;
	void set_collision_skip_root_vertices(int p_count);
	int get_collision_skip_root_vertices() const;

	void set_vsp_coeff(float p_value);
	float get_vsp_coeff() const;
	void set_vsp_accel_threshold(float p_value);
	float get_vsp_accel_threshold() const;
	void set_local_stiffness(float p_value);
	float get_local_stiffness() const;
	void set_local_iterations(int p_value);
	int get_local_iterations() const;
	void set_global_stiffness(float p_value);
	float get_global_stiffness() const;
	void set_global_range(float p_value);
	float get_global_range() const;
	void set_length_iterations(int p_value);
	int get_length_iterations() const;
	void set_damping(float p_value);
	float get_damping() const;
	void set_gravity(float p_value);
	float get_gravity() const;
	void set_wind_direction(const Vector3 &p_direction);
	Vector3 get_wind_direction() const;
	void set_wind_magnitude(float p_value);
	float get_wind_magnitude() const;
	void set_clamp_position_delta(float p_value);
	float get_clamp_position_delta() const;
	void set_simulation_distance(float p_distance);
	float get_simulation_distance() const;
	void set_simulate_offscreen(bool p_enabled);
	bool get_simulate_offscreen() const;

	void reset_positions();
	int get_strand_count() const;
	int get_guide_strand_count() const;
	int get_vertex_count() const;
	int get_vertices_per_strand() const;
	int get_cpu_time_usec() const;
	void capture_positions();
	PackedFloat32Array get_captured_positions() const;

	TressFXHair();
};

} // namespace godot
