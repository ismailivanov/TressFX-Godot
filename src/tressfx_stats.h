// SPDX-License-Identifier: MIT

#pragma once

#include <godot_cpp/classes/label.hpp>

namespace godot {

// On-screen performance readout for every TressFXHair and TressFXCollisionMesh in the tree.
// Drop it under a CanvasLayer; it anchors itself to the top right.
class TressFXStats : public Label {
	GDCLASS(TressFXStats, Label)

	bool measure_gpu = true;
	double gpu_sim_ns = 0.0;
	double gpu_sdf_ns = 0.0;
	double accumulated = 0.0;
	int frames = 0;

	void _read_timestamps();
	void _step(double p_delta);

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	void set_measure_gpu(bool p_enabled);
	bool get_measure_gpu() const;
};

} // namespace godot
