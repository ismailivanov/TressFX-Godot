// SPDX-License-Identifier: MIT

#include "tressfx_hair.h"

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/rd_shader_file.hpp>
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <string.h>

namespace godot {

static const char *SIM_SHADER_PATH = "res://addons/tressfx/shaders/tressfx_sim.glsl";
static const char *COLLIDE_SHADER_PATH = "res://addons/tressfx/shaders/tressfx_sdf_collide.glsl";
static const char *STRAND_SHADER_PATH = "res://addons/tressfx/shaders/tressfx_strand.gdshader";

// Byte layout of the std140 `Params` block in tressfx_sim.glsl (160 bytes).
static const int PARAMS_SIZE = 160;
static const int PARAMS_DT_OFFSET = 84; // grav_time_tip.y, the only per-frame value that always changes
static const int PARAMS_SIM_INTS_OFFSET = 96; // length iterations, local iterations, skip root vertices

static Ref<RDShaderSPIRV> _load_spirv(const char *p_path) {
	Ref<RDShaderFile> file = ResourceLoader::get_singleton()->load(p_path);
	ERR_FAIL_COND_V_MSG(file.is_null(), Ref<RDShaderSPIRV>(), String("TressFX: cannot load ") + p_path);
	return file->get_spirv();
}

// ---------------------------------------------------------------------------------------------
// Render thread

void TressFXHairGPU::init(int64_t p_self) {
	TressFXHairGPU *g = (TressFXHairGPU *)p_self;
	RenderingDevice *rd = RenderingServer::get_singleton()->get_rendering_device();
	if (rd == nullptr) { // Headless, or the Compatibility renderer.
		return;
	}
	g->rd = rd;
	TressFXPipelines::acquire();
	TressFXPipelines::get()->simulation.ensure(rd, g->sim_spirv);
	TressFXPipelines::get()->collision.ensure(rd, g->collide_spirv);
	if (!TressFXPipelines::get()->simulation.pipeline.is_valid() || !TressFXPipelines::get()->collision.pipeline.is_valid()) {
		return;
	}

	const PackedByteArray &pos = g->positions;
	RID buffers[9] = {
		rd->storage_buffer_create(pos.size(), pos), // 0 positions
		rd->storage_buffer_create(pos.size(), pos), // 1 previous
		rd->storage_buffer_create(pos.size(), pos), // 2 previous previous
		rd->storage_buffer_create(g->num_total_strands * 48), // 3 strand level data
		rd->storage_buffer_create(pos.size(), pos), // 4 initial (rest) positions
		rd->storage_buffer_create(g->rest_lengths.size(), g->rest_lengths), // 5
		rd->storage_buffer_create(g->follow_root_offsets.size(), g->follow_root_offsets), // 6
		rd->storage_buffer_create(g->bone_skinning.size(), g->bone_skinning), // 7
		rd->storage_buffer_create(TRESSFX_MAX_BONES * 64), // 8 bone matrices
	};
	g->positions_buffer = buffers[0];
	g->previous_buffer = buffers[1];
	g->bones_buffer = buffers[8];
	g->params_ubo = rd->uniform_buffer_create(PARAMS_SIZE);

	Ref<RDTextureFormat> format;
	format.instantiate();
	format->set_format(RenderingDevice::DATA_FORMAT_R32G32B32A32_SFLOAT);
	format->set_width(TRESSFX_TEXTURE_WIDTH);
	format->set_height((g->num_total_vertices + TRESSFX_TEXTURE_WIDTH - 1) / TRESSFX_TEXTURE_WIDTH);
	format->set_usage_bits(RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT | RenderingDevice::TEXTURE_USAGE_STORAGE_BIT);
	Ref<RDTextureView> view;
	view.instantiate();
	g->image = rd->texture_create(format, view);

	TypedArray<RDUniform> uniforms;
	for (int i = 0; i < 9; i++) {
		uniforms.push_back(tressfx_make_uniform(i, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, buffers[i]));
	}
	uniforms.push_back(tressfx_make_uniform(9, RenderingDevice::UNIFORM_TYPE_UNIFORM_BUFFER, g->params_ubo));
	uniforms.push_back(tressfx_make_uniform(10, RenderingDevice::UNIFORM_TYPE_IMAGE, g->image));
	g->uniform_set = rd->uniform_set_create(uniforms, TressFXPipelines::get()->simulation.shader, 0);

	// Free order: the uniform set before the buffers it references.
	g->rids.push_back(g->uniform_set);
	g->rids.push_back(g->image);
	g->rids.push_back(g->params_ubo);
	for (int i = 0; i < 9; i++) {
		g->rids.push_back(buffers[i]);
	}
}

// Uniform sets for colliding with one mesh, cached per SDF buffer. The sets die with the buffers
// they reference (the collider reloaded), so a stale entry is simply recreated.
const Pair<RID, RID> &TressFXHairGPU::_collision_sets_for(TressFXCollisionGPU *p_collider) {
	const uint64_t key = p_collider->sdf_buffer.get_id();
	Pair<RID, RID> *sets = collision_sets.getptr(key);
	if (sets != nullptr && rd->uniform_set_is_valid(sets->first) && rd->uniform_set_is_valid(sets->second)) {
		return *sets;
	}
	TypedArray<RDUniform> set0;
	set0.push_back(tressfx_make_uniform(0, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, p_collider->sdf_buffer));
	set0.push_back(tressfx_make_uniform(1, RenderingDevice::UNIFORM_TYPE_UNIFORM_BUFFER, p_collider->params_ubo));
	TypedArray<RDUniform> set1;
	set1.push_back(tressfx_make_uniform(0, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, positions_buffer));
	set1.push_back(tressfx_make_uniform(1, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, previous_buffer));
	const RID shader = TressFXPipelines::get()->collision.shader;
	return collision_sets.insert(key, Pair<RID, RID>(rd->uniform_set_create(set0, shader, 0), rd->uniform_set_create(set1, shader, 1)))->value;
}

// Upstream Simulation::RunSimulation + RunCollision: one compute list per hair object.
void TressFXHairGPU::simulate(int64_t p_self, const PackedByteArray &p_params, const PackedByteArray &p_bones, const PackedInt64Array &p_colliders) {
	TressFXHairGPU *g = (TressFXHairGPU *)p_self;
	if (!g->uniform_set.is_valid() || p_params.size() != PARAMS_SIZE) {
		return;
	}
	RenderingDevice *rd = g->rd;
	rd->buffer_update(g->params_ubo, 0, PARAMS_SIZE, p_params);
	rd->buffer_update(g->bones_buffer, 0, p_bones.size(), p_bones);

	const int32_t *sim_ints = (const int32_t *)(p_params.ptr() + PARAMS_SIM_INTS_OFFSET);
	const int local_iterations = sim_ints[1];
	const int skip_root_vertices = sim_ints[2];
	const int n = g->num_verts_per_strand;
	const int vertex_groups = g->num_guide_strands * n / TRESSFX_THREAD_GROUP_SIZE;
	const int strand_groups = g->num_guide_strands / TRESSFX_THREAD_GROUP_SIZE;
	const int total_groups = (g->num_total_vertices + TRESSFX_THREAD_GROUP_SIZE - 1) / TRESSFX_THREAD_GROUP_SIZE;

	const String timestamp = tressfx_gpu_timing ? "tfx_sim_" + String::num_uint64((uint64_t)p_self) : String();
	if (tressfx_gpu_timing) {
		rd->capture_timestamp(timestamp + String(":begin"));
	}
	const int64_t list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, TressFXPipelines::get()->simulation.pipeline);
	rd->compute_list_bind_uniform_set(list, g->uniform_set, 0);
	tressfx_dispatch(rd, list, 0, vertex_groups); // Integration and global shape constraints.
	tressfx_dispatch(rd, list, 1, strand_groups); // Strand level data.
	tressfx_dispatch(rd, list, 2, vertex_groups); // Velocity shock propagation.
	for (int i = 0; i < local_iterations; i++) {
		tressfx_dispatch(rd, list, 3, strand_groups); // Local shape constraints.
	}
	tressfx_dispatch(rd, list, 4, vertex_groups); // Length constraints and wind.

