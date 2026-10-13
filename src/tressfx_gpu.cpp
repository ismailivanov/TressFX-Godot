// SPDX-License-Identifier: MIT

#include "tressfx_gpu.h"

#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/core/memory.hpp>

#include <stdlib.h>
#include <string.h>

namespace godot {

bool tressfx_gpu_timing = false;

TressFXPipelines *TressFXPipelines::singleton = nullptr;

void TressFXPipeline::ensure(RenderingDevice *p_rd, const Ref<RDShaderSPIRV> &p_spirv) {
	if (pipeline.is_valid()) {
		return;
	}
	ERR_FAIL_COND_MSG(p_spirv.is_null(), "TressFX: compute shader not loaded. Are the files in addons/tressfx/shaders imported?");
	shader = p_rd->shader_create_from_spirv(p_spirv);
	ERR_FAIL_COND_MSG(!shader.is_valid(), "TressFX: compute shader failed to compile.");
	pipeline = p_rd->compute_pipeline_create(shader);
}

void TressFXPipeline::free(RenderingDevice *p_rd) {
	if (pipeline.is_valid()) {
		p_rd->free_rid(pipeline);
	}
	if (shader.is_valid()) {
		p_rd->free_rid(shader);
	}
	pipeline = RID();
	shader = RID();
}

TressFXPipelines *TressFXPipelines::acquire() {
	if (singleton == nullptr) {
		singleton = memnew(TressFXPipelines);
	}
	singleton->users++;
	return singleton;
}

void TressFXPipelines::release(RenderingDevice *p_rd) {
	if (singleton == nullptr) {
		return;
	}
	singleton->users--;
	if (singleton->users > 0) {
		return;
	}
	singleton->simulation.free(p_rd);
	singleton->collision.free(p_rd);
	singleton->sdf.free(p_rd);
	memdelete(singleton);
	singleton = nullptr;
}

Ref<RDUniform> tressfx_make_uniform(int p_binding, RenderingDevice::UniformType p_type, const RID &p_rid) {
	Ref<RDUniform> uniform;
	uniform.instantiate();
	uniform->set_binding(p_binding);
	uniform->set_uniform_type(p_type);
	uniform->add_id(p_rid);
	return uniform;
}

void tressfx_free_rids(RenderingDevice *p_rd, LocalVector<RID> &p_rids) {
	for (const RID &rid : p_rids) {
		if (rid.is_valid()) {
			p_rd->free_rid(rid);
		}
	}
	p_rids.clear();
}

// maxComputeWorkGroupCount[0]: 65535 (the Vulkan minimum, and always on D3D12) on many GPUs, so a
// large SDF grid or hair asset needs several dispatches. In debug builds the environment variable
// TRESSFX_MAX_DISPATCH_GROUPS lowers it, to test the split on GPUs with a higher limit.
static int64_t _max_dispatch_groups(RenderingDevice *p_rd) {
	static int64_t max_groups = 0; // One rendering device per session.
	if (max_groups == 0) {
		const uint64_t device_limit = p_rd->limit_get(RenderingDevice::LIMIT_MAX_COMPUTE_WORKGROUP_COUNT_X);
		int64_t limit = device_limit == 0 ? 65535 : (int64_t)MIN(device_limit, (uint64_t)INT32_MAX); // 0: unknown, assume the minimum.
#ifdef DEBUG_ENABLED
		const char *env = getenv("TRESSFX_MAX_DISPATCH_GROUPS");
		if (env != nullptr && atoll(env) > 0) {
			limit = MIN(limit, (int64_t)atoll(env));
		}
#endif
		max_groups = CLAMP(limit, (int64_t)1, (int64_t)INT32_MAX);
	}
	return max_groups;
}

void tressfx_dispatch_groups(RenderingDevice *p_rd, int64_t p_list, int32_t *p_push_constant, int p_ints, int p_offset_slot, int p_groups) {
	const int64_t max_groups = _max_dispatch_groups(p_rd);
	PackedByteArray bytes;
	bytes.resize(p_ints * sizeof(int32_t));
	// No barrier between the pieces: within a pass every group works on its own data.
	for (int64_t first = 0; first < p_groups; first += max_groups) {
		p_push_constant[p_offset_slot] = (int32_t)first;
		memcpy(bytes.ptrw(), p_push_constant, p_ints * sizeof(int32_t));
		p_rd->compute_list_set_push_constant(p_list, bytes, (uint32_t)bytes.size());
		p_rd->compute_list_dispatch(p_list, (uint32_t)MIN(max_groups, (int64_t)p_groups - first), 1, 1);
	}
	p_rd->compute_list_add_barrier(p_list);
}

void tressfx_dispatch(RenderingDevice *p_rd, int64_t p_list, int p_pass, int p_groups) {
	int32_t push_constant[4] = { p_pass, 0, 0, 0 }; // pass, group offset
	tressfx_dispatch_groups(p_rd, p_list, push_constant, 4, 1, p_groups);
}

} // namespace godot
