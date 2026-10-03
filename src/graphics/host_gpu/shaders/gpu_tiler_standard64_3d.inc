#include "gpu_tiler_standard4_3d.inc"
uvec3 standard64_3d_extent() {
	if (ELEMENT_BYTES == 1) return uvec3(64, 32, 32);
	if (ELEMENT_BYTES == 2) return uvec3(32, 32, 32);
	if (ELEMENT_BYTES == 4) return uvec3(32, 32, 16);
	if (ELEMENT_BYTES == 8) return uvec3(32, 16, 16);
	return uvec3(16, 16, 16);
}
uint standard64_3d_offset(uvec3 p) {
	// NOTE: this was a nested ternary selecting uvec4 constants. Some drivers
	// crash compiling the resulting composite OpSpecConstantOp Select chains
	// (observed amdvlk null-read at pipeline creation). The arithmetic below
	// is exactly equivalent for the valid element sizes 1/2/4/8/16.
	uvec4 s = uvec4(2u + uint(ELEMENT_BYTES < 16u) + uint(ELEMENT_BYTES == 1u),
	                3u + uint(ELEMENT_BYTES < 4u),
	                3u + uint(ELEMENT_BYTES < 8u),
	                3u + uint(ELEMENT_BYTES < 16u) + uint(ELEMENT_BYTES == 1u));
	return standard3d_offset(p) ^ (((p.x >> s.x) & 1u) << 12) ^
	       (((p.z >> s.y) & 1u) << 13) ^ (((p.y >> s.z) & 1u) << 14) ^
	       (((p.x >> s.w) & 1u) << 15);
}