	// SDF collision on the guide strands, every mesh (upstream applies all to all too).
	bool collided = false;
	for (int i = 0; i < p_colliders.size(); i++) {
		TressFXCollisionGPU *collider = (TressFXCollisionGPU *)p_colliders[i];
		if (collider == nullptr || !collider->sdf_buffer.is_valid()) {
			continue;
		}
		const Pair<RID, RID> &sets = g->_collision_sets_for(collider);
		rd->compute_list_bind_compute_pipeline(list, TressFXPipelines::get()->collision.pipeline);
		rd->compute_list_bind_uniform_set(list, sets.first, 0);
		rd->compute_list_bind_uniform_set(list, sets.second, 1);
		const int32_t push_constant[4] = { g->num_guide_strands * n, n, g->num_follow_per_guide, skip_root_vertices };
		PackedByteArray bytes;
		bytes.resize(sizeof(push_constant));
		memcpy(bytes.ptrw(), push_constant, sizeof(push_constant));
		rd->compute_list_set_push_constant(list, bytes, sizeof(push_constant));
		rd->compute_list_dispatch(list, vertex_groups, 1, 1);
		rd->compute_list_add_barrier(list);
		collided = true;
	}
	if (collided) {
		rd->compute_list_bind_compute_pipeline(list, TressFXPipelines::get()->simulation.pipeline);
		rd->compute_list_bind_uniform_set(list, g->uniform_set, 0);
	}

	tressfx_dispatch(rd, list, 5, vertex_groups); // Follow hairs from the (collided) guides.
	tressfx_dispatch(rd, list, 6, total_groups); // Positions and tangents into the render texture.
	rd->compute_list_end();
	if (tressfx_gpu_timing) {
		rd->capture_timestamp(timestamp + String(":end"));
	}
}

void TressFXHairGPU::capture(int64_t p_self) {
	TressFXHairGPU *g = (TressFXHairGPU *)p_self;
	if (g->positions_buffer.is_valid()) {
		g->captured_positions = g->rd->buffer_get_data(g->positions_buffer).to_float32_array();
	}
}

void TressFXHairGPU::destroy(int64_t p_self) {
	TressFXHairGPU *g = (TressFXHairGPU *)p_self;
	if (g->rd != nullptr) {
		for (const KeyValue<uint64_t, Pair<RID, RID>> &kv : g->collision_sets) {
			if (g->rd->uniform_set_is_valid(kv.value.first)) {
				g->rd->free_rid(kv.value.first);
			}
			if (g->rd->uniform_set_is_valid(kv.value.second)) {
				g->rd->free_rid(kv.value.second);
			}
		}
		tressfx_free_rids(g->rd, g->rids);
		TressFXPipelines::release(g->rd);
	}
	memdelete(g);
}

// ---------------------------------------------------------------------------------------------
// Main thread

TressFXHair::TressFXHair() {
	texture.instantiate();
}

void TressFXHair::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY: {
			// Run after AnimationPlayer and TressFXCollisionMesh (90) so this frame's poses and SDFs
			// are in. Runtime only, so the editor does not save the priority into the scene.
			if (!Engine::get_singleton()->is_editor_hint()) {
				set_process_priority(100);
			}
			add_to_group("tressfx_hair");
			set_process(true);
			_load();
		} break;
		case NOTIFICATION_ENTER_TREE: {
			// Re-added to the tree (reparenting): the GPU side was freed on exit, rebuild it.
			if (loaded && gpu == nullptr) {
				frame = 0;
				_init_gpu();
			}
		} break;
		case NOTIFICATION_EXIT_TREE:
		case NOTIFICATION_PREDELETE: {
			_unload_gpu();
		} break;
		case NOTIFICATION_PROCESS: {
			if (Engine::get_singleton()->is_editor_hint()) {
				_reload_if_files_changed();
			}
			_step(get_process_delta_time());
		} break;
	}
}

