shader_type spatial;
// alpha_to_coverage: no sorting or blending, MSAA turns the per-pixel coverage into soft
// edges. Godot puts A2C materials in the transparent pass, which does not write depth, so
// depth_draw_always is needed or strands never occlude each other and the hair looks x-rayed.
// ponytail: TressFX uses ShortCut OIT here, needs custom render passes; revisit if this isn't
// good enough.
render_mode world_vertex_coords, cull_disabled, alpha_to_coverage, depth_draw_always;

// Set by TressFXHair: simulated vertices, xyz = world position, w = packed tangent.
uniform sampler2D positions : filter_nearest;
// Set by TressFXHair: one texel per strand, its texture coordinate on the body albedo.
uniform sampler2D strand_uvs : filter_nearest;
uniform int verts_per_strand = 8;
// Set by TressFXHair per mesh instance: strands are dealt into LOD_BUCKETS meshes, strand k of
// mesh b is global strand b + k * LOD_BUCKETS.
instance uniform int lod_bucket = 0;
const int LOD_BUCKETS = 4;

// TressFXStrandParameters / TressFXParameters (defaults from TressFXSettings.h).
uniform sampler2D base_albedo : source_color, hint_default_white, filter_linear_mipmap;
uniform vec4 base_color : source_color = vec4(1.0, 1.0, 1.0, 0.63); // alpha = strand opacity
uniform vec4 tip_color : source_color = vec4(1.0, 1.0, 1.0, 0.63);
uniform float tip_percentage = 0.0;
uniform float fiber_radius = 0.0021;
uniform float fiber_ratio = 0.463; // tip radius / root radius
uniform bool thin_tip = true;
uniform float expand_pixels = 0.71;
// Hair LOD (set by TressFXHair): fibers widen with distance to make up for dropped strands.
uniform float lod_start = 1.0;
uniform float lod_end = 5.0;
uniform float lod_width_multiplier = 1.0;
uniform float k_diffuse = 0.07;
uniform float k_spec1 = 0.0017; // primary (root-shifted) highlight, light coloured
uniform float spec_exp1 = 14.4;
uniform float k_spec2 = 0.072; // secondary (tip-shifted) highlight, hair coloured
uniform float spec_exp2 = 11.8;
uniform float cone_angle_deg = 10.0;
// ponytail: TressFX self-shadows with its own deep shadow map; darkening towards the root is
// the cheap stand-in. 0 disables.
uniform float root_shadow : hint_range(0.0, 1.0) = 0.5;
// Not in TressFX: per-strand brightness/tint jitter so the volume doesn't read as one flat
// mass, and a fade over the last part of each strand so tips don't end in hard stubs.
uniform float color_variation : hint_range(0.0, 1.0) = 0.3;
uniform float tip_fade : hint_range(0.0, 1.0) = 0.25;

varying vec3 v_tangent; // view space
varying vec3 v_color;
varying float v_t; // 0 root .. 1 tip
varying float v_side; // -1 .. 1 across the ribbon
varying float v_half_width; // true strand half width, pixels
varying float v_expanded; // expanded half width, pixels

// Inverse of pack_tangent() in tressfx_sim.glsl (12+12 bit octahedral).
vec3 unpack_tangent(float p) {
	float x = floor(p / 4096.0);
	vec2 e = vec2(x, p - x * 4096.0) / 4095.0 * 2.0 - 1.0;
	vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
	float t = max(-n.z, 0.0);
	n.xy += vec2(n.x >= 0.0 ? -t : t, n.y >= 0.0 ? -t : t);
	return normalize(n);
}

