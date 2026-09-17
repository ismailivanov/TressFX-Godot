// SPDX-License-Identifier: MIT

#pragma once

#include <godot_cpp/classes/editor_node3d_gizmo.hpp>
#include <godot_cpp/classes/editor_node3d_gizmo_plugin.hpp>
#include <godot_cpp/classes/button.hpp>
#include <godot_cpp/classes/editor_plugin.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>

namespace godot {

class TressFXCollisionMesh;

// Draws every TressFXCollisionMesh as the distance field sees it. Unselected, the shape is depth
// tested, so only the parts sticking out of the body show. Selecting the collider, or a
// TressFXHair that lists it, also draws the hidden parts on top.
class TressFXCollisionGizmoPlugin : public EditorNode3DGizmoPlugin {
	GDCLASS(TressFXCollisionGizmoPlugin, EditorNode3DGizmoPlugin)

	Ref<StandardMaterial3D> visible_material;
	Ref<StandardMaterial3D> visible_selected_material;
	Ref<StandardMaterial3D> hidden_material;

	static bool _is_highlighted(const TressFXCollisionMesh *p_collider);

protected:
	static void _bind_methods() {}

public:
	// Editor setting behind the "Hair Colliders" toolbar button.
	static bool shapes_visible();

	TressFXCollisionGizmoPlugin();

	bool _has_gizmo(Node3D *p_for_node_3d) const override;
	String _get_gizmo_name() const override;
	void _redraw(const Ref<EditorNode3DGizmo> &p_gizmo) override;
};

// Always-on editor plugin of the extension (there is no plugin.cfg to enable).
class TressFXEditorPlugin : public EditorPlugin {
	GDCLASS(TressFXEditorPlugin, EditorPlugin)

	Ref<TressFXCollisionGizmoPlugin> gizmo_plugin;
	Button *toggle = nullptr;

	void _redraw_colliders();
	void _toggled(bool p_on);
	void _settings_changed();

protected:
	static void _bind_methods() {}
	void _notification(int p_what);

public:
	String _get_plugin_name() const override;
};

} // namespace godot
