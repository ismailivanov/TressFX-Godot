// SPDX-License-Identifier: MIT

#include "tressfx_collision_mesh.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/rd_shader_file.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/skin.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <stdlib.h>
#include <string.h>

namespace godot {

static const char *SDF_SHADER_PATH = "res://addons/tressfx/shaders/tressfx_sdf.glsl";
static const int MAX_CELLS_X = 128; // 128 + 2 * 102 padding cells = 332^3 cells = 146 MB, plenty.

// ---------------------------------------------------------------------------------------------
// Render thread

void TressFXCollisionGPU::init(int64_t p_self) {
	TressFXCollisionGPU *g = (TressFXCollisionGPU *)p_self;
	RenderingDevice *rd = RenderingServer::get_singleton()->get_rendering_device();
	if (rd == nullptr) { // Headless, or the Compatibility renderer.
		return;
	}
	g->rd = rd;
	TressFXPipelines::acquire();
	TressFXPipelines::get()->sdf.ensure(rd, g->sdf_spirv);
	if (!TressFXPipelines::get()->sdf.pipeline.is_valid()) {
		return;
	}

	const RID skinning = rd->storage_buffer_create(g->skinning.size(), g->skinning);
	const RID initial = rd->storage_buffer_create(g->vertices.size(), g->vertices);
	const RID skinned = rd->storage_buffer_create(g->vertices.size(), g->vertices);
	const RID index = rd->storage_buffer_create(g->indices.size(), g->indices);
	g->sdf_buffer = rd->storage_buffer_create((uint32_t)g->num_cells * g->num_cells * g->num_cells * sizeof(float));
	g->bones_buffer = rd->storage_buffer_create(TRESSFX_MAX_BONES * 64);
	const RID adjacency = rd->storage_buffer_create(g->adjacency.size(), g->adjacency);
	g->params_ubo = rd->uniform_buffer_create(64);

	TypedArray<RDUniform> uniforms;
	uniforms.push_back(tressfx_make_uniform(0, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, skinning));
	uniforms.push_back(tressfx_make_uniform(1, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, initial));
	uniforms.push_back(tressfx_make_uniform(2, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, skinned));
	uniforms.push_back(tressfx_make_uniform(3, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, index));
	uniforms.push_back(tressfx_make_uniform(4, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, g->sdf_buffer));
	uniforms.push_back(tressfx_make_uniform(5, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, g->bones_buffer));
	uniforms.push_back(tressfx_make_uniform(6, RenderingDevice::UNIFORM_TYPE_UNIFORM_BUFFER, g->params_ubo));
	uniforms.push_back(tressfx_make_uniform(7, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, adjacency));
	g->uniform_set = rd->uniform_set_create(uniforms, TressFXPipelines::get()->sdf.shader, 0);

	// Free order: the uniform set before the buffers it references.
	g->rids.push_back(g->uniform_set);
	g->rids.push_back(g->params_ubo);
	g->rids.push_back(adjacency);
	g->rids.push_back(skinning);
	g->rids.push_back(initial);
	g->rids.push_back(skinned);
	g->rids.push_back(index);
	g->rids.push_back(g->sdf_buffer);
	g->rids.push_back(g->bones_buffer);
}

// Upstream TressFXBoneSkinning::Update + TressFXSDFCollision::Update.
void TressFXCollisionGPU::update(int64_t p_self, const PackedByteArray &p_bones, const PackedByteArray &p_params) {
	TressFXCollisionGPU *g = (TressFXCollisionGPU *)p_self;
	if (!g->uniform_set.is_valid()) {
		return;
	}
	RenderingDevice *rd = g->rd;
	rd->buffer_update(g->bones_buffer, 0, p_bones.size(), p_bones);
	rd->buffer_update(g->params_ubo, 0, p_params.size(), p_params);

	const int cell_groups = (g->num_cells * g->num_cells * g->num_cells + TRESSFX_THREAD_GROUP_SIZE - 1) / TRESSFX_THREAD_GROUP_SIZE;
	const int vertex_groups = (g->num_vertices + TRESSFX_THREAD_GROUP_SIZE - 1) / TRESSFX_THREAD_GROUP_SIZE;
	const int triangle_groups = (g->num_triangles + TRESSFX_THREAD_GROUP_SIZE - 1) / TRESSFX_THREAD_GROUP_SIZE;

	const String timestamp = tressfx_gpu_timing ? "tfx_sdf_" + String::num_uint64((uint64_t)p_self) : String();
	if (tressfx_gpu_timing) {
		rd->capture_timestamp(timestamp + String(":begin"));
	}
	const int64_t list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, TressFXPipelines::get()->sdf.pipeline);
	rd->compute_list_bind_uniform_set(list, g->uniform_set, 0);
	tressfx_dispatch(rd, list, 0, vertex_groups); // Bone skinning.
	tressfx_dispatch(rd, list, 1, cell_groups); // Initialize the SDF.
	tressfx_dispatch(rd, list, 2, triangle_groups); // Construct: atomic min per triangle.
	tressfx_dispatch(rd, list, 3, cell_groups); // Finalize: undo the atomic-friendly bit flip.
	rd->compute_list_end();
	if (tressfx_gpu_timing) {
		rd->capture_timestamp(timestamp + String(":end"));
	}
}

