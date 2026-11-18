#pragma once
#include <algorithm>
#include <cmath>

#include "kworld/collision.hpp"

// A door BUILDING's sliding pose, one function for the drawn leaf (WorldBuildings::setDoors) and the pick volume (main.cpp pick_at), so look and click
// never disagree (kenshi-k16.2.241.1; kinspect findings/door_pick.md door2: 140298d10 moves the leaf node and every collision actor by one offset).
struct DoorSlide
{
	int axis = 0;		// `door axis`: the shape's local x y z (Z-up parts)
	float dist = 30;	// `door move dist`
	float out = 0;		// `door comes out`: forwards out of the frame before the slide (0: one phase)
	float speed = 0.5f;	// `door move speed`: openness per second (the Door_Shut cue's 1 / speed travel)
};

// The offset at openness o (0 closed .. 1 open), world axes, of the door whose first collision shape is s. False: the shape is no box, so the
// comes-out leg has no normal and is left out (off still holds the slide).
inline bool doorOffset(const kworld::WorldShape& s, const DoorSlide& d, double o, double off[3])
{
	o = std::clamp(o, 0.0, 1.0);
	const int axis = std::clamp(d.axis, 0, 2);
	double slide[3], norm[3] = {};
	for (int k = 0; k < 3; k++) { slide[k] = s.axes[3 * axis + k]; off[k] = 0; }
	bool ok = true;
	double out = std::fabs((double)d.out), t0 = 0;
	if (out > 0)
	{
		if (s.half[0] == 0) { ok = false; out = 0; }
		else
		{
			// the leaf's normal: the thinnest other axis, turned from the actor origin toward the box centre
			int n = axis == 0 ? 1 : 0;
			for (int j = 0; j < 3; j++) { if (j != axis && s.half[j] < s.half[n]) { n = j; } }
			double side = 0;
			for (int k = 0; k < 3; k++) { side += s.axes[3 * n + k] * ((s.min[k] + s.max[k]) / 2.0 - s.actor[k]); }
			for (int k = 0; k < 3; k++) { norm[k] = s.axes[3 * n + k] * (side < 0 ? -1 : 1); }
			t0 = out / (0.25 * std::fabs((double)d.dist) + out);
		}
	}
	if (out == 0)
	{
		for (int k = 0; k < 3; k++) { off[k] = slide[k] * d.dist * o; }
		return ok;
	}
	if (o < t0) { for (int k = 0; k < 3; k++) { off[k] = norm[k] * out * (o / t0); } }
	else { for (int k = 0; k < 3; k++) { off[k] = norm[k] * out + slide[k] * d.dist * (o - t0) / (1 - t0); } }
	return ok;
}
