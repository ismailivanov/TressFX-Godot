// SPDX-License-Identifier: MIT

#pragma once

#include <godot_cpp/classes/skeleton3d.hpp>
#include <godot_cpp/templates/local_vector.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/transform3d.hpp>

namespace godot {

// World-space skinning matrices of a Skeleton3D (skeleton transform x bone global pose x inverse
// bind pose), the same matrices its skinned meshes use. Without a skeleton there is a single
// bone: the fallback transform passed to update().
struct TressFXSkin {
	uint64_t skeleton_id = 0;
	int bone_count = 1;
	LocalVector<Transform3D> inverse_bind;
	LocalVector<Transform3D> transforms;

	void init(Skeleton3D *p_skeleton);
	Skeleton3D *get_skeleton() const;
	int find_bone(const String &p_name) const;
	void update(const Transform3D &p_fallback);
	// Column-major mat4 per bone, ready for a storage buffer.
	PackedByteArray pack() const;
};

} // namespace godot
