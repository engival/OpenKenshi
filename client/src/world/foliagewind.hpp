#pragma once
// The one live foliage wind (kenshi-k16.2.420.1): WorldBuildings::advanceMotion smooths it from the same windSpeed / wind[] the blade spin and the
// wind-aimed parts read; leaves (render.leaf_sway) and grass (render.grass_live_wind) take it from here. Never a second ramp.
namespace foliagewind
{
struct State
{
	float strength = 1;	// 0 = still, 1 = render.foliage_wind_ref (today's look); an unknown wind (an offline scene) holds 1
	float dir[2] = { 0.8f, 0.6f };	// unit x, z the wind blows toward; today's direction until a wind is known
	double phase = 0;	// the sway clock (s-equivalent), integrated at a rate that rises with the strength, so a wind change never jumps the pose
};
inline State g;
}