void TressFXHair::_step(double p_delta) {
	if (!loaded || gpu == nullptr) {
		return;
	}
	const uint64_t t0 = Time::get_singleton()->get_ticks_usec();
	if (!texture_bound && gpu->image.is_valid()) {
		texture->set_texture_rd_rid(gpu->image);
		texture_bound = true;
	}
	const bool editor = Engine::get_singleton()->is_editor_hint();
	Skeleton3D *skeleton = skin.get_skeleton();
	const Transform3D anchor = skeleton != nullptr ? skeleton->get_global_transform() : get_global_transform();
	const Vector3 center_world = anchor.xform(bound_center);
	const Vector3 center = to_local(center_world);

	// Hidden, off screen or far away: nothing to simulate for. After a long sleep the character
	// may be somewhere else entirely, so snap to the rest pose instead of flying there.
	if (!_should_simulate(center_world)) {
		skipped_frames++;
		last_cpu_usec = Time::get_singleton()->get_ticks_usec() - t0;
		return;
	}
	if (skipped_frames > 60) {
		frame = 0;
	}
	skipped_frames = 0;

	skin.update(get_global_transform());
	const PackedByteArray bones = skin.pack();
	const PackedByteArray params = _pack_params(MIN((float)p_delta, 0.05f));

	// In the editor, sleep once the pose and the settings have not changed for a while; nothing
	// would redraw the settled hair anyway. Any change wakes it up.
	if (editor) {
		idle_frames = _inputs_changed(params, bones) ? 0 : idle_frames + 1;
		if (idle_frames > EDITOR_IDLE_FRAMES) {
			last_cpu_usec = Time::get_singleton()->get_ticks_usec() - t0;
			return;
		}
	}

	// Culling bounds follow the skeleton. In the editor they are set every active frame, which
	// also asks the viewport to redraw the moving hair.
	const float r = bound_radius;
	if (editor || center.distance_squared_to(last_center) > 0.0025f * r * r) {
		const AABB aabb(center - Vector3(r, r, r), Vector3(2.0f * r, 2.0f * r, 2.0f * r));
		for (MeshInstance3D *mi : mesh_instances) {
			mi->set_custom_aabb(aabb);
		}
		last_center = center;
	}

	PackedInt64Array colliders;
	for (int i = 0; i < collision_meshes.size(); i++) {
		// get_validated_object: a collider freed with queue_free() may still be listed here.
		TressFXCollisionMesh *mesh = Object::cast_to<TressFXCollisionMesh>(collision_meshes[i].get_validated_object());
		if (mesh != nullptr && mesh->_get_gpu() != nullptr) {
			mesh->_mark_used();
			colliders.push_back((int64_t)mesh->_get_gpu());
		}
	}
	RenderingServer::get_singleton()->call_on_render_thread(
			callable_mp_static(&TressFXHairGPU::simulate).bind((int64_t)gpu, params, bones, colliders));
	frame++;
	last_cpu_usec = Time::get_singleton()->get_ticks_usec() - t0;
}

bool TressFXHair::_should_simulate(const Vector3 &p_center) const {
	if (!simulate_offscreen && !is_visible_in_tree()) {
		return false;
	}
	if (simulate_offscreen && simulation_distance <= 0.0f) {
		return true;
	}
	Viewport *viewport = get_viewport();
	Camera3D *camera = viewport != nullptr ? viewport->get_camera_3d() : nullptr;
	if (camera == nullptr) {
		return true;
	}
	const float r = bound_radius;
	if (simulation_distance > 0.0f && camera->get_global_position().distance_to(p_center) - r > simulation_distance) {
		return false;
	}
	if (!simulate_offscreen) {
		// Bounding sphere against the frustum; the planes face outwards.
		const TypedArray<Plane> frustum = camera->get_frustum();
		for (int i = 0; i < frustum.size(); i++) {
			const Plane plane = frustum[i];
			if (plane.distance_to(p_center) > r) {
				return false;
			}
		}
	}
	return true;
}

bool TressFXHair::_inputs_changed(const PackedByteArray &p_params, const PackedByteArray &p_bones) {
	const bool changed = p_bones != last_bones || p_params.size() != last_params.size() ||
			memcmp(p_params.ptr(), last_params.ptr(), PARAMS_DT_OFFSET) != 0 ||
			memcmp(p_params.ptr() + PARAMS_DT_OFFSET + 4, last_params.ptr() + PARAMS_DT_OFFSET + 4, PARAMS_SIZE - PARAMS_DT_OFFSET - 4) != 0;
	last_params = p_params;
	last_bones = p_bones;
	return changed;
}

void TressFXHair::_load() {
	if (tfx_path.is_empty()) {
		return;
	}
	const uint64_t t = Time::get_singleton()->get_ticks_msec();
	file_stamp = _file_stamp();
	pending_file_stamp = file_stamp;
	loaded = asset.load(tfx_path, num_follow_hairs, tip_separation, import_scale, follow_radius);
	if (!loaded) {
		return;
	}
	skin.init(get_hair_skeleton());
	if (!tfxbone_path.is_empty() && skin.get_skeleton() != nullptr) {
		asset.load_bone_data(tfxbone_path, skin);
	}
	bound_radius = asset.rest_bounds(bound_center) * 1.3f + 0.5f; // Slack for the animation.
	while ((int)mesh_instances.size() < LOD_BUCKETS) {
		MeshInstance3D *mi = memnew(MeshInstance3D);
		add_child(mi, false, Node::INTERNAL_MODE_FRONT);
		mesh_instances.push_back(mi);
	}
	_make_meshes();
	for (MeshInstance3D *mi : mesh_instances) {
		mi->set_cast_shadows_setting(_shadow_setting());
	}
	_apply_material();
	_apply_lod();
	frame = 0;
	_init_gpu();
	UtilityFunctions::print_verbose("TressFX: ", tfx_path.get_file(), " - ", asset.num_total_strands, " strands (",
			asset.num_guide_strands, " guide), ", asset.num_total_vertices, " verts, ", skin.bone_count, " bones, ",
			Time::get_singleton()->get_ticks_msec() - t, " ms");
}

