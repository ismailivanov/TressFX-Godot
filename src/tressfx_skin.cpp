// SPDX-License-Identifier: MIT

#include "tressfx_skin.h"

#include "tressfx_gpu.h"

#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/skin.hpp>
#include <godot_cpp/core/object.hpp>

namespace godot {

void TressFXSkin::init(Skeleton3D *p_skeleton) {
	skeleton_id = 0;
	bone_count = 1;
	inverse_bind.clear();
	transforms.clear();
	inverse_bind.push_back(Transform3D());
	transforms.push_back(Transform3D());
	if (p_skeleton == nullptr || p_skeleton->get_bone_count() == 0) {
		return;
	}
	skeleton_id = p_skeleton->get_instance_id();
	bone_count = MIN(p_skeleton->get_bone_count(), TRESSFX_MAX_BONES);
	inverse_bind.resize(bone_count);
	transforms.resize(bone_count);
	// Bind the rest pose where it sits in the skeleton's scene, as glTF inverse binds do: hair and
	// collider positions are relative to the scene root (the imported file's root), skinned
	// mesh or not.
	Transform3D to_scene;
	const Node *owner = p_skeleton->get_owner();
	for (Node *n = p_skeleton; n != nullptr && n != owner; n = n->get_parent()) {
		if (const Node3D *n3d = Object::cast_to<Node3D>(n)) {
			to_scene = n3d->get_transform() * to_scene;
		}
	}
	for (int i = 0; i < bone_count; i++) {
		inverse_bind[i] = (to_scene * p_skeleton->get_bone_global_rest(i)).affine_inverse();
	}
	// Prefer the mesh Skin's inverse bind poses: an exact match with the rendered mesh.
	TypedArray<Node> children = p_skeleton->get_children();
	for (int c = 0; c < children.size(); c++) {
		MeshInstance3D *mi = Object::cast_to<MeshInstance3D>(children[c]);
		if (mi == nullptr) {
			continue;
		}
		Ref<Skin> skin = mi->get_skin();
		if (skin.is_null()) {
			continue;
		}
		for (int j = 0; j < skin->get_bind_count(); j++) {
			int bone = skin->get_bind_bone(j);
			if (bone < 0) {
				bone = p_skeleton->find_bone(skin->get_bind_name(j));
			}
			if (bone >= 0 && bone < bone_count) {
				inverse_bind[bone] = skin->get_bind_pose(j);
			}
		}
		break;
	}
}

Skeleton3D *TressFXSkin::get_skeleton() const {
	if (skeleton_id == 0) {
		return nullptr;
	}
	return Object::cast_to<Skeleton3D>(ObjectDB::get_instance(skeleton_id));
}

int TressFXSkin::find_bone(const String &p_name) const {
	Skeleton3D *skeleton = get_skeleton();
	return skeleton != nullptr ? skeleton->find_bone(p_name) : -1;
}

void TressFXSkin::update(const Transform3D &p_fallback) {
	Skeleton3D *skeleton = get_skeleton();
	if (skeleton == nullptr) {
		transforms[0] = p_fallback;
		return;
	}
	const Transform3D world = skeleton->get_global_transform();
	for (int i = 0; i < bone_count; i++) {
		transforms[i] = world * skeleton->get_bone_global_pose(i) * inverse_bind[i];
	}
}

PackedByteArray TressFXSkin::pack() const {
	PackedByteArray out;
	out.resize(transforms.size() * 16 * sizeof(float));
	float *f = (float *)out.ptrw();
	for (uint32_t i = 0; i < transforms.size(); i++) {
		const Transform3D &m = transforms[i];
		float *o = f + i * 16;
		for (int col = 0; col < 3; col++) {
			o[col * 4] = m.basis.rows[0][col];
			o[col * 4 + 1] = m.basis.rows[1][col];
			o[col * 4 + 2] = m.basis.rows[2][col];
			o[col * 4 + 3] = 0.0f;
		}
		o[12] = m.origin.x;
		o[13] = m.origin.y;
		o[14] = m.origin.z;
		o[15] = 1.0f;
	}
	return out;
}

} // namespace godot
