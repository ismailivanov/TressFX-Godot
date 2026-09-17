// SPDX-License-Identifier: MIT

#include "register_types.h"

#include "tressfx_collision_mesh.h"
#include "tressfx_editor.h"
#include "tressfx_hair.h"
#include "tressfx_stats.h"

#include <godot_cpp/classes/editor_plugin_registration.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/godot.hpp>

using namespace godot;

void initialize_tressfx_module(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
		GDREGISTER_CLASS(TressFXCollisionMesh);
		GDREGISTER_CLASS(TressFXHair);
		GDREGISTER_CLASS(TressFXStats);
	} else if (p_level == MODULE_INITIALIZATION_LEVEL_EDITOR) {
		GDREGISTER_INTERNAL_CLASS(TressFXCollisionGizmoPlugin);
		GDREGISTER_INTERNAL_CLASS(TressFXEditorPlugin);
		EditorPlugins::add_by_type<TressFXEditorPlugin>();
	}
}

void uninitialize_tressfx_module(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_EDITOR) {
		EditorPlugins::remove_by_type<TressFXEditorPlugin>();
	}
}

extern "C" {
GDExtensionBool GDE_EXPORT tressfx_library_init(GDExtensionInterfaceGetProcAddress p_get_proc_address, GDExtensionClassLibraryPtr p_library, GDExtensionInitialization *r_initialization) {
	GDExtensionBinding::InitObject init_obj(p_get_proc_address, p_library, r_initialization);

	init_obj.register_initializer(initialize_tressfx_module);
	init_obj.register_terminator(uninitialize_tressfx_module);
	init_obj.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);

	return init_obj.init();
}
}