// Inspector changes that need the asset rebuilt (only once the node is up).
void TressFXHair::_reload() {
	if (!is_node_ready()) {
		return;
	}
	_unload_gpu();
	loaded = false;
	for (MeshInstance3D *mi : mesh_instances) {
		mi->set_mesh(Ref<Mesh>());
	}
	_load();
}

uint64_t TressFXHair::_file_stamp() const {
	uint64_t stamp = FileAccess::file_exists(tfx_path) ? FileAccess::get_modified_time(tfx_path) : 0;
	if (FileAccess::file_exists(tfxbone_path)) {
		stamp = stamp * 31 + FileAccess::get_modified_time(tfxbone_path);
	}
	return stamp;
}

// The .tfx and .tfxbone are not imported resources, so nothing tells the editor when the Blender
// add-on writes them again. Check once a second and reload once the files have stopped changing
// (the exporter writes the two files one after the other).
void TressFXHair::_reload_if_files_changed() {
	const uint64_t now = Time::get_singleton()->get_ticks_msec();
	if (tfx_path.is_empty() || now - last_file_check_msec < 1000) {
		return;
	}
	last_file_check_msec = now;
	const uint64_t stamp = _file_stamp();
	if (stamp == file_stamp) {
		return;
	}
	if (stamp == pending_file_stamp) {
		_reload();
	} else {
		pending_file_stamp = stamp;
	}
}

void TressFXHair::_init_gpu() {
	TressFXHairGPU *g = memnew(TressFXHairGPU);
	g->sim_spirv = _load_spirv(SIM_SHADER_PATH);
	g->collide_spirv = _load_spirv(COLLIDE_SHADER_PATH);
	g->positions = asset.positions.to_byte_array();
	g->rest_lengths = asset.rest_lengths.to_byte_array();
	g->follow_root_offsets = asset.follow_root_offsets.to_byte_array();
	g->bone_skinning = asset.bone_skinning.to_byte_array();
	g->num_guide_strands = asset.num_guide_strands;
	g->num_total_strands = asset.num_total_strands;
	g->num_verts_per_strand = asset.num_verts_per_strand;
	g->num_total_vertices = asset.num_total_vertices;
	g->num_follow_per_guide = asset.num_follow_per_guide;
	gpu = g;
	texture_bound = false;
	idle_frames = 0;
	skipped_frames = 0;
	last_params = PackedByteArray();
	last_bones = PackedByteArray();
	RenderingServer::get_singleton()->call_on_render_thread(callable_mp_static(&TressFXHairGPU::init).bind((int64_t)g));
}

void TressFXHair::_unload_gpu() {
	// The RenderingServer texture wrapping our image must go before the image is freed; both
	// travel through the same command queue, so the order here is the order on the GPU.
	if (texture_bound) {
		texture->set_texture_rd_rid(RID());
		texture_bound = false;
	}
	if (gpu != nullptr) {
		RenderingServer::get_singleton()->call_on_render_thread(callable_mp_static(&TressFXHairGPU::destroy).bind((int64_t)gpu));
		gpu = nullptr;
	}
}

void TressFXHair::_apply_material() {
	active_material = material;
	if (active_material.is_null()) {
		active_material.instantiate();
	}
	if (active_material->get_shader().is_null()) {
		active_material->set_shader(ResourceLoader::get_singleton()->load(STRAND_SHADER_PATH));
	}
	active_material->set_shader_parameter("positions", texture);
	active_material->set_shader_parameter("strand_uvs", strand_uv_texture);
	active_material->set_shader_parameter("verts_per_strand", asset.num_verts_per_strand);
	for (MeshInstance3D *mi : mesh_instances) {
		mi->set_material_override(active_material);
	}
}

// Bucket 0 is always drawn; the others fade out one by one between lod_start and lod_end so that
// lod_percent of the strands remain. The width multiplier is applied in the shader.
void TressFXHair::_apply_lod() {
	if (mesh_instances.is_empty()) {
		return;
	}
	const int keep = MAX(1, (int)Math::round(lod_percent * LOD_BUCKETS));
	for (int i = 0; i < LOD_BUCKETS; i++) {
		MeshInstance3D *mi = mesh_instances[i];
		if (!lod_enabled || i < keep) {
			mi->set_visibility_range_end(0.0f);
			mi->set_visibility_range_fade_mode(GeometryInstance3D::VISIBILITY_RANGE_FADE_DISABLED);
			continue;
		}
		const float t = (float)(LOD_BUCKETS - i) / (float)(LOD_BUCKETS - keep); // Last bucket goes first.
		mi->set_visibility_range_end(Math::lerp(lod_start_distance, lod_end_distance, t));
		mi->set_visibility_range_end_margin((lod_end_distance - lod_start_distance) * 0.25f);
		mi->set_visibility_range_fade_mode(GeometryInstance3D::VISIBILITY_RANGE_FADE_SELF);
	}
	if (active_material.is_valid()) {
		active_material->set_shader_parameter("lod_start", lod_start_distance);
		active_material->set_shader_parameter("lod_end", lod_end_distance);
		active_material->set_shader_parameter("lod_width_multiplier", lod_enabled ? lod_width_multiplier : 1.0f);
	}
}

GeometryInstance3D::ShadowCastingSetting TressFXHair::_shadow_setting() const {
	return cast_hair_shadows ? GeometryInstance3D::SHADOW_CASTING_SETTING_ON : GeometryInstance3D::SHADOW_CASTING_SETTING_OFF;
}

