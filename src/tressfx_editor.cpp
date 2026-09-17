// SPDX-License-Identifier: MIT

#include "tressfx_editor.h"

#include "tressfx_collision_mesh.h"
#include "tressfx_hair.h"

#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_selection.hpp>
#include <godot_cpp/classes/editor_settings.hpp>
#include <godot_cpp/classes/theme.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/triangle_mesh.hpp>

namespace godot {

static const char *SHOW_SHAPES_SETTING = "tressfx/editor/show_collision_shapes";

bool TressFXCollisionGizmoPlugin::shapes_visible() {
	const Ref<EditorSettings> settings = EditorInterface::get_singleton()->get_editor_settings();
	return settings.is_null() || !settings->has_setting(SHOW_SHAPES_SETTING) || bool(settings->get_setting(SHOW_SHAPES_SETTING));
}

static Ref<StandardMaterial3D> _gizmo_material(float p_alpha, bool p_on_top) {
	Ref<StandardMaterial3D> m;
	m.instantiate();
	m->set_albedo(Color(1.0f, 0.55f, 0.15f, p_alpha));
	m->set_shading_mode(BaseMaterial3D::SHADING_MODE_UNSHADED);
	m->set_flag(BaseMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR, true);
	m->set_flag(BaseMaterial3D::FLAG_DISABLE_FOG, true);
	m->set_cull_mode(BaseMaterial3D::CULL_BACK);
	if (p_on_top) {
		m->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA);
		m->set_flag(BaseMaterial3D::FLAG_DISABLE_DEPTH_TEST, true);
		m->set_render_priority(Material::RENDER_PRIORITY_MAX);
	} else {
		// The pre-pass lets the shape hide its own back side and hide behind the body.
		m->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA_DEPTH_PRE_PASS);
	}
	return m;
}

TressFXCollisionGizmoPlugin::TressFXCollisionGizmoPlugin() {
	visible_material = _gizmo_material(0.3f, false);
	visible_selected_material = _gizmo_material(0.55f, false);
	hidden_material = _gizmo_material(0.3f, true);
}

bool TressFXCollisionGizmoPlugin::_has_gizmo(Node3D *p_for_node_3d) const {
	return Object::cast_to<TressFXCollisionMesh>(p_for_node_3d) != nullptr;
}

String TressFXCollisionGizmoPlugin::_get_gizmo_name() const {
	return "TressFXCollisionMesh";
}

bool TressFXCollisionGizmoPlugin::_is_highlighted(const TressFXCollisionMesh *p_collider) {
	const TypedArray<Node> selected = EditorInterface::get_singleton()->get_selection()->get_selected_nodes();
	for (int i = 0; i < selected.size(); i++) {
		const Object *node = selected[i].get_validated_object();
		if (node == p_collider) {
			return true;
		}
		const TressFXHair *hair = Object::cast_to<TressFXHair>(node);
		if (hair == nullptr) {
			continue;
		}
		const TypedArray<TressFXCollisionMesh> colliders = hair->get_collision_meshes();
		for (int j = 0; j < colliders.size(); j++) {
			if (colliders[j].get_validated_object() == p_collider) {
				return true;
			}
		}
	}
	return false;
}

void TressFXCollisionGizmoPlugin::_redraw(const Ref<EditorNode3DGizmo> &p_gizmo) {
	p_gizmo->clear();
	TressFXCollisionMesh *collider = Object::cast_to<TressFXCollisionMesh>(p_gizmo->get_node_3d());
	if (collider == nullptr || !shapes_visible()) {
		return;
	}
	const Ref<ArrayMesh> mesh = collider->build_debug_mesh();
	if (mesh.is_null()) {
		return;
	}
	if (_is_highlighted(collider)) {
		p_gizmo->add_mesh(mesh, hidden_material);
		p_gizmo->add_mesh(mesh, visible_selected_material);
	} else {
		p_gizmo->add_mesh(mesh, visible_material);
	}
	// Click to select. Building the shape takes milliseconds on a body mesh, so while the pose is
	// changing (an animation playing) the last one is kept.
	const bool settled = collider->skin.get_skeleton() == nullptr || collider->gizmo_still_frames >= 30;
	if (collider->gizmo_triangles.is_null() || (collider->gizmo_triangles_stale && settled)) {
		collider->gizmo_triangles = mesh->generate_triangle_mesh();
		collider->gizmo_triangles_stale = false;
	}
	p_gizmo->add_collision_triangles(collider->gizmo_triangles);
}