void TressFXCollisionGPU::destroy(int64_t p_self) {
	TressFXCollisionGPU *g = (TressFXCollisionGPU *)p_self;
	if (g->rd != nullptr) {
		tressfx_free_rids(g->rd, g->rids);
		TressFXPipelines::release(g->rd);
	}
	memdelete(g);
}

void TressFXCollisionGPU::debug_print_distances(int64_t p_self, const PackedVector3Array &p_points, const String &p_name) {
	TressFXCollisionGPU *g = (TressFXCollisionGPU *)p_self;
	if (!g->sdf_buffer.is_valid()) {
		return;
	}
	const PackedFloat32Array data = g->rd->buffer_get_data(g->sdf_buffer).to_float32_array();
	int unset = 0;
	for (int i = 0; i < data.size(); i++) {
		if (data[i] >= 1e9f) {
			unset++;
		}
	}
	UtilityFunctions::print("SDF ", p_name, ": ", unset, "/", data.size(), " cells unset");
	const PackedByteArray params = g->rd->buffer_get_data(g->params_ubo);
	const float *pf = (const float *)params.ptr();
	const Vector3 origin(pf[0], pf[1], pf[2]);
	const float cell = pf[4];
	const int n = g->num_cells;
	for (int i = 0; i < p_points.size(); i++) {
		const Vector3 pt = p_points[i];
		const Vector3 cf = ((pt - origin) / cell).floor();
		const Vector3i c((int)cf.x, (int)cf.y, (int)cf.z);
		if (c.x < 0 || c.y < 0 || c.z < 0 || c.x >= n || c.y >= n || c.z >= n) {
			UtilityFunctions::print("  ", pt, " -> outside grid");
			continue;
		}
		UtilityFunctions::print("  ", pt, " -> ", data[n * n * c.z + n * c.y + c.x], " m (cell ", c, ")");
	}
}

// ---------------------------------------------------------------------------------------------
// Main thread

TressFXCollisionMesh::TressFXCollisionMesh() {
}

void TressFXCollisionMesh::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY: {
			if (!Engine::get_singleton()->is_editor_hint()) {
				set_process_priority(90); // After AnimationPlayer, before TressFXHair (100).
			}
			add_to_group("tressfx_collision");
			set_process(true);
			_load();
		} break;
		case NOTIFICATION_ENTER_TREE: {
			// Re-added to the tree (reparenting): the GPU side was freed on exit, rebuild it.
			if (num_vertices > 0 && gpu == nullptr) {
				_init_gpu();
			}
		} break;
		case NOTIFICATION_EXIT_TREE:
		case NOTIFICATION_PREDELETE: {
			_unload_gpu();
		} break;
		case NOTIFICATION_PROCESS: {
			_step();
		} break;
	}
}