// Two ribbon vertices per hair vertex (left/right), quads between consecutive ones. The mesh
// carries no attributes at all: the strand shader derives the hair vertex, the side and the strand
// from VERTEX_ID and the per-instance `lod_bucket`. Strand s goes to LOD bucket s % LOD_BUCKETS,
// so every bucket is a uniform subset. Strand texture coordinates travel in a small texture.
void TressFXHair::_make_meshes() {
	const int n = asset.num_verts_per_strand;
	for (int b = 0; b < LOD_BUCKETS; b++) {
		const int strands = (asset.num_total_strands - b + LOD_BUCKETS - 1) / LOD_BUCKETS;
		PackedVector3Array verts;
		verts.resize(strands * n * 2); // Zeros; the real positions come from the texture.
		PackedInt32Array index;
		index.resize(strands * (n - 1) * 6);
		int32_t *iw = index.ptrw();
		int c = 0;
		for (int k = 0; k < strands; k++) {
			for (int j = 0; j < n - 1; j++) {
				const int lv = (k * n + j) * 2; // Local ribbon vertex pair.
				// Same index pattern as upstream FillTriangleIndexArray.
				iw[c] = lv;
				iw[c + 1] = lv + 1;
				iw[c + 2] = lv + 2;
				iw[c + 3] = lv + 2;
				iw[c + 4] = lv + 1;
				iw[c + 5] = lv + 3;
				c += 6;
			}
		}
		Array arrays;
		arrays.resize(Mesh::ARRAY_MAX);
		arrays[Mesh::ARRAY_VERTEX] = verts;
		arrays[Mesh::ARRAY_INDEX] = index;
		Ref<ArrayMesh> mesh;
		mesh.instantiate();
		mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
		mesh_instances[b]->set_mesh(mesh);
		mesh_instances[b]->set_instance_shader_parameter("lod_bucket", b);
	}

	// One RG32F texel per strand, wrapped like the positions texture.
	const int strands = asset.num_total_strands;
	const int w = MIN(strands, TRESSFX_TEXTURE_WIDTH);
	const int h = (strands + w - 1) / w;
	PackedByteArray uv_data;
	uv_data.resize(w * h * 2 * sizeof(float));
	uv_data.fill(0);
	memcpy(uv_data.ptrw(), asset.strand_uv.ptr(), (size_t)strands * 2 * sizeof(float));
	strand_uv_texture = ImageTexture::create_from_image(Image::create_from_data(w, h, false, Image::FORMAT_RGF, uv_data));
}

// Matches the std140 `Params` block in tressfx_sim.glsl.
PackedByteArray TressFXHair::_pack_params(float p_delta) const {
	PackedByteArray out;
	out.resize(PARAMS_SIZE);
	float *f = (float *)out.ptrw();
	int32_t *i = (int32_t *)out.ptrw();
	const int n = asset.num_verts_per_strand;
	_wind_corners(f); // wind0..wind3
	f[16] = damping;
	f[17] = local_stiffness;
	f[18] = global_stiffness;
	f[19] = global_range;
	f[20] = gravity;
	f[21] = p_delta;
	f[22] = tip_separation;
	f[23] = 0.0f;
	i[24] = length_iterations;
	i[25] = local_iterations;
	i[26] = collision_skip_root_vertices;
	i[27] = 0;
	i[28] = TRESSFX_THREAD_GROUP_SIZE / n;
	i[29] = asset.num_follow_per_guide;
	i[30] = n;
	i[31] = asset.num_total_vertices;
	f[32] = vsp_coeff;
	f[33] = vsp_accel_threshold;
	f[34] = 0.0f;
	f[35] = 0.0f;
	f[36] = frame < 2 ? 1.0f : 0.0f; // Upstream resets the positions on the first two frames.
	f[37] = clamp_position_delta;
	f[38] = 0.0f;
	f[39] = 0.0f;
	return out;
}

// Upstream TressFXHairObject::SetWind: four vectors on a 40 degree cone around the wind direction,
// gusting between 0.5x and 1.5x (upstream ties the gust to the frame count; this uses time).
void TressFXHair::_wind_corners(float *r_out) const {
	const float gust = Math::pow(Math::sin(Time::get_singleton()->get_ticks_msec() * 0.0006), 2.0) + 0.5;
	const float magnitude = wind_magnitude * gust;
	const Vector3 dir = wind_direction.normalized();
	const Vector3 x_cross_w = Vector3(1, 0, 0).cross(dir);
	Quaternion rot;
	const float angle = Math::asin(CLAMP(x_cross_w.length(), -1.0f, 1.0f));
	if (angle > 0.001f) {
		rot = Quaternion(x_cross_w.normalized(), angle);
	}
	const Vector3 axes[4] = { Vector3(0, 1, 0), Vector3(0, -1, 0), Vector3(0, 0, 1), Vector3(0, 0, -1) };
	for (int k = 0; k < 4; k++) {
		const Vector3 w = (rot * Quaternion(axes[k], Math::deg_to_rad(40.0f))).xform(Vector3(1, 0, 0)) * magnitude;
		r_out[k * 4] = w.x;
		r_out[k * 4 + 1] = w.y;
		r_out[k * 4 + 2] = w.z;
		r_out[k * 4 + 3] = 0.0f;
	}
}

// ---------------------------------------------------------------------------------------------
// Public API

void TressFXHair::reset_positions() {
	frame = 0;
}

int TressFXHair::get_strand_count() const {
	return loaded ? asset.num_total_strands : 0;
}

int TressFXHair::get_guide_strand_count() const {
	return loaded ? asset.num_guide_strands : 0;
}

int TressFXHair::get_vertex_count() const {
	return loaded ? asset.num_total_vertices : 0;
}

int TressFXHair::get_vertices_per_strand() const {
	return loaded ? asset.num_verts_per_strand : 0;
}

int TressFXHair::get_cpu_time_usec() const {
	return (int)last_cpu_usec;
}

void TressFXHair::capture_positions() {
	if (gpu != nullptr) {
		RenderingServer::get_singleton()->call_on_render_thread(callable_mp_static(&TressFXHairGPU::capture).bind((int64_t)gpu));
	}
}

PackedFloat32Array TressFXHair::get_captured_positions() const {
	return gpu != nullptr ? gpu->captured_positions : PackedFloat32Array();
}

// ---------------------------------------------------------------------------------------------
// Properties

void TressFXHair::set_tfx_path(const String &p_path) {
	tfx_path = p_path;
	_reload();
}

String TressFXHair::get_tfx_path() const {
	return tfx_path;
}

void TressFXHair::set_tfxbone_path(const String &p_path) {
	tfxbone_path = p_path;
	_reload();
}

String TressFXHair::get_tfxbone_path() const {
	return tfxbone_path;
}

