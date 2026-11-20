// `log gait ID|players|combat|all|off` (kenshi-h2v.1.9.1, the crab-walk check; dev-only, docs/CLIENT.md "Dev-only code is fenced"): per frame per character, the sim's yaw
// (the wire sample), the yaw the interpolation hands the body (spec.yaw) and the yaw it is drawn at (c.orient), the heading of the wire's own velocity, the legs'
// forward / lateral speed in the body frame (the filtered sl.vFwd / vLat the clips read), the catch-up's world velocity (its share of those), the wire's fwd / lat
// and the lower clips. Angles in degrees, yaw convention (forward = (sin, cos)); dy = drawn - sim velocity heading, wrapped. Lines go to stdout, as `log feet` does.
// The characters: the one id, the players, the sim's combat ones, or all drawn within the interest radius (nearest the camera first, MINE cap 32).
#include "dev/devhooks.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "characters/characters.hpp"
#include "net/simlink.hpp"
#include "bridge/simview.hpp"

namespace dev
{
namespace
{
bool on = false;
int mode = 1;	// 0 id, 1 players, 2 combat, 3 all
uint64_t wantId = 0;
double clockT = 0;

std::vector<uint64_t> pick(const SimView& sim, const double cam[3])
{
	std::vector<uint64_t> ids;
	if (mode == 3)
	{
		std::vector<std::pair<double, uint64_t>> near;
		for (const uint64_t id : sim.characterIds())
		{
			double feet[3];
			float hull[3];
			if (!sim.placed(id, feet, hull)) { continue; }
			if (std::hypot(feet[0] - cam[0], feet[2] - cam[2]) > SimView::kInterestRadius) { continue; }
			near.emplace_back(std::hypot(feet[0] - cam[0], feet[1] - cam[1], feet[2] - cam[2]), id);
		}
		std::sort(near.begin(), near.end());
		for (size_t i = 0; i < near.size() && i < 32; i++) { ids.push_back(near[i].second); }
	}
	else
	{
		for (const auto& [id, ch] : sim.decoder().chars)
		{
			const bool want = mode == 0 ? id == wantId : mode == 2 ? (ch.cur.flags & simlink::kCombat) != 0 : (ch.cur.flags & simlink::kPlayer) != 0;
			if (want) { ids.push_back(id); }
		}
		std::sort(ids.begin(), ids.end());
	}
	return ids;
}
}

bool gaitOp(const std::string& arg)
{
	on = arg != "off";
	mode = arg == "players" ? 1 : arg == "combat" ? 2 : arg == "all" ? 3 : arg == "off" ? 1 : 0;
	wantId = mode == 0 ? strtoull(arg.c_str(), nullptr, 10) : 0;
	clockT = 0;
	return true;
}

void gaitFrame(const SimView& sim, Characters& chars, const double cam[3], double dt)
{
	if (!on) { return; }
	clockT += dt;
	for (const uint64_t id : pick(sim, cam))
	{
		const auto ch = sim.decoder().chars.find(id);
		if (ch == sim.decoder().chars.end()) { continue; }
		const int index = sim.charIndex(id);
		if (index < 0) { continue; }
		const Characters::Spec& s = chars.live((size_t)index);
		const double span = ((double)ch->second.cur.tick - (double)ch->second.prev.tick) / SimView::kTickRate;
		const double vx = span > 0 ? (ch->second.cur.pos[0] - ch->second.prev.pos[0]) / span : 0.0, vz = span > 0 ? (ch->second.cur.pos[2] - ch->second.prev.pos[2]) / span : 0.0;
		const double wv = std::hypot(vx, vz), deg = 180.0 / M_PI;
		const double head = wv > 1.0 ? std::atan2(vx, vz) * deg : NAN;
		const double drawn = chars.drawnYaw((size_t)index) * deg;
		const double dy = std::isnan(head) ? NAN : std::remainder(drawn - head, 360.0);
		double cv[2] = {};
		sim.catchVelocity(id, cv);
		double at[3] = {};
		float hull[3];
		sim.placed(id, at, hull);	// k16.2.183: the drawn position, for the formation check
		printf("gait %.4f %.4f id %llu tick %llu simyaw %.1f interp %.1f drawn %.1f vhead %.1f dy %.1f wv %.2f legs %.2f %.2f catch %.2f %.2f wire %.2f %.2f flags 0x%x at %.2f %.2f | %s\n",
			clockT, dt, (unsigned long long)id, (unsigned long long)ch->second.cur.tick, ch->second.cur.yaw * deg, s.yaw * deg, drawn, head, dy, wv, s.move, s.strafe, cv[0], cv[1],
			ch->second.cur.fwd, ch->second.cur.lat, ch->second.cur.flags, at[0], at[2], chars.animSummary((size_t)index).c_str());
	}
}
}
