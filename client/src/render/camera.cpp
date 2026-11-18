#include "render/camera.hpp"

void mat_mul(float out[16], const float a[16], const float b[16])
{
	for (int c = 0; c < 4; c++)
	{
		for (int r = 0; r < 4; r++)
		{
			float s = 0;
			for (int k = 0; k < 4; k++) { s += a[k * 4 + r] * b[c * 4 + k]; }
			out[c * 4 + r] = s;
		}
	}
}

void Camera::mvp(float out[16], float nearZ, float farZ) const
{
	const float t = 1.0f / tanY;
	float proj[16] = {};
	proj[0] = t / aspect;
	proj[5] = t;
	proj[11] = -1;
#if defined(SOKOL_VULKAN) || defined(SOKOL_METAL)	// z clip 0..1 (GL: -1..1)
	proj[10] = farZ / (nearZ - farZ);	// clip z in 0..1
	proj[14] = farZ * nearZ / (nearZ - farZ);
#else
	proj[10] = (farZ + nearZ) / (nearZ - farZ);
	proj[14] = 2 * farZ * nearZ / (nearZ - farZ);
#endif
	float view[16] = {};
	for (int i = 0; i < 3; i++)
	{
		view[i * 4 + 0] = (float)r[i];
		view[i * 4 + 1] = (float)u[i];
		view[i * 4 + 2] = (float)-f[i];
	}
	view[15] = 1;
	mat_mul(out, proj, view);
}