void TressFXHair::set_hair_skeleton(Node *p_skeleton) {
	Skeleton3D *skeleton = Object::cast_to<Skeleton3D>(p_skeleton);
	skeleton_id = skeleton != nullptr ? skeleton->get_instance_id() : 0;
	_reload();
}

Skeleton3D *TressFXHair::get_hair_skeleton() const {
	return skeleton_id != 0 ? Object::cast_to<Skeleton3D>(ObjectDB::get_instance(skeleton_id)) : nullptr;
}

void TressFXHair::set_num_follow_hairs(int p_count) {
	num_follow_hairs = MAX(0, p_count);
	_reload();
}

int TressFXHair::get_num_follow_hairs() const {
	return num_follow_hairs;
}

void TressFXHair::set_tip_separation(float p_separation) {
	tip_separation = p_separation;
}

float TressFXHair::get_tip_separation() const {
	return tip_separation;
}

void TressFXHair::set_follow_radius(float p_radius) {
	follow_radius = p_radius;
	_reload();
}

float TressFXHair::get_follow_radius() const {
	return follow_radius;
}

void TressFXHair::set_import_scale(float p_scale) {
	import_scale = p_scale;
	_reload();
}

float TressFXHair::get_import_scale() const {
	return import_scale;
}

void TressFXHair::set_material(const Ref<ShaderMaterial> &p_material) {
	material = p_material;
	if (loaded) {
		_apply_material();
		_apply_lod();
	}
}

Ref<ShaderMaterial> TressFXHair::get_material() const {
	return material;
}

void TressFXHair::set_cast_hair_shadows(bool p_enabled) {
	cast_hair_shadows = p_enabled;
	for (MeshInstance3D *mi : mesh_instances) {
		mi->set_cast_shadows_setting(_shadow_setting());
	}
}

bool TressFXHair::get_cast_hair_shadows() const {
	return cast_hair_shadows;
}

void TressFXHair::set_lod_enabled(bool p_enabled) {
	lod_enabled = p_enabled;
	_apply_lod();
}

bool TressFXHair::get_lod_enabled() const {
	return lod_enabled;
}

void TressFXHair::set_lod_start_distance(float p_distance) {
	lod_start_distance = p_distance;
	_apply_lod();
}

float TressFXHair::get_lod_start_distance() const {
	return lod_start_distance;
}

void TressFXHair::set_lod_end_distance(float p_distance) {
	lod_end_distance = p_distance;
	_apply_lod();
}

float TressFXHair::get_lod_end_distance() const {
	return lod_end_distance;
}

void TressFXHair::set_lod_percent(float p_percent) {
	lod_percent = CLAMP(p_percent, 0.0f, 1.0f);
	_apply_lod();
}

float TressFXHair::get_lod_percent() const {
	return lod_percent;
}

void TressFXHair::set_lod_width_multiplier(float p_multiplier) {
	lod_width_multiplier = p_multiplier;
	_apply_lod();
}

float TressFXHair::get_lod_width_multiplier() const {
	return lod_width_multiplier;
}

void TressFXHair::set_collision_meshes(const TypedArray<TressFXCollisionMesh> &p_meshes) {
	collision_meshes = p_meshes;
}

TypedArray<TressFXCollisionMesh> TressFXHair::get_collision_meshes() const {
	return collision_meshes;
}

void TressFXHair::set_collision_skip_root_vertices(int p_count) {
	collision_skip_root_vertices = MAX(0, p_count);
}

int TressFXHair::get_collision_skip_root_vertices() const {
	return collision_skip_root_vertices;
}

void TressFXHair::set_vsp_coeff(float p_value) {
	vsp_coeff = p_value;
}

float TressFXHair::get_vsp_coeff() const {
	return vsp_coeff;
}

void TressFXHair::set_vsp_accel_threshold(float p_value) {
	vsp_accel_threshold = p_value;
}

float TressFXHair::get_vsp_accel_threshold() const {
	return vsp_accel_threshold;
}

void TressFXHair::set_local_stiffness(float p_value) {
	local_stiffness = p_value;
}

float TressFXHair::get_local_stiffness() const {
	return local_stiffness;
}

void TressFXHair::set_local_iterations(int p_value) {
	local_iterations = CLAMP(p_value, 0, 32);
}

int TressFXHair::get_local_iterations() const {
	return local_iterations;
}

void TressFXHair::set_global_stiffness(float p_value) {
	global_stiffness = p_value;
}

float TressFXHair::get_global_stiffness() const {
	return global_stiffness;
}

void TressFXHair::set_global_range(float p_value) {
	global_range = p_value;
}

float TressFXHair::get_global_range() const {
	return global_range;
}

void TressFXHair::set_length_iterations(int p_value) {
	length_iterations = CLAMP(p_value, 0, 32);
}

int TressFXHair::get_length_iterations() const {
	return length_iterations;
}

void TressFXHair::set_damping(float p_value) {
	damping = p_value;
}

float TressFXHair::get_damping() const {
	return damping;
}

void TressFXHair::set_gravity(float p_value) {
	gravity = p_value;
}

float TressFXHair::get_gravity() const {
	return gravity;
}

void TressFXHair::set_wind_direction(const Vector3 &p_direction) {
	wind_direction = p_direction;
}

Vector3 TressFXHair::get_wind_direction() const {
	return wind_direction;
}

void TressFXHair::set_wind_magnitude(float p_value) {
	wind_magnitude = p_value;
}

float TressFXHair::get_wind_magnitude() const {
	return wind_magnitude;
}

void TressFXHair::set_clamp_position_delta(float p_value) {
	clamp_position_delta = p_value;
}

float TressFXHair::get_clamp_position_delta() const {
	return clamp_position_delta;
}

void TressFXHair::set_simulation_distance(float p_distance) {
	simulation_distance = MAX(0.0f, p_distance);
}

float TressFXHair::get_simulation_distance() const {
	return simulation_distance;
}

void TressFXHair::set_simulate_offscreen(bool p_enabled) {
	simulate_offscreen = p_enabled;
}