void TressFXCollisionMesh::_step() {
	if (num_vertices == 0 || gpu == nullptr) {
		return;
	}
	const uint64_t t0 = Time::get_singleton()->get_ticks_usec();
	skin.update(get_global_transform());
	// Upstream GetBoundingBox: the rest bounds moved by the follow bone (bone 0 = root, or this
	// node's transform without a skeleton).
	const int follow = follow_index >= 0 && follow_index < (int)skin.transforms.size() ? follow_index : 0;
	const Vector3 origin = skin.transforms[follow].xform(center) - Vector3(1, 1, 1) * (radius + padding);

	PackedByteArray params;
	params.resize(64);
	float *pf = (float *)params.ptrw();
	int32_t *pi = (int32_t *)params.ptrw();
	pf[0] = origin.x;
	pf[1] = origin.y;
	pf[2] = origin.z;
	pf[3] = 0.0f;
	pf[4] = cell_size;
	pf[5] = collision_margin * cell_size;
	pf[6] = push_limit * cell_size;
	pf[7] = 0.0f;
	pi[8] = num_cells;
	pi[9] = num_cells;
	pi[10] = num_cells;
	pi[11] = num_triangles;
	pi[12] = num_vertices;
	pi[13] = 0;
	pi[14] = 0;
	pi[15] = 0;
	const PackedByteArray bones = skin.pack();

	// The SDF only depends on the pose and the grid: nothing moved, nothing to rebuild.
	if (bones == last_bones && params == last_params) {
		last_cpu_usec = Time::get_singleton()->get_ticks_usec() - t0;
		return;
	}
	last_bones = bones;
	last_params = params;
	RenderingServer::get_singleton()->call_on_render_thread(
			callable_mp_static(&TressFXCollisionGPU::update).bind((int64_t)gpu, bones, params));
	last_cpu_usec = Time::get_singleton()->get_ticks_usec() - t0;
}

void TressFXCollisionMesh::_load() {
	if (tfxmesh_path.is_empty() && mesh.is_null()) {
		return;
	}
	const uint64_t t = Time::get_singleton()->get_ticks_msec();
	skin.init(get_hair_skeleton());
	const bool ok = mesh.is_valid() ? _from_mesh(mesh) : _parse_tfxmesh(tfxmesh_path);
	if (!ok || !_finish_load()) {
		num_vertices = 0;
		num_triangles = 0;
		return;
	}
	follow_index = skin.find_bone(follow_bone);
	// Upstream: grid over the bounding sphere's cube, cell size from num_cells_x, then
	// 0.8 * num_cells_x padding cells on each side.
	cell_size = 2.0f * radius / num_cells_x;
	const int pad_cells = (int)(0.8f * num_cells_x);
	padding = pad_cells * cell_size;
	num_cells = num_cells_x + 2 * pad_cells;
	_init_gpu();
	UtilityFunctions::print_verbose("TressFX SDF: ", mesh.is_valid() ? mesh->get_path().get_file() : tfxmesh_path.get_file(),
			" - ", num_vertices, " verts, ", num_triangles, " tris, grid ", num_cells, "^3 (",
			String::num(cell_size * 100.0f, 1), " cm cells), ", Time::get_singleton()->get_ticks_msec() - t, " ms");
}