void TressFXEditorPlugin::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			EditorInterface *editor = EditorInterface::get_singleton();
			const Ref<EditorSettings> settings = editor->get_editor_settings();
			if (!settings->has_setting(SHOW_SHAPES_SETTING)) {
				settings->set_setting(SHOW_SHAPES_SETTING, true);
			}
			settings->set_initial_value(SHOW_SHAPES_SETTING, true, false);
			Dictionary info;
			info["name"] = SHOW_SHAPES_SETTING;
			info["type"] = Variant::BOOL;
			settings->add_property_info(info);

			gizmo_plugin.instantiate();
			add_node_3d_gizmo_plugin(gizmo_plugin);

			toggle = memnew(Button);
			toggle->set_text("Hair Colliders");
			toggle->set_tooltip_text("Show the TressFX collision shapes the hair is kept out of.");
			toggle->set_button_icon(editor->get_editor_theme()->get_icon("CollisionShape3D", "EditorIcons"));
			toggle->set_flat(true);
			toggle->set_toggle_mode(true);
			toggle->set_focus_mode(Control::FOCUS_NONE);
			toggle->set_pressed_no_signal(TressFXCollisionGizmoPlugin::shapes_visible());
			toggle->connect("toggled", callable_mp(this, &TressFXEditorPlugin::_toggled));
			add_control_to_container(CONTAINER_SPATIAL_EDITOR_MENU, toggle);

			editor->get_selection()->connect("selection_changed", callable_mp(this, &TressFXEditorPlugin::_redraw_colliders));
			settings->connect("settings_changed", callable_mp(this, &TressFXEditorPlugin::_settings_changed));
		} break;
		case NOTIFICATION_EXIT_TREE: {
			EditorInterface *editor = EditorInterface::get_singleton();
			const Callable redraw = callable_mp(this, &TressFXEditorPlugin::_redraw_colliders);
			if (editor->get_selection()->is_connected("selection_changed", redraw)) {
				editor->get_selection()->disconnect("selection_changed", redraw);
			}
			const Ref<EditorSettings> settings = editor->get_editor_settings();
			const Callable changed = callable_mp(this, &TressFXEditorPlugin::_settings_changed);
			if (settings->is_connected("settings_changed", changed)) {
				settings->disconnect("settings_changed", changed);
			}
			remove_control_from_container(CONTAINER_SPATIAL_EDITOR_MENU, toggle);
			memdelete(toggle);
			toggle = nullptr;
			remove_node_3d_gizmo_plugin(gizmo_plugin);
			gizmo_plugin.unref();
		} break;
	}
}

void TressFXEditorPlugin::_toggled(bool p_on) {
	EditorInterface::get_singleton()->get_editor_settings()->set_setting(SHOW_SHAPES_SETTING, p_on);
	_redraw_colliders();
}

// The setting can also be changed in Editor Settings.
void TressFXEditorPlugin::_settings_changed() {
	const bool on = TressFXCollisionGizmoPlugin::shapes_visible();
	if (toggle != nullptr && toggle->is_pressed() != on) {
		toggle->set_pressed_no_signal(on);
		_redraw_colliders();
	}
}

// Selecting a hair highlights its colliders, so every collider has to be redrawn.
void TressFXEditorPlugin::_redraw_colliders() {
	const TypedArray<Node> colliders = get_tree()->get_nodes_in_group("tressfx_collision");
	for (int i = 0; i < colliders.size(); i++) {
		if (Node3D *node = Object::cast_to<Node3D>(colliders[i].get_validated_object())) {
			node->update_gizmos();
		}
	}
}

String TressFXEditorPlugin::_get_plugin_name() const {
	return "TressFX";
}

} // namespace godot
