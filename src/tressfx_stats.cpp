// SPDX-License-Identifier: MIT

#include "tressfx_stats.h"

#include "tressfx_collision_mesh.h"
#include "tressfx_gpu.h"
#include "tressfx_hair.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/templates/hash_map.hpp>

namespace godot {

void TressFXStats::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY: {
			// A runtime overlay only. In the editor nothing draws unless something changes, so
			// capturing timestamps every frame would overflow the query pool.
			if (Engine::get_singleton()->is_editor_hint()) {
				break;
			}
			set_anchors_and_offsets_preset(Control::PRESET_TOP_RIGHT);
			set_offset(SIDE_LEFT, -560);
			set_offset(SIDE_RIGHT, -12);
			set_offset(SIDE_TOP, 12);
			set_horizontal_alignment(HORIZONTAL_ALIGNMENT_RIGHT);
			add_theme_font_size_override("font_size", 14);
			add_theme_color_override("font_shadow_color", Color(0, 0, 0, 0.8));
			add_theme_constant_override("shadow_offset_x", 1);
			add_theme_constant_override("shadow_offset_y", 1);
			set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
			tressfx_gpu_timing = measure_gpu;
			RenderingServer::get_singleton()->viewport_set_measure_render_time(get_viewport()->get_viewport_rid(), true);
			set_process(true);
		} break;
		case NOTIFICATION_EXIT_TREE: {
			tressfx_gpu_timing = false;
		} break;
		case NOTIFICATION_PROCESS: {
			_step(get_process_delta_time());
		} break;
	}
}

void TressFXStats::_step(double p_delta) {
	if (measure_gpu) {
		RenderingServer::get_singleton()->call_on_render_thread(callable_mp(this, &TressFXStats::_read_timestamps));
	}
	accumulated += p_delta;
	frames++;
	if (accumulated < 0.25) {
		return;
	}
	const double fps = frames / accumulated;
	accumulated = 0.0;
	frames = 0;

	int strands = 0;
	int guides = 0;
	int verts = 0;
	int cpu_us = 0;
	TypedArray<Node> hairs = get_tree()->get_nodes_in_group("tressfx_hair");
	for (int i = 0; i < hairs.size(); i++) {
		TressFXHair *hair = Object::cast_to<TressFXHair>(hairs[i]);
		if (hair == nullptr) {
			continue;
		}
		strands += hair->get_strand_count();
		guides += hair->get_guide_strand_count();
		verts += hair->get_vertex_count();
		cpu_us += hair->get_cpu_time_usec();
	}
	int64_t cells = 0;
	TypedArray<Node> meshes = get_tree()->get_nodes_in_group("tressfx_collision");
	for (int i = 0; i < meshes.size(); i++) {
		TressFXCollisionMesh *mesh = Object::cast_to<TressFXCollisionMesh>(meshes[i]);
		if (mesh == nullptr) {
			continue;
		}
		const int64_t n = mesh->get_cell_count();
		cells += n * n * n;
		cpu_us += mesh->get_cpu_time_usec();
	}

	RenderingServer *rs = RenderingServer::get_singleton();
	const RID vp = get_viewport()->get_viewport_rid();
	const Vector2 size = get_viewport()->get_visible_rect().size;
	static const char *msaa_names[] = { "off", "2x", "4x", "8x" };
	const int msaa = CLAMP((int)get_viewport()->get_msaa_3d(), 0, 3);

	String text = String::num(fps, 0) + String(" fps  ") + String::num(1000.0 / MAX(fps, 1.0), 2) + String(" ms/frame\n");
	text += "viewport draw: gpu " + String::num(rs->viewport_get_measured_render_time_gpu(vp), 2) + String(" ms, cpu ") +
			String::num(rs->viewport_get_measured_render_time_cpu(vp), 2) + String(" ms\n");
	if (measure_gpu) {
		text += "hair sim gpu " + String::num(gpu_sim_ns / 1e6, 2) + String(" ms | sdf build gpu ") + String::num(gpu_sdf_ns / 1e6, 2) + String(" ms\n");
	} else {
		text += "hair/sdf gpu timing off (measure_gpu)\n";
	}
	text += "tressfx cpu " + String::num(cpu_us / 1000.0, 2) + String(" ms\n");
	text += String::num_int64(strands) + String(" strands (") + String::num_int64(guides) + String(" guides), ") + String::num_int64(verts) + String(" vertices\n");
	text += String::num_int64(meshes.size()) + String(" colliders, ") + String::num(cells / 1e6, 1) + String(" M sdf cells\n");
	text += String::num(rs->get_rendering_info(RenderingServer::RENDERING_INFO_TOTAL_PRIMITIVES_IN_FRAME) / 1e6, 2) + String(" M triangles drawn this frame\n");
	text += String::num_int64((int64_t)size.x) + String("x") + String::num_int64((int64_t)size.y) + String(", msaa ") + msaa_names[msaa];
	set_text(text);
}

// GPU time between the begin/end timestamps the nodes capture around their compute lists.
void TressFXStats::_read_timestamps() {
	RenderingDevice *rd = RenderingServer::get_singleton()->get_rendering_device();
	if (rd == nullptr) {
		return;
	}
	double sim = 0.0;
	double sdf = 0.0;
	HashMap<String, uint64_t> begin;
	const int count = rd->get_captured_timestamps_count();
	for (int i = 0; i < count; i++) {
		const String name = rd->get_captured_timestamp_name(i);
		if (!name.begins_with("tfx_")) {
			continue;
		}
		if (name.ends_with(":begin")) {
			begin[name.trim_suffix(":begin")] = rd->get_captured_timestamp_gpu_time(i);
		} else if (name.ends_with(":end")) {
			const String key = name.trim_suffix(":end");
			const uint64_t *start = begin.getptr(key);
			if (start != nullptr) {
				const double dt = (double)(rd->get_captured_timestamp_gpu_time(i) - *start);
				if (key.begins_with("tfx_sim")) {
					sim += dt;
				} else {
					sdf += dt;
				}
			}
		}
	}
	gpu_sim_ns = sim;
	gpu_sdf_ns = sdf;
}

void TressFXStats::set_measure_gpu(bool p_enabled) {
	measure_gpu = p_enabled;
	if (is_inside_tree() && !Engine::get_singleton()->is_editor_hint()) {
		tressfx_gpu_timing = p_enabled;
	}
}

bool TressFXStats::get_measure_gpu() const {
	return measure_gpu;
}

void TressFXStats::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_measure_gpu", "enabled"), &TressFXStats::set_measure_gpu);
	ClassDB::bind_method(D_METHOD("get_measure_gpu"), &TressFXStats::get_measure_gpu);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "measure_gpu"), "set_measure_gpu", "get_measure_gpu");
}

} // namespace godot