void TressFXCollisionMesh::_init_gpu() {
	TressFXCollisionGPU *g = memnew(TressFXCollisionGPU);
	Ref<RDShaderFile> shader_file = ResourceLoader::get_singleton()->load(SDF_SHADER_PATH);
	if (shader_file.is_valid()) {
		g->sdf_spirv = shader_file->get_spirv();
	} else {
		ERR_PRINT(String("TressFX: cannot load ") + SDF_SHADER_PATH);
	}
	g->vertices = vertices.to_byte_array();
	g->skinning = skinning.to_byte_array();
	g->indices = indices.to_byte_array();
	g->adjacency = adjacency.to_byte_array();
	g->num_vertices = num_vertices;
	g->num_triangles = num_triangles;
	g->num_cells = num_cells;
	gpu = g;
	last_bones = PackedByteArray();
	last_params = PackedByteArray();
	RenderingServer::get_singleton()->call_on_render_thread(callable_mp_static(&TressFXCollisionGPU::init).bind((int64_t)g));
}

void TressFXCollisionMesh::_reload() {
	if (!is_node_ready()) {
		return;
	}
	_unload_gpu();
	num_vertices = 0;
	num_triangles = 0;
	_load();
}

void TressFXCollisionMesh::_unload_gpu() {
	if (gpu == nullptr) {
		return;
	}
	RenderingServer::get_singleton()->call_on_render_thread(callable_mp_static(&TressFXCollisionGPU::destroy).bind((int64_t)gpu));
	gpu = nullptr;
}

// Upstream LoadTressFXCollisionMeshData: a text file with bone names, vertices
// (position, normal, 4 bone indices, 4 weights) and triangles.
bool TressFXCollisionMesh::_parse_tfxmesh(const String &p_path) {
	Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::READ);
	ERR_FAIL_COND_V_MSG(f.is_null(), false, "TressFXCollisionMesh: cannot open " + p_path);
	PackedByteArray data = f->get_buffer(f->get_length());
	data.push_back(0);
	const char *p = (const char *)data.ptr();

	LocalVector<int> bone_ids;
	int vi = 0;
	int ti = 0;
	float *verts = nullptr;
	float *skin_w = nullptr;
	int32_t *idx = nullptr;

	auto skip_blanks = [&p]() {
		while (*p == ' ' || *p == '\t' || *p == '\r') {
			p++;
		}
	};
	auto next_int = [&p]() -> long {
		char *end = nullptr;
		const long v = strtol(p, &end, 10);
		p = end;
		return v;
	};
	auto next_float = [&p]() -> float {
		char *end = nullptr;
		const float v = strtof(p, &end);
		p = end;
		return v;
	};

	while (*p) {
		skip_blanks();
		if (*p == '\n') {
			p++;
			continue;
		}
		if (*p != '#') {
			const char *word = p;
			while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') {
				p++;
			}
			const size_t word_len = p - word;
			if (word_len == 10 && strncmp(word, "numOfBones", 10) == 0) {
				const long n = next_int();
				ERR_FAIL_COND_V_MSG(n < 0 || n > TRESSFX_MAX_BONES, false, p_path + String(": bad bone count"));
				bone_ids.resize(n);
				for (long i = 0; i < n; i++) {
					bone_ids[i] = 0;
				}
			} else if (word_len == 13 && strncmp(word, "numOfVertices", 13) == 0) {
				const long n = next_int();
				ERR_FAIL_COND_V_MSG(n <= 0 || n > 10000000, false, p_path + String(": bad vertex count"));
				num_vertices = n;
				vertices.resize(num_vertices * 8);
				skinning.resize(num_vertices * 8);
				skinning.fill(0.0f);
				verts = vertices.ptrw();
				skin_w = skinning.ptrw();
			} else if (word_len == 14 && strncmp(word, "numOfTriangles", 14) == 0) {
				const long n = next_int();
				ERR_FAIL_COND_V_MSG(n <= 0 || n > 10000000, false, p_path + String(": bad triangle count"));
				num_triangles = n;
				indices.resize(num_triangles * 3);
				idx = indices.ptrw();
			} else if (num_vertices == 0) {
				// "index name"
				const long bi = strtol(word, nullptr, 10);
				skip_blanks();
				const char *name = p;
				while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') {
					p++;
				}
				const String bone_name = String::utf8(name, p - name);
				const int id = skin.find_bone(bone_name);
				if (id < 0 && skin.get_skeleton() != nullptr) {
					WARN_PRINT("TressFXCollisionMesh: bone '" + bone_name + String("' not found in the skeleton."));
				}
				if (bi >= 0 && bi < (long)bone_ids.size()) {
					bone_ids[bi] = id > 0 ? id : 0;
				}
			} else if (num_triangles == 0) {
				// "index x y z nx ny nz j0 j1 j2 j3 w0 w1 w2 w3"
				if (vi < num_vertices) {
					float *o = verts + vi * 8;
					for (int k = 0; k < 3; k++) {
						o[k] = next_float();
					}
					for (int k = 0; k < 3; k++) {
						o[4 + k] = next_float();
					}
					o[3] = 1.0f;
					o[7] = 0.0f;
					for (int k = 0; k < 4; k++) {
						const long bi = next_int();
						skin_w[vi * 8 + k] = bi >= 0 && bi < (long)bone_ids.size() ? (float)bone_ids[bi] : 0.0f;
					}
					for (int k = 0; k < 4; k++) {
						skin_w[vi * 8 + 4 + k] = next_float();
					}
				}
				vi++;
			} else {
				// "index v0 v1 v2"
				if (ti < num_triangles) {
					for (int k = 0; k < 3; k++) {
						idx[ti * 3 + k] = next_int();
					}
				}
				ti++;
			}
		}
		while (*p && *p != '\n') {
			p++;
		}
	}
	ERR_FAIL_COND_V_MSG(vi != num_vertices || ti != num_triangles, false, p_path + String(" is truncated."));
	return true;
}