// TressFXStrands.hlsl GetExpandedTressFXVert. The mesh has no attributes: two ribbon vertices
// per hair vertex, so VERTEX_ID gives the hair vertex, the side and (with lod_bucket) the strand.
void vertex() {
	int hair_vertex = VERTEX_ID >> 1;
	int strand = lod_bucket + (hair_vertex / verts_per_strand) * LOD_BUCKETS;
	int gi = strand * verts_per_strand + hair_vertex % verts_per_strand;
	float side = float(VERTEX_ID & 1) * 2.0 - 1.0;
	int w = textureSize(positions, 0).x;
	vec4 d = texelFetch(positions, ivec2(gi % w, gi / w), 0);
	vec3 p = d.xyz;
	vec3 t = unpack_tangent(d.w);

	v_t = float(gi % verts_per_strand) / float(verts_per_strand - 1);
	float ratio = thin_tip ? mix(1.0, fiber_ratio, v_t) : 1.0;
	vec3 eye = INV_VIEW_MATRIX[3].xyz;
	float lod = smoothstep(lod_start, lod_end, distance(eye, p));
	float r = fiber_radius * ratio * mix(1.0, lod_width_multiplier, lod);

	vec3 right = normalize(cross(t, normalize(p - eye)));
	// Project both true edges to measure the strand width in pixels (no assumptions about
	// the projection matrix' conventions).
	mat4 vp = PROJECTION_MATRIX * VIEW_MATRIX;
	vec4 c0 = vp * vec4(p - right * r, 1.0);
	vec4 c1 = vp * vec4(p + right * r, 1.0);
	vec2 s0 = c0.xy / max(c0.w, 1e-4) * VIEWPORT_SIZE * 0.5;
	vec2 s1 = c1.xy / max(c1.w, 1e-4) * VIEWPORT_SIZE * 0.5;
	v_half_width = max(0.5 * length(s1 - s0), 1e-4);
	float px = r / v_half_width; // world-space size of one pixel here
	v_expanded = v_half_width + expand_pixels;
	v_side = side;
	VERTEX = p + right * side * v_expanded * px;
	NORMAL = normalize(cross(right, t));
	v_tangent = (VIEW_MATRIX * vec4(t, 0.0)).xyz;

	// GetStrandColor: body albedo at the strand root, optional tip colour.
	int wu = textureSize(strand_uvs, 0).x;
	vec2 strand_uv = texelFetch(strand_uvs, ivec2(strand % wu, strand / wu), 0).xy;
	vec3 root_color = textureLod(base_albedo, strand_uv, 0.0).rgb * base_color.rgb;
	v_color = v_t > 1.0 - tip_percentage ? mix(root_color, tip_color.rgb, v_t) : root_color;
	float h1 = fract(sin(float(strand) * 12.9898) * 43758.5453);
	float h2 = fract(sin(float(strand) * 78.233) * 43758.5453);
	v_color *= 1.0 + color_variation * (h1 - 0.5) * 1.4;
	v_color *= mix(vec3(1.0), vec3(1.0 + (h2 - 0.5) * 0.3, 1.0, 1.0 - (h2 - 0.5) * 0.3), color_variation);
}

// Coverage across the ribbon: 1 inside the true strand, fading to 0 over the expand_pixels
// outside it. (TressFX's ComputeCoverage ramps across the edge and multiplies by the strand
// opacity; that suits its OIT accumulation, not alpha-to-coverage, where sub-pixel fibers
// would round to nothing. base_color.a is therefore unused.)
void fragment() {
	ALBEDO = v_color;
	float dist = abs(v_side) * v_expanded;
	ALPHA = clamp((v_expanded - dist) / max(expand_pixels, 1e-3), 0.0, 1.0);
	if (tip_fade > 0.0) {
		ALPHA *= smoothstep(1.0, 1.0 - tip_fade, v_t);
	}
	AO = mix(1.0 - root_shadow, 1.0, v_t);
	AO_LIGHT_AFFECT = 1.0;
}

// TressFXLighting.hlsl ComputeDiffuseSpecFactors: Kajiya diffuse, Marschner R lobe shifted
// to the root (light coloured) and TRT lobe shifted to the tip (hair coloured).
void light() {
	vec3 T = normalize(v_tangent);
	float cos_tl = dot(T, LIGHT);
	float sin_tl = sqrt(max(0.0, 1.0 - cos_tl * cos_tl));
	float cos_trl = -cos_tl;
	float sin_trl = sin_tl;
	float cos_te = dot(T, VIEW);
	float sin_te = sqrt(max(0.0, 1.0 - cos_te * cos_te));
	float a = radians(cone_angle_deg);

	float cos_root = cos_trl * cos(2.0 * a) - sin_trl * sin(2.0 * a);
	float sin_root = sqrt(max(0.0, 1.0 - cos_root * cos_root));
	float spec_root = max(0.0, cos_root * cos_te + sin_root * sin_te);

	float cos_tip = cos_trl * cos(-3.0 * a) - sin_trl * sin(-3.0 * a);
	float sin_tip = sqrt(max(0.0, 1.0 - cos_tip * cos_tip));
	float spec_tip = max(0.0, cos_tip * cos_te + sin_tip * sin_te);

	vec3 light_col = LIGHT_COLOR * ATTENUATION / PI;
	// DIFFUSE_LIGHT is multiplied by ALBEDO by the engine, so the coloured terms go there.
	DIFFUSE_LIGHT += (k_diffuse * sin_tl + k_spec2 * pow(spec_tip, spec_exp2)) * light_col;
	SPECULAR_LIGHT += k_spec1 * pow(spec_root, spec_exp1) * light_col;
}
