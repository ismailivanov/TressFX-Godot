// SPDX-License-Identifier: MIT

#include "tressfx_gpu.h"

#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/core/memory.hpp>

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

void tressfx_dispatch(RenderingDevice *p_rd, int64_t p_list, int p_pass, int p_groups) {
	int32_t push_constant[4] = { p_pass, 0, 0, 0 };
	PackedByteArray bytes;
	bytes.resize(sizeof(push_constant));
	memcpy(bytes.ptrw(), push_constant, sizeof(push_constant));
	p_rd->compute_list_set_push_constant(p_list, bytes, sizeof(push_constant));
	p_rd->compute_list_dispatch(p_list, p_groups, 1, 1);
	p_rd->compute_list_add_barrier(p_list);
}

} // namespace godot