// Any Godot mesh: positions, normals, indices and (if skinned) bone weights from surface 0.
bool TressFXCollisionMesh::_from_mesh(const Ref<Mesh> &p_mesh) {
	ERR_FAIL_COND_V_MSG(p_mesh->get_surface_count() == 0, false, "TressFXCollisionMesh: mesh has no surfaces.");
	const Array arrays = p_mesh->surface_get_arrays(0);
	const PackedVector3Array verts = arrays[Mesh::ARRAY_VERTEX];
	const PackedVector3Array normals = arrays[Mesh::ARRAY_NORMAL];
	PackedInt32Array index = arrays[Mesh::ARRAY_INDEX];
	const PackedInt32Array bones = arrays[Mesh::ARRAY_BONES];
	const PackedFloat32Array weights = arrays[Mesh::ARRAY_WEIGHTS];
	ERR_FAIL_COND_V_MSG(verts.is_empty(), false, "TressFXCollisionMesh: mesh has no vertices.");
	const int per_vertex = bones.size() == verts.size() * 8 ? 8 : 4;
	const bool skinned = bones.size() == verts.size() * per_vertex && weights.size() == bones.size();

	// Mesh bone indices refer to the Skin's binds; map them to skeleton bones.
	LocalVector<int> bind_to_bone;
	Skeleton3D *skeleton = skin.get_skeleton();
	if (skeleton != nullptr && skinned) {
		TypedArray<Node> children = skeleton->get_children();
		for (int c = 0; c < children.size(); c++) {
			MeshInstance3D *mi = Object::cast_to<MeshInstance3D>(children[c]);
			Ref<Skin> mesh_skin = mi != nullptr ? mi->get_skin() : Ref<Skin>();
			if (mesh_skin.is_null()) {
				continue;
			}
			for (int j = 0; j < mesh_skin->get_bind_count(); j++) {
				const int b = mesh_skin->get_bind_bone(j);
				bind_to_bone.push_back(b >= 0 ? b : skeleton->find_bone(mesh_skin->get_bind_name(j)));
			}
			break;
		}
	}

	num_vertices = verts.size();
	vertices.resize(num_vertices * 8);
	skinning.resize(num_vertices * 8);
	skinning.fill(0.0f);
	float *v = vertices.ptrw();
	float *s = skinning.ptrw();
	for (int i = 0; i < num_vertices; i++) {
		float *o = v + i * 8;
		o[0] = verts[i].x;
		o[1] = verts[i].y;
		o[2] = verts[i].z;
		o[3] = 1.0f;
		const Vector3 normal = i < normals.size() ? normals[i] : Vector3(0, 1, 0);
		o[4] = normal.x;
		o[5] = normal.y;
		o[6] = normal.z;
		o[7] = 0.0f;
		if (!skinned) {
			s[i * 8 + 4] = 1.0f; // All on bone 0.
			continue;
		}
		for (int k = 0; k < 4; k++) {
			const int bi = bones[i * per_vertex + k];
			s[i * 8 + k] = bi >= 0 && bi < (int)bind_to_bone.size() ? (float)bind_to_bone[bi] : (float)bi;
			s[i * 8 + 4 + k] = weights[i * per_vertex + k];
		}
	}
	if (index.is_empty()) { // Non-indexed triangle list.
		index.resize(num_vertices);
		int32_t *iw = index.ptrw();
		for (int i = 0; i < num_vertices; i++) {
			iw[i] = i;
		}
	}
	num_triangles = index.size() / 3;
	indices = index;
	return true;
}

