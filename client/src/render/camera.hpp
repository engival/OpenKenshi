#pragma once
// The view a frame renders from. Renderers take this, never the window size or main.cpp's camera globals, so a later stereo mode
// can resolve one per eye. main.cpp's current_camera builds it once per frame.

void mat_mul(float out[16], const float a[16], const float b[16]);	// column-major out = a x b

struct Camera
{
	double pos[3];
	double f[3], r[3], u[3];	// forward / right / up unit vectors (+y up, right = forward x up)
	float fovY, tanY, aspect;	// vertical field of view (radians), tan(fovY / 2), width / height
	float nearZ, farZ;
	int width, height;		// target pixels
	float focus = 0;		// distance to what the camera looks at (the floating camera's focus, the followed character); 0 = none (free camera)

	// Camera-relative projection x view (the position is not in it: renderers draw relative to pos).
	void mvp(float out[16], float nearZ, float farZ) const;
	void mvp(float out[16]) const { mvp(out, nearZ, farZ); }
};
