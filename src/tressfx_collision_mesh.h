// SPDX-License-Identifier: MIT

#pragma once

#include "tressfx_gpu.h"
#include "tressfx_skin.h"

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/triangle_mesh.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>

namespace godot {

// Render-thread half of a TressFXCollisionMesh: the skinned mesh and its signed distance field.
// Filled on the main thread, then handed to the render thread, which owns it until destroy() runs.
struct TressFXCollisionGPU {
	Ref<RDShaderSPIRV> sdf_spirv;
	PackedByteArray vertices;
	PackedByteArray skinning;
	PackedByteArray indices;
	PackedByteArray adjacency;
	int num_vertices = 0;
	int num_triangles = 0;
	int num_cells = 0;

	RenderingDevice *rd = nullptr;
	RID uniform_set;
	RID params_ubo;
	RID bones_buffer;
	RID sdf_buffer;
	LocalVector<RID> rids;

	static void init(int64_t p_self);
	static void update(int64_t p_self, const PackedByteArray &p_bones, const PackedByteArray &p_params);
	static void destroy(int64_t p_self);
	static void debug_print_distances(int64_t p_self, const PackedVector3Array &p_points, const String &p_name);
};

class TressFXCollisionMesh : public Node3D {
	GDCLASS(TressFXCollisionMesh, Node3D)

	String tfxmesh_path;
	Ref<Mesh> mesh;
	float import_scale = 1.0f;
	uint64_t skeleton_id = 0;
	int num_cells_x = 50;
	float collision_margin = 0.0f;
	float push_limit = 1.0f;
	String follow_bone;

	int num_vertices = 0;
	int num_triangles = 0;
	int num_cells = 0; // Per axis, including padding.
	float cell_size = 0.0f;
	float padding = 0.0f;
	Vector3 center; // Rest-pose bounding sphere, model space.
	float radius = 0.0f;
	PackedFloat32Array vertices; // float4 position, float4 normal
	PackedFloat32Array skinning; // float4 bone indices, float4 weights
	PackedInt32Array indices;
	PackedInt32Array adjacency; // 3 per triangle: neighbour across edge 01, 12, 20 (-1 = none)
	TressFXSkin skin;
	int follow_index = -1;
	TressFXCollisionGPU *gpu = nullptr;
	PackedByteArray last_bones;
	PackedByteArray last_params;
	uint64_t file_stamp = 0; // Editor: modification time of the .tfxmesh, to reload it when rewritten.
	uint64_t pending_file_stamp = 0;
	uint64_t last_file_check_msec = 0;
	uint64_t last_used_frame = 0;
	uint64_t last_cpu_usec = 0;
	// Editor gizmo: the skin and node transforms it was last drawn with, and its click shape, which
	// is slow to build and only refreshed once the pose has been still for a moment.
	PackedByteArray gizmo_pose;
	int gizmo_still_frames = 0;
	Ref<TriangleMesh> gizmo_triangles;
	bool gizmo_triangles_stale = true;
	friend class TressFXCollisionGizmoPlugin;

	void _load();
	void _reload();
	void _init_gpu();
	void _unload_gpu();
	void _step();
	bool _parse_tfxmesh(const String &p_path);
	bool _from_mesh(const Ref<Mesh> &p_mesh);
	bool _finish_load();
	void _build_adjacency();
	void _update_gizmo_pose();
	uint64_t _file_stamp() const;
	void _reload_if_file_changed();

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	void set_tfxmesh_path(const String &p_path);
	String get_tfxmesh_path() const;
	void set_mesh(const Ref<Mesh> &p_mesh);
	Ref<Mesh> get_mesh() const;
	void set_import_scale(float p_scale);
	float get_import_scale() const;
	void set_hair_skeleton(Node *p_skeleton);
	Skeleton3D *get_hair_skeleton() const;
	void set_num_cells_x(int p_cells);
	int get_num_cells_x() const;
	void set_collision_margin(float p_margin);
	float get_collision_margin() const;
	void set_push_limit(float p_limit);
	float get_push_limit() const;
	void set_follow_bone(const String &p_bone);
	String get_follow_bone() const;

	int get_cell_count() const;
	int get_vertex_count() const;
	int get_triangle_count() const;
	int get_cpu_time_usec() const;
	void debug_print_distances(const PackedVector3Array &p_points);

	// Main-thread handle of the render-thread state; null until loaded.
	TressFXCollisionGPU *_get_gpu() const { return gpu; }
	// Called by every TressFXHair that simulates against this mesh; unused meshes are not rebuilt.
	void _mark_used();
	// Editor gizmo: the collision surface as the distance field sees it (skinned), in this node's
	// space, with a simple light baked into the vertex colours. Null until loaded.
	Ref<ArrayMesh> build_debug_mesh();

	TressFXCollisionMesh();
};

} // namespace godot