bool TressFXCollisionMesh::_finish_load() {
	const int32_t *idx = indices.ptr();
	for (int i = 0; i < num_triangles * 3; i++) {
		ERR_FAIL_COND_V_MSG(idx[i] < 0 || idx[i] >= num_vertices, false, "TressFXCollisionMesh: triangle index out of range.");
	}
	float *v = vertices.ptrw();
	if (import_scale != 1.0f) {
		for (int i = 0; i < num_vertices; i++) {
			v[i * 8] *= import_scale;
			v[i * 8 + 1] *= import_scale;
			v[i * 8 + 2] *= import_scale;
		}
	}
	// Upstream bounds: a sphere around the vertex centroid.
	center = Vector3();
	for (int i = 0; i < num_vertices; i++) {
		center += Vector3(v[i * 8], v[i * 8 + 1], v[i * 8 + 2]);
	}
	center /= (float)num_vertices;
	radius = 0.0f;
	for (int i = 0; i < num_vertices; i++) {
		radius = MAX(radius, Vector3(v[i * 8], v[i * 8 + 1], v[i * 8 + 2]).distance_to(center));
	}
	ERR_FAIL_COND_V_MSG(radius <= 0.0f, false, "TressFXCollisionMesh: mesh has no extent.");
	_build_adjacency();
	return true;
}

// Neighbouring triangle across each edge, for the SDF's edge pseudo-normals. Vertices are welded
// by position first (imported meshes split them along normal/UV seams).
void TressFXCollisionMesh::_build_adjacency() {
	adjacency.resize(num_triangles * 3);
	adjacency.fill(-1);
	const float *v = vertices.ptr();
	const int32_t *idx = indices.ptr();

	// Quantize positions to a million steps across the bounds: ~1 micrometre on a metre-sized mesh.
	Vector3 lo(v[0], v[1], v[2]);
	Vector3 hi = lo;
	for (int i = 1; i < num_vertices; i++) {
		const Vector3 p(v[i * 8], v[i * 8 + 1], v[i * 8 + 2]);
		lo = lo.min(p);
		hi = hi.max(p);
	}
	const Vector3 extent = (hi - lo).max(Vector3(1e-9f, 1e-9f, 1e-9f));
	const Vector3 inv = Vector3(1048575.0f, 1048575.0f, 1048575.0f) / extent;

	LocalVector<int> weld;
	weld.resize(num_vertices);
	HashMap<uint64_t, int> by_position;
	for (int i = 0; i < num_vertices; i++) {
		const Vector3 q = ((Vector3(v[i * 8], v[i * 8 + 1], v[i * 8 + 2]) - lo) * inv).round();
		const uint64_t key = ((uint64_t)q.x << 42) | ((uint64_t)q.y << 21) | (uint64_t)q.z;
		const int *first = by_position.getptr(key);
		if (first != nullptr) {
			weld[i] = *first;
		} else {
			by_position.insert(key, i);
			weld[i] = i;
		}
	}

	HashMap<uint64_t, int> edges; // (min index << 32 | max index) -> triangle * 3 + edge slot
	int32_t *adj = adjacency.ptrw();
	for (int t = 0; t < num_triangles; t++) {
		for (int e = 0; e < 3; e++) {
			const uint32_t a = weld[idx[t * 3 + e]];
			const uint32_t b = weld[idx[t * 3 + (e + 1) % 3]];
			const uint64_t key = ((uint64_t)MIN(a, b) << 32) | (uint64_t)MAX(a, b);
			const int *other = edges.getptr(key);
			if (other != nullptr) {
				adj[t * 3 + e] = *other / 3;
				adj[*other] = t;
			} else {
				edges.insert(key, t * 3 + e);
			}
		}
	}
}

