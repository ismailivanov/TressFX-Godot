// SPDX-License-Identifier: MIT
// Shared GPU helpers for the TressFX nodes. Everything in here runs on the render thread.

#pragma once

#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/templates/local_vector.hpp>

namespace godot {

// Must match the compute shaders (THREAD_GROUP_SIZE) and the .tfx format (4-64 vertices per strand).
constexpr int TRESSFX_THREAD_GROUP_SIZE = 64;
constexpr int TRESSFX_MAX_BONES = 512; // AMD_TRESSFX_MAX_NUM_BONES
constexpr int TRESSFX_TEXTURE_WIDTH = 4096; // Simulated positions texture, one texel per vertex.

// Written by TressFXStats on the main thread, read on the render thread (a flag, races are benign).
extern bool tressfx_gpu_timing;

// A compute shader and its pipeline, created on first use.
struct TressFXPipeline {
	RID shader;
	RID pipeline;

	void ensure(RenderingDevice *p_rd, const Ref<RDShaderSPIRV> &p_spirv);
	void free(RenderingDevice *p_rd);
};

// The pipelines every hair and collision node shares. The first GPU object acquires them, the last
// one to be destroyed frees them, so scenes without hair cost nothing. Heap allocated on purpose:
// a GDExtension must not hold static instances of Godot types (they are constructed at library
// load, before the bindings exist).
class TressFXPipelines {
	static TressFXPipelines *singleton;
	int users = 0;

public:
	TressFXPipeline simulation;
	TressFXPipeline collision;
	TressFXPipeline sdf;

	static TressFXPipelines *get() { return singleton; }
	static TressFXPipelines *acquire();
	static void release(RenderingDevice *p_rd);
};

Ref<RDUniform> tressfx_make_uniform(int p_binding, RenderingDevice::UniformType p_type, const RID &p_rid);
void tressfx_free_rids(RenderingDevice *p_rd, LocalVector<RID> &p_rids);
// Sets the `pass` push constant, dispatches p_groups x 1 x 1 and adds a barrier.
void tressfx_dispatch(RenderingDevice *p_rd, int64_t p_list, int p_pass, int p_groups);

} // namespace godot
