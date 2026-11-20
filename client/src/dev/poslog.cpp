// `log pos ID|players|all|off` (kenshi-k16.2.180.1, the pause-toggle rocking check; dev-only, docs/CLIENT.md "Dev-only code is fenced"): per frame per character,
// the sim's interpolated position (what gameplay reads), the drawn position (the display offset included), and the move the drawn made since that character's
// previous logged frame. The render clock (the tick the interpolation is at) and the newest complete snapshot's tick ride every line, so a clock hold, snap or
// ease across a pause shows beside the position it moved. Lines go to stdout, as `log feet` does.
// The characters: the one id, the players, or every drawn character within the interest radius (nearest the camera first, cap 32).
// Two trailing fields (kenshi-k16.2.181.1, the 5x stutter check): `wall`, the steady clock since the op was turned on, and `snaps`, the
// decoder's SNAPSHOT_END count (its rise between two frames is how many snapshots that frame took in). A scripted run advances its sim clock
// a fixed 1/60 s a frame whatever the frame really took, so the frame's own time tells that harness artefact apart from a stutter, and the
// arrival count tells an even send from a bursty one.
#include "dev/devhooks.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>

#include "net/simlink.hpp"
#include "bridge/simview.hpp"

namespace dev
{
namespace
{
bool on = false;
int mode = 1;	// 0 id, 1 players, 2 all
uint64_t wantId = 0;
double clockT = 0;
double wallT = 0;
std::chrono::steady_clock::time_point wallStart;
std::map<uint64_t, double> last[3];	// the previous logged frame's drawn position: [axis][id]

std::vector<uint64_t> pickIds(const SimView& sim, const double cam[3])
{
	std::vector<uint64_t> ids;
	if (mode == 2)
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
			if (mode == 0 ? id == wantId : (ch.cur.flags & simlink::kPlayer) != 0) { ids.push_back(id); }
		}
		std::sort(ids.begin(), ids.end());
	}
	return ids;
}
}

bool posOp(const std::string& arg)
{
	on = arg != "off";
	mode = arg == "players" ? 1 : arg == "all" ? 2 : arg == "off" ? 1 : 0;
	wantId = mode == 0 ? strtoull(arg.c_str(), nullptr, 10) : 0;
	clockT = 0;
	wallT = 0;
	wallStart = std::chrono::steady_clock::now();
	for (auto& l : last) { l.clear(); }
	return true;
}

void posFrame(const SimView& sim, const Characters& chars, const double cam[3], double dt)
{
	if (!on) { return; }
	clockT += dt;
	wallT = std::chrono::duration<double>(std::chrono::steady_clock::now() - wallStart).count();
	const auto& cl = sim.decoder().clock;
	const unsigned long long newest = (unsigned long long)sim.decoder().lastCompleteTick;
	const unsigned snapSerial = sim.decoder().snapSerial;	// counts SNAPSHOT_END frames: its rise between two frames is the snapshots the frame took in
	const int paused = cl.valid && cl.paused ? 1 : 0;
	for (const uint64_t id : pickIds(sim, cam))
	{
		double drawn[3], base[3];
		float hull[3];
		if (!sim.placed(id, drawn, hull) || !sim.simPos(id, base)) { continue; }
		auto it = last[0].find(id);
		const double step = it == last[0].end() ? NAN : std::hypot(drawn[0] - it->second, drawn[1] - last[1][id], drawn[2] - last[2][id]);
		last[0][id] = drawn[0]; last[1][id] = drawn[1]; last[2][id] = drawn[2];
		// The wire's own samples (kenshi-k16.2.264.1): the newest and the one before it, their ticks and how many the client holds, so a sample that
		// arrived late or stood still shows beside the drawn; then what the body is playing and whether it is in the frame's set at all.
		const auto ch = sim.decoder().chars.find(id);
		const uint64_t curT = ch == sim.decoder().chars.end() ? 0 : ch->second.cur.tick;
		const uint64_t prevT = ch == sim.decoder().chars.end() ? 0 : ch->second.prev.tick;
		const int samples = ch == sim.decoder().chars.end() ? 0 : ch->second.samples;
		const int index = sim.charIndex(id);
		double fp[3], facing = NAN;
		const simlink::Route* fr;
		sim.pose(id, fp, facing, fr);	// the drawn body's yaw (the prediction's heading while driven)
		const Characters::DrawState dstate = index < 0 ? Characters::kNotInSet : chars.drawState((size_t)index);
		// The character's blood off the wire (kenshi-k16.2.264.1, the user's 99 -> 75 between two shots): CHAR_MEDICAL at 2.5 Hz, with its own tick, the
		// bleeding rate and the hurt flag, so a drop between frames names the wire sample (and tick) that carried it. `blood -` with no CHAR_MEDICAL yet.
		char med[96] = "blood -";
		if (const simlink::Medical* m = sim.medical(id)) { snprintf(med, sizeof med, "blood %.2f/%.0f bleed %.3f hurt %d medtick %llu", m->blood, m->bloodMax, m->bleed, (int)((m->flags & simlink::Medical::kHurt) != 0), (unsigned long long)m->tick); }
		printf("pos %.4f dt %.5f id %llu tick %.3f newest %llu paused %d sim %.3f %.3f %.3f drawn %.3f %.3f %.3f step %.3f wall %.4f snaps %u samp %llu %llu %d anim [%s] body %s %s wire drive %u strafe %d yaw %.3f facing %.3f\n",
			clockT, dt, (unsigned long long)id, sim.tick(), newest, paused, base[0], base[1], base[2], drawn[0], drawn[1], drawn[2], step, wallT, snapSerial,
			(unsigned long long)curT, (unsigned long long)prevT, samples, index >= 0 ? chars.animSummary((size_t)index).c_str() : "",
			Characters::drawStateName(dstate), med,
			ch == sim.decoder().chars.end() ? 0u : (unsigned)ch->second.cur.drive,
			ch == sim.decoder().chars.end() ? 0 : (int)((ch->second.cur.flags >> 23) & 1) - (int)((ch->second.cur.flags >> 22) & 1),
			ch == sim.decoder().chars.end() ? 0.0 : (double)ch->second.cur.yaw, facing);
	}
}
}