void TressFXCollisionMesh::debug_print_distances(const PackedVector3Array &p_points) {
	if (gpu == nullptr) {
		return;
	}
	RenderingServer::get_singleton()->call_on_render_thread(
			callable_mp_static(&TressFXCollisionGPU::debug_print_distances).bind((int64_t)gpu, p_points, String(get_name())));
}

// ---------------------------------------------------------------------------------------------
// Properties

void TressFXCollisionMesh::set_tfxmesh_path(const String &p_path) {
	tfxmesh_path = p_path;
	_reload();
}

String TressFXCollisionMesh::get_tfxmesh_path() const {
	return tfxmesh_path;
}

void TressFXCollisionMesh::set_mesh(const Ref<Mesh> &p_mesh) {
	mesh = p_mesh;
	_reload();
}

Ref<Mesh> TressFXCollisionMesh::get_mesh() const {
	return mesh;
}

void TressFXCollisionMesh::set_import_scale(float p_scale) {
	import_scale = p_scale;
	_reload();
}

float TressFXCollisionMesh::get_import_scale() const {
	return import_scale;
}

void TressFXCollisionMesh::set_hair_skeleton(Node *p_skeleton) {
	Skeleton3D *skeleton = Object::cast_to<Skeleton3D>(p_skeleton);
	skeleton_id = skeleton != nullptr ? skeleton->get_instance_id() : 0;
	_reload();
}

Skeleton3D *TressFXCollisionMesh::get_hair_skeleton() const {
	return skeleton_id != 0 ? Object::cast_to<Skeleton3D>(ObjectDB::get_instance(skeleton_id)) : nullptr;
}

void TressFXCollisionMesh::set_num_cells_x(int p_cells) {
	num_cells_x = CLAMP(p_cells, 4, MAX_CELLS_X);
	_reload();
}

int TressFXCollisionMesh::get_num_cells_x() const {
	return num_cells_x;
}

void TressFXCollisionMesh::set_collision_margin(float p_margin) {
	collision_margin = p_margin;
}

float TressFXCollisionMesh::get_collision_margin() const {
	return collision_margin;
}

void TressFXCollisionMesh::set_push_limit(float p_limit) {
	push_limit = p_limit;
}

float TressFXCollisionMesh::get_push_limit() const {
	return push_limit;
}

void TressFXCollisionMesh::set_follow_bone(const String &p_bone) {
	follow_bone = p_bone;
	follow_index = skin.find_bone(p_bone);
}

String TressFXCollisionMesh::get_follow_bone() const {
	return follow_bone;
}

int TressFXCollisionMesh::get_cell_count() const {
	return num_cells;
}

int TressFXCollisionMesh::get_vertex_count() const {
	return num_vertices;
}

