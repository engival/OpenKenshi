#pragma once
#include "sokol_gfx.h"

// The cursor ID buffer (ours, user 2026-11-05: the hover peeks what is drawn, not a collision box or a bounding box): after the scene pass, the
// buildings along the cursor ray are drawn again into an RGBA32F target with the scene's depth (EQUAL, no write), so only the surface the scene
// shows under the cursor writes its building id and position. One pixel is read back through a pixel buffer a frame later (no stall).
// GL only: other backends report nothing (warned once).
class IdBuffer
{
public:
	struct Hit
	{
		bool valid = false;	// a result arrived (false for the first frames and after a resize)
		int building = -1;	// building instance index, -1: no building drawn there
		double at[3] = {};	// world position of that surface
	};

	void shutdown();
	// The pass to begin: the ID target (cleared to 0) over the scene depth (loaded, kept). Recreates the target when the size changed.
	sg_pass pass(sg_view sceneDepth, int w, int h);
	// After the pass ended: queues the read of pixel (x, y) (top-left origin) with this frame's camera, and returns the last finished read.
	Hit read(int x, int y, const double cam[3]);

private:
	static constexpr int kRing = 2;
	int w_ = 0, h_ = 0;
	sg_image image_ = {};
	sg_view att_ = {};
	unsigned fbo_ = 0, pbo_[kRing] = {};
	bool queued_[kRing] = {};
	double cam_[kRing][3] = {};
	int next_ = 0;
	Hit last_;
};