bool TressFXHair::get_simulate_offscreen() const {
	return simulate_offscreen;
}

void TressFXHair::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_tfx_path", "path"), &TressFXHair::set_tfx_path);
	ClassDB::bind_method(D_METHOD("get_tfx_path"), &TressFXHair::get_tfx_path);
	ClassDB::bind_method(D_METHOD("set_tfxbone_path", "path"), &TressFXHair::set_tfxbone_path);
	ClassDB::bind_method(D_METHOD("get_tfxbone_path"), &TressFXHair::get_tfxbone_path);
	ClassDB::bind_method(D_METHOD("set_hair_skeleton", "skeleton"), &TressFXHair::set_hair_skeleton);
	ClassDB::bind_method(D_METHOD("get_hair_skeleton"), &TressFXHair::get_hair_skeleton);
	ClassDB::bind_method(D_METHOD("set_num_follow_hairs", "count"), &TressFXHair::set_num_follow_hairs);
	ClassDB::bind_method(D_METHOD("get_num_follow_hairs"), &TressFXHair::get_num_follow_hairs);
	ClassDB::bind_method(D_METHOD("set_tip_separation", "separation"), &TressFXHair::set_tip_separation);
	ClassDB::bind_method(D_METHOD("get_tip_separation"), &TressFXHair::get_tip_separation);
	ClassDB::bind_method(D_METHOD("set_follow_radius", "radius"), &TressFXHair::set_follow_radius);
	ClassDB::bind_method(D_METHOD("get_follow_radius"), &TressFXHair::get_follow_radius);
	ClassDB::bind_method(D_METHOD("set_import_scale", "scale"), &TressFXHair::set_import_scale);
	ClassDB::bind_method(D_METHOD("get_import_scale"), &TressFXHair::get_import_scale);
	ClassDB::bind_method(D_METHOD("set_material", "material"), &TressFXHair::set_material);
	ClassDB::bind_method(D_METHOD("get_material"), &TressFXHair::get_material);
	ClassDB::bind_method(D_METHOD("set_cast_hair_shadows", "enabled"), &TressFXHair::set_cast_hair_shadows);
	ClassDB::bind_method(D_METHOD("get_cast_hair_shadows"), &TressFXHair::get_cast_hair_shadows);

	ClassDB::bind_method(D_METHOD("set_lod_enabled", "enabled"), &TressFXHair::set_lod_enabled);
	ClassDB::bind_method(D_METHOD("get_lod_enabled"), &TressFXHair::get_lod_enabled);
	ClassDB::bind_method(D_METHOD("set_lod_start_distance", "distance"), &TressFXHair::set_lod_start_distance);
	ClassDB::bind_method(D_METHOD("get_lod_start_distance"), &TressFXHair::get_lod_start_distance);
	ClassDB::bind_method(D_METHOD("set_lod_end_distance", "distance"), &TressFXHair::set_lod_end_distance);
	ClassDB::bind_method(D_METHOD("get_lod_end_distance"), &TressFXHair::get_lod_end_distance);
	ClassDB::bind_method(D_METHOD("set_lod_percent", "percent"), &TressFXHair::set_lod_percent);
	ClassDB::bind_method(D_METHOD("get_lod_percent"), &TressFXHair::get_lod_percent);
	ClassDB::bind_method(D_METHOD("set_lod_width_multiplier", "multiplier"), &TressFXHair::set_lod_width_multiplier);
	ClassDB::bind_method(D_METHOD("get_lod_width_multiplier"), &TressFXHair::get_lod_width_multiplier);

	ClassDB::bind_method(D_METHOD("set_collision_meshes", "meshes"), &TressFXHair::set_collision_meshes);
	ClassDB::bind_method(D_METHOD("get_collision_meshes"), &TressFXHair::get_collision_meshes);
	ClassDB::bind_method(D_METHOD("set_collision_skip_root_vertices", "count"), &TressFXHair::set_collision_skip_root_vertices);
	ClassDB::bind_method(D_METHOD("get_collision_skip_root_vertices"), &TressFXHair::get_collision_skip_root_vertices);

	ClassDB::bind_method(D_METHOD("set_vsp_coeff", "value"), &TressFXHair::set_vsp_coeff);
	ClassDB::bind_method(D_METHOD("get_vsp_coeff"), &TressFXHair::get_vsp_coeff);
	ClassDB::bind_method(D_METHOD("set_vsp_accel_threshold", "value"), &TressFXHair::set_vsp_accel_threshold);
	ClassDB::bind_method(D_METHOD("get_vsp_accel_threshold"), &TressFXHair::get_vsp_accel_threshold);
	ClassDB::bind_method(D_METHOD("set_local_stiffness", "value"), &TressFXHair::set_local_stiffness);
	ClassDB::bind_method(D_METHOD("get_local_stiffness"), &TressFXHair::get_local_stiffness);
	ClassDB::bind_method(D_METHOD("set_local_iterations", "value"), &TressFXHair::set_local_iterations);
	ClassDB::bind_method(D_METHOD("get_local_iterations"), &TressFXHair::get_local_iterations);
	ClassDB::bind_method(D_METHOD("set_global_stiffness", "value"), &TressFXHair::set_global_stiffness);
	ClassDB::bind_method(D_METHOD("get_global_stiffness"), &TressFXHair::get_global_stiffness);
	ClassDB::bind_method(D_METHOD("set_global_range", "value"), &TressFXHair::set_global_range);
	ClassDB::bind_method(D_METHOD("get_global_range"), &TressFXHair::get_global_range);
	ClassDB::bind_method(D_METHOD("set_length_iterations", "value"), &TressFXHair::set_length_iterations);
	ClassDB::bind_method(D_METHOD("get_length_iterations"), &TressFXHair::get_length_iterations);
	ClassDB::bind_method(D_METHOD("set_damping", "value"), &TressFXHair::set_damping);
	ClassDB::bind_method(D_METHOD("get_damping"), &TressFXHair::get_damping);
	ClassDB::bind_method(D_METHOD("set_gravity", "value"), &TressFXHair::set_gravity);
	ClassDB::bind_method(D_METHOD("get_gravity"), &TressFXHair::get_gravity);
	ClassDB::bind_method(D_METHOD("set_wind_direction", "direction"), &TressFXHair::set_wind_direction);
	ClassDB::bind_method(D_METHOD("get_wind_direction"), &TressFXHair::get_wind_direction);
	ClassDB::bind_method(D_METHOD("set_wind_magnitude", "value"), &TressFXHair::set_wind_magnitude);
	ClassDB::bind_method(D_METHOD("get_wind_magnitude"), &TressFXHair::get_wind_magnitude);
	ClassDB::bind_method(D_METHOD("set_clamp_position_delta", "value"), &TressFXHair::set_clamp_position_delta);
	ClassDB::bind_method(D_METHOD("get_clamp_position_delta"), &TressFXHair::get_clamp_position_delta);
	ClassDB::bind_method(D_METHOD("set_simulation_distance", "distance"), &TressFXHair::set_simulation_distance);
	ClassDB::bind_method(D_METHOD("get_simulation_distance"), &TressFXHair::get_simulation_distance);
	ClassDB::bind_method(D_METHOD("set_simulate_offscreen", "enabled"), &TressFXHair::set_simulate_offscreen);
	ClassDB::bind_method(D_METHOD("get_simulate_offscreen"), &TressFXHair::get_simulate_offscreen);

	ClassDB::bind_method(D_METHOD("reset_positions"), &TressFXHair::reset_positions);
	ClassDB::bind_method(D_METHOD("get_strand_count"), &TressFXHair::get_strand_count);
	ClassDB::bind_method(D_METHOD("get_guide_strand_count"), &TressFXHair::get_guide_strand_count);
	ClassDB::bind_method(D_METHOD("get_vertex_count"), &TressFXHair::get_vertex_count);
	ClassDB::bind_method(D_METHOD("get_vertices_per_strand"), &TressFXHair::get_vertices_per_strand);
	ClassDB::bind_method(D_METHOD("get_cpu_time_usec"), &TressFXHair::get_cpu_time_usec);
	ClassDB::bind_method(D_METHOD("capture_positions"), &TressFXHair::capture_positions);
	ClassDB::bind_method(D_METHOD("get_captured_positions"), &TressFXHair::get_captured_positions);

	ADD_PROPERTY(PropertyInfo(Variant::STRING, "tfx_path", PROPERTY_HINT_FILE, "*.tfx"), "set_tfx_path", "get_tfx_path");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "tfxbone_path", PROPERTY_HINT_FILE, "*.tfxbone"), "set_tfxbone_path", "get_tfxbone_path");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "hair_skeleton", PROPERTY_HINT_NODE_TYPE, "Skeleton3D"), "set_hair_skeleton", "get_hair_skeleton");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "num_follow_hairs", PROPERTY_HINT_RANGE, "0,64,1"), "set_num_follow_hairs", "get_num_follow_hairs");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "tip_separation"), "set_tip_separation", "get_tip_separation");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "follow_radius"), "set_follow_radius", "get_follow_radius");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "import_scale"), "set_import_scale", "get_import_scale");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "material", PROPERTY_HINT_RESOURCE_TYPE, "ShaderMaterial"), "set_material", "get_material");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "cast_hair_shadows"), "set_cast_hair_shadows", "get_cast_hair_shadows");

	ADD_GROUP("LOD", "lod_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "lod_enabled"), "set_lod_enabled", "get_lod_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "lod_start_distance", PROPERTY_HINT_RANGE, "0,100,0.1,or_greater,suffix:m"), "set_lod_start_distance", "get_lod_start_distance");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "lod_end_distance", PROPERTY_HINT_RANGE, "0,100,0.1,or_greater,suffix:m"), "set_lod_end_distance", "get_lod_end_distance");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "lod_percent", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_lod_percent", "get_lod_percent");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "lod_width_multiplier"), "set_lod_width_multiplier", "get_lod_width_multiplier");

	ADD_GROUP("Collision", "collision_");
	const String node_array_hint = String::num_int64(Variant::OBJECT) + String("/") + String::num_int64(PROPERTY_HINT_NODE_TYPE) + String(":TressFXCollisionMesh");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "collision_meshes", PROPERTY_HINT_TYPE_STRING, node_array_hint), "set_collision_meshes", "get_collision_meshes");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "collision_skip_root_vertices", PROPERTY_HINT_RANGE, "0,64,1"), "set_collision_skip_root_vertices", "get_collision_skip_root_vertices");

	ADD_GROUP("Simulation", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "vsp_coeff", PROPERTY_HINT_RANGE, "0,1,0.001"), "set_vsp_coeff", "get_vsp_coeff");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "vsp_accel_threshold"), "set_vsp_accel_threshold", "get_vsp_accel_threshold");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "local_stiffness", PROPERTY_HINT_RANGE, "0,1,0.001"), "set_local_stiffness", "get_local_stiffness");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "local_iterations", PROPERTY_HINT_RANGE, "0,32,1"), "set_local_iterations", "get_local_iterations");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "global_stiffness", PROPERTY_HINT_RANGE, "0,1,0.001"), "set_global_stiffness", "get_global_stiffness");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "global_range", PROPERTY_HINT_RANGE, "0,1,0.001"), "set_global_range", "get_global_range");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "length_iterations", PROPERTY_HINT_RANGE, "0,32,1"), "set_length_iterations", "get_length_iterations");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "damping", PROPERTY_HINT_RANGE, "0,1,0.001"), "set_damping", "get_damping");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "gravity"), "set_gravity", "get_gravity");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "wind_direction"), "set_wind_direction", "get_wind_direction");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "wind_magnitude"), "set_wind_magnitude", "get_wind_magnitude");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "clamp_position_delta"), "set_clamp_position_delta", "get_clamp_position_delta");

	ADD_GROUP("Culling", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "simulation_distance", PROPERTY_HINT_RANGE, "0,100,0.5,or_greater,suffix:m"), "set_simulation_distance", "get_simulation_distance");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "simulate_offscreen"), "set_simulate_offscreen", "get_simulate_offscreen");
}

} // namespace godot