int TressFXCollisionMesh::get_triangle_count() const {
	return num_triangles;
}

int TressFXCollisionMesh::get_cpu_time_usec() const {
	return (int)last_cpu_usec;
}

void TressFXCollisionMesh::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_tfxmesh_path", "path"), &TressFXCollisionMesh::set_tfxmesh_path);
	ClassDB::bind_method(D_METHOD("get_tfxmesh_path"), &TressFXCollisionMesh::get_tfxmesh_path);
	ClassDB::bind_method(D_METHOD("set_mesh", "mesh"), &TressFXCollisionMesh::set_mesh);
	ClassDB::bind_method(D_METHOD("get_mesh"), &TressFXCollisionMesh::get_mesh);
	ClassDB::bind_method(D_METHOD("set_import_scale", "scale"), &TressFXCollisionMesh::set_import_scale);
	ClassDB::bind_method(D_METHOD("get_import_scale"), &TressFXCollisionMesh::get_import_scale);
	ClassDB::bind_method(D_METHOD("set_hair_skeleton", "skeleton"), &TressFXCollisionMesh::set_hair_skeleton);
	ClassDB::bind_method(D_METHOD("get_hair_skeleton"), &TressFXCollisionMesh::get_hair_skeleton);
	ClassDB::bind_method(D_METHOD("set_num_cells_x", "cells"), &TressFXCollisionMesh::set_num_cells_x);
	ClassDB::bind_method(D_METHOD("get_num_cells_x"), &TressFXCollisionMesh::get_num_cells_x);
	ClassDB::bind_method(D_METHOD("set_collision_margin", "margin"), &TressFXCollisionMesh::set_collision_margin);
	ClassDB::bind_method(D_METHOD("get_collision_margin"), &TressFXCollisionMesh::get_collision_margin);
	ClassDB::bind_method(D_METHOD("set_push_limit", "limit"), &TressFXCollisionMesh::set_push_limit);
	ClassDB::bind_method(D_METHOD("get_push_limit"), &TressFXCollisionMesh::get_push_limit);
	ClassDB::bind_method(D_METHOD("set_follow_bone", "bone"), &TressFXCollisionMesh::set_follow_bone);
	ClassDB::bind_method(D_METHOD("get_follow_bone"), &TressFXCollisionMesh::get_follow_bone);

	ClassDB::bind_method(D_METHOD("get_cell_count"), &TressFXCollisionMesh::get_cell_count);
	ClassDB::bind_method(D_METHOD("get_vertex_count"), &TressFXCollisionMesh::get_vertex_count);
	ClassDB::bind_method(D_METHOD("get_triangle_count"), &TressFXCollisionMesh::get_triangle_count);
	ClassDB::bind_method(D_METHOD("get_cpu_time_usec"), &TressFXCollisionMesh::get_cpu_time_usec);
	ClassDB::bind_method(D_METHOD("debug_print_distances", "points"), &TressFXCollisionMesh::debug_print_distances);

	ADD_PROPERTY(PropertyInfo(Variant::STRING, "tfxmesh_path", PROPERTY_HINT_FILE, "*.tfxmesh"), "set_tfxmesh_path", "get_tfxmesh_path");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "mesh", PROPERTY_HINT_RESOURCE_TYPE, "Mesh"), "set_mesh", "get_mesh");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "import_scale"), "set_import_scale", "get_import_scale");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "hair_skeleton", PROPERTY_HINT_NODE_TYPE, "Skeleton3D"), "set_hair_skeleton", "get_hair_skeleton");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "num_cells_x", PROPERTY_HINT_RANGE, "4,128,1"), "set_num_cells_x", "get_num_cells_x");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "collision_margin"), "set_collision_margin", "get_collision_margin");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "push_limit"), "set_push_limit", "get_push_limit");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "follow_bone"), "set_follow_bone", "get_follow_bone");
}

} // namespace godot
