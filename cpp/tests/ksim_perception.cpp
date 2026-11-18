// Perception (perception.hpp, docs/reference/PERCEPTION_LIVE.md items 17 and 18): the pure rules against the measured
// numbers, the scan on hand-built fighters (ranges by day, night, dust and indoors; cone edges; noise; the sneak meter; memory),
// and the arena with a save and load in the middle of a meter.

#include "ksim/combat.hpp"
#include "ksim/clock.hpp"
#include "ksim/perception.hpp"
#include "ksim/simulation.hpp"

#include "kworld/gamedata.hpp"

#include "test_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>
#include <string>

namespace {

using namespace ksim;
namespace pc = ksim::perception;

void check(bool condition, const std::string& what)
{
	test::require(condition, what);
	std::printf("ok  %s\n", what.c_str());
}

bool near(double a, double b, double tolerance)
{
	return std::abs(a - b) <= tolerance;
}

constexpr double kPi = 3.14159265358979323846;

// A character at (x, z) facing yaw (0 = +z, towards +x).
state::Fighter make(double x, double z, double yaw = 0.0)
{
	state::Fighter f;
	f.x = x;
	f.z = z;
	f.yaw = yaw;
	return f;
}

pc::Environment at(double hour)
{
	pc::Environment env;
	env.dayFraction = hour / 24.0;
	return env;
}

// One scan of `obs` (id 1) over `obs` and `tgt` (id 2) with dt seconds.
struct Pair
{
	state::Fighter obs;
	state::Fighter tgt;
	std::vector<pc::Noticed> noticed;
	pc::Environment env = at(12.0);
	double visionMult = 1.0;

	void scan(double dt = 1.0)
	{
		const std::vector<pc::Other> others = {{1, &obs}, {2, &tgt}};
		pc::scan(1, obs, visionMult, dt, others, env, noticed);
	}
	bool aware() const { return pc::aware(obs, 2); }
	double meter() const
	{
		const state::SightRecord* r = pc::find(obs, 2);
		return r != nullptr ? r->meter : 0.0;
	}
};

// Whether a standing target at (x, z) is noticed by an observer at the origin facing +z within `scans` scans of 1 s.
bool seesStanding(double x, double z, const pc::Environment& env, bool human = true, double mult = 1.0, std::uint64_t obsBuilding = 0,
	std::uint64_t tgtBuilding = 0)
{
	Pair p;
	p.env = env;
	p.visionMult = mult;
	p.obs = make(0.0, 0.0);
	p.obs.animal = !human;
	p.obs.building = obsBuilding;
	p.tgt = make(x, z);
	p.tgt.building = tgtBuilding;
	p.scan();
	return p.aware();
}

void rules()
{
	// ---- daylight: sunrise 05:00, sunset 23:00, 2 h ramps ----
	check(pc::daylight(0.0) == 0.0 && pc::daylight(5.0 / 24.0) == 0.0 && pc::daylight(23.5 / 24.0) == 0.0, "no daylight at night");
	check(near(pc::daylight(6.0 / 24.0), 0.5, 1e-9) && near(pc::daylight(22.0 / 24.0), 0.5, 1e-9), "half daylight one hour into a ramp");
	check(pc::daylight(7.0 / 24.0) == 1.0 && pc::daylight(0.5) == 1.0 && pc::daylight(21.0 / 24.0) == 1.0, "full daylight 07:00-21:00");

	// ---- vision range ----
	check(near(pc::visionRange(1.0, false, at(12.0), true), 1500.0, 1e-9), "clear day: 1500");
	check(near(pc::visionRange(1.0, false, at(1.0), true), 750.0, 1e-9), "night: 750");
	check(near(pc::visionRange(1.0, false, at(6.0), true), 1125.0, 1e-9), "06:00: 1125 (daylight 0.5)");
	check(near(pc::visionRange(2.0, false, at(12.0), true), 3000.0, 1e-9), "race vision range mult 2: 3000");
	pc::Environment dust = at(12.0);
	dust.weatherType = 1;
	dust.weatherStrength = 1.0;
	check(near(pc::visionRange(1.0, false, dust, true), 600.0, 1e-9), "dust storm, human: 600 (measured 600)");
	check(near(pc::visionRange(1.0, false, dust, false), 1500.0, 1e-9), "dust storm, animal: the full 1500 (measured)");
	check(near(pc::visionRange(1.0, false, dust, true, 0.5), 1050.0, 1e-9), "clothing protection 0.5 halves the dust loss");
	dust.weatherType = 5;
	check(near(pc::visionRange(1.0, false, dust, true), 1125.0, 1e-9), "rain, human: 0.75");
	check(near(pc::visionRange(1.0, true, at(12.0), true), 500.0, 1e-9), "indoors: 500");
	check(near(pc::visionRange(1.0, true, at(1.0), true), 250.0, 1e-9), "indoors at night: 250 (the day factor applies to every range)");

	// ---- the cone ----
	check(pc::coneFactor(0.2) == 0.0 && pc::coneFactor(0.19) == 0.0 && pc::coneFactor(-1.0) == 0.0, "cone: 0 at and past the edge (78.5 degrees)");
	check(near(pc::coneFactor(0.6), 0.5, 1e-9) && near(pc::coneFactor(1.0), 1.0, 1e-9), "cone: c = (dot - 0.2) / 0.8");
	check(near(pc::visibility(1.0, false, true, false, 0.0), 0.5, 1e-9), "crouched: v x 0.5");
	check(near(pc::visibility(1.0, true, false, true, 1.0), 0.6, 1e-9), "sneaking, moving, s = 1: v x 0.6");
	check(near(pc::visibility(1.0, true, false, true, 0.0), 1.0, 1e-9), "sneaking with s = 0: no cut");
	check(near(pc::visibility(1.0, true, false, false, 1.0), 0.45, 1e-9), "sneaking and standing still: x 0.75 more (c >= 0.5)");

	// ---- the reach and the light terms ----
	check(near(pc::reach(1500.0, 0.99, 1.0, false), 1485.0, 1e-9), "reach 1485 at v 0.99: the live miss at 1488 u");
	check(near(pc::reach(1500.0, 1.0, 1.0, true), 750.0, 1e-9), "an observer in combat reaches 0.5 R at most");
	check(near(pc::lightTerm(0.0, 0.0), 0.4, 1e-9), "unlit target, dark observer: A 1, B 0.4");
	check(near(pc::lightTerm(1.6, 0.0), 1.76, 1e-9), "target at light 1.6, dark observer: 4.4 x the unlit reach");
	check(near(pc::lightTerm(1.0, 1.0), 1.0, 1e-9), "day: 1");
	check(near(pc::reach(750.0, 0.97, pc::lightTerm(0.0, 0.0), false), 291.0, 1e-9), "night reach 300 v: item 17's 175-264 u are inside it");
	check(pc::characterLight(1.0, false, 0.0) == 1.0 && pc::characterLight(0.6, false, 0.0) == 1.0, "light: 1 for ambient above 0.5");
	check(pc::characterLight(0.0, false, 1.6) == 1.6 && pc::characterLight(0.3, false, 0.4) == 1.0, "light: 2 x ambient + lamps at night");

	// ---- noise ----
	pc::Environment clear = at(12.0);
	state::Fighter walker;
	walker.moveMode = 0;
	check(pc::noiseRange(walker, clear, false, false) == 0.0, "standing still out of combat: no noise");
	check(near(pc::noiseRange(walker, clear, true, false), 375.0, 1e-9), "walking: 375 (measured 365-379)");
	walker.moveMode = 2;
	check(near(pc::noiseRange(walker, clear, true, false), 600.0, 1e-9), "fastest: 600 (measured 600)");
	check(near(pc::noiseRange(walker, clear, false, true), 350.0, 1e-9), "standing in combat: 350");
	walker.sneak = true;
	walker.skills.stealth = 100.0;
	check(near(pc::noiseRange(walker, clear, true, false), 80.0, 1e-9), "sneaking at s = 1: a flat 80 (measured)");
	walker.skills.stealth = 0.0;
	check(near(pc::noiseRange(walker, clear, true, false), 450.0, 1e-9), "sneaking at s = 0: 450");
	check(near(pc::noiseRange(walker, clear, true, true), 600.0, 1e-9), "the sneak flag does nothing in combat");
	walker.sneak = false;
	pc::Environment dusty = clear;
	dusty.weatherType = 1;
	dusty.weatherStrength = 1.0;
	check(near(pc::noiseRange(walker, dusty, true, false), 150.0, 1e-9), "dust: a human's noise x 0.25");
	walker.animal = true;
	check(near(pc::noiseRange(walker, dusty, true, false), 600.0, 1e-9), "dust: an animal's noise is unchanged");

	// ---- the sneak meter's fill time against the live numbers (k1: attn 0.4, s = 1) ----
	check(near(pc::fillTime(1.0, 0.45, 0.4), 9.1, 0.05), "fill time at v 0.45: 9.1 s (live 9.1)");
	check(near(pc::fillTime(1.0, 0.315, 0.4), 11.0, 0.1), "fill time at v 0.31: 11 s (live 11.0)");
	check(near(pc::fillTime(0.0, 1.0, 1.0), 0.5, 1e-9), "fully visible and attentive: 0.5 s");
	check(near(pc::fillTime(1.0, 0.0, 0.0), 22.0, 1e-9), "v -> 0, s = 1, attn 0: 22 s");
	check(near(pc::fillTime(1.0, 0.6, 0.5), 6.25, 1e-9), "attn 0.5, s = 1, v 0.6: 6.25 s");

	// ---- polling ----
	check(pc::scanInterval(10) == 0.5 && pc::scanInterval(5000) == 1.5 && near(pc::scanInterval(480), 1.0, 1e-9), "scan interval 0.5-1.5 s (480 characters per second)");
}

void ranges()
{
	// Day, clear.
	check(seesStanding(0.0, 1400.0, at(12.0)), "day: a standing target ahead at 1400 is seen");
	check(!seesStanding(0.0, 1600.0, at(12.0)), "day: nothing past 1500");
	check(seesStanding(0.0, 2900.0, at(12.0), true, 2.0) && !seesStanding(0.0, 3100.0, at(12.0), true, 2.0), "race mult 2 doubles it");
	// Night: reach 0.4 x 750 x v = 300.
	check(seesStanding(0.0, 250.0, at(1.0)) && !seesStanding(0.0, 320.0, at(1.0)), "night: an unlit target is seen to 300, not beyond");
	// Dust.
	pc::Environment dust = at(12.0);
	dust.weatherType = 1;
	dust.weatherStrength = 1.0;
	check(seesStanding(0.0, 550.0, dust) && !seesStanding(0.0, 700.0, dust), "dust: humans see to 600");
	check(seesStanding(0.0, 1000.0, dust, false), "dust: animals still see 1000");
	// Indoors: a building does not shorten the sight (bq7_sight_region.md: the Hub's Bar reads region 0); only walls do (townRange below).
	check(seesStanding(0.0, 400.0, at(12.0), true, 1.0, 7, 7) && seesStanding(0.0, 600.0, at(12.0), true, 1.0, 7, 7), "indoors, unwalled: the full reach (600 u seen)");
	check(!seesStanding(0.0, 100.0, at(12.0), true, 1.0, 7, 0) && !seesStanding(0.0, 100.0, at(12.0), true, 1.0, 0, 7) && !seesStanding(0.0, 100.0, at(12.0), true, 1.0, 7, 8),
		"no sight between different buildings, or inside and out");
	// A lit target at night is seen from 4.4 x as far (but never past the scan radius, 750 at night).
	{
		Pair p;
		p.env = at(1.0);
		p.env.lamps = [](double, double z, std::uint64_t) { return z > 100.0 ? 1.6 : 0.0; };
		p.tgt = make(0.0, 700.0);
		p.scan();
		check(p.aware(), "night: a target standing by a lamp (light 1.6) is seen at 700 (reach 1.76 x 750)");
		check(!seesStanding(0.0, 700.0, at(1.0)), "night: the same target unlit is not");
		p.tgt.z = 800.0;
		Pair q = p;
		q.obs.sight.clear();
		q.scan();
		check(!q.aware(), "night: the lamp does not extend the scan radius (750)");
	}
	// The cone.
	const auto ring = [](double angleDeg, double dist, double hour = 12.0)
	{
		const double a = angleDeg * kPi / 180.0;
		return seesStanding(std::sin(a) * dist, std::cos(a) * dist, at(hour));
	};
	check(ring(60.0, 500.0) && !ring(60.0, 600.0), "cone: at 60 degrees (c 0.375) the reach is 562");
	check(!ring(79.0, 100.0) && !ring(120.0, 100.0) && !ring(180.0, 100.0), "cone: nothing past 78.5 degrees, behind never");
	check(ring(70.0, 200.0) && !ring(70.0, 300.0), "cone: at 70 degrees (c 0.178) the reach is 267");
	// Line of sight.
	{
		Pair p;
		p.env.lineOfSight = [](std::uint64_t, const state::Fighter&, std::uint64_t, const state::Fighter&) { return false; };
		p.tgt = make(0.0, 300.0);
		p.scan();
		check(!p.aware(), "a blocked line of sight sees nothing");
	}
	// A sleeping observer has cone 0.
	{
		Pair p;
		p.obs.asleep = true;
		p.tgt = make(0.0, 100.0);
		p.scan();
		check(!p.aware(), "a sleeping observer does not see a standing target");
	}
}

// The notice range by place (kinspect findings/bq7_sight_region.md, correcting s73_static.md section 1): 500 only for an observer whose navmesh
// gate-region id is > 0, i.e. inside the walls of a gated town; 1500 x the race's `vision range mult` everywhere else, the unwalled Hub and
// the inside of its buildings included; x(0.5 + 0.5 daylight) either way (so x0.5 at night). The sim models a walled region by the
// town's gate sites (APPROXIMATION, perception.hpp).
void townRange()
{
	using Region = pc::Environment::WalledRegion;
	// A walled town round the observer: gates 300 u north (outside = +z) and 300 u south (outside = -z), radius 300.
	Region walls;
	walls.radius = 300.0;
	walls.gates = {{0.0, 300.0, 0.0, 1.0}, {0.0, -300.0, 0.0, -1.0}};
	pc::Environment walled = at(12.0);
	walled.walled = {walls};
	pc::Environment wild = at(12.0);
	check(walled.inWalls(0.0, 0.0) && walled.inWalls(0.0, 250.0) && !walled.inWalls(0.0, 350.0) && !walled.inWalls(0.0, 300.5),
		"Environment::inWalls: between the gates only; the street beyond a gate is outside");
	check(!walled.inWalls(250.0, 250.0) && !wild.inWalls(0.0, 0.0), "outside the radius, and no walls at all: not inside");

	// ---- the range itself ----
	check(near(pc::visionRange(1.0, true, walled, true), 500.0, 1e-9), "walled by day: 500");
	check(near(pc::visionRange(2.0, true, walled, true), 500.0, 1e-9), "walled: the race's vision range mult does not scale the 500");
	check(near(pc::visionRange(0.5, false, wild, true), 750.0, 1e-9), "unwalled, race vision range mult 0.5: 750");
	check(near(pc::visionRange(1.0, true, at(1.0), true), 250.0, 1e-9), "walled at night: 250 (x0.5)");
	check(near(pc::visionRange(1.0, false, at(1.0), true), 750.0, 1e-9), "unwalled at night: 750 (x0.5)");

	// ---- the scan: a standing target straight ahead ----
	check(seesStanding(0.0, 450.0, walled) && !seesStanding(0.0, 550.0, walled), "scan inside walls: sees 450 u ahead, not 550");
	check(seesStanding(0.0, 1400.0, wild) && !seesStanding(0.0, 1600.0, wild), "scan unwalled: sees 1400 u ahead, not 1600");
	check(seesStanding(0.0, 700.0, wild) && !seesStanding(0.0, 700.0, walled), "the same target at 700 u: seen unwalled, not inside walls");
	check(!seesStanding(0.0, 700.0, walled, true, 2.0) && seesStanding(0.0, 1400.0, wild, true, 2.0), "race mult 2: the open reach doubles, the walled one does not");
	check(seesStanding(0.0, 600.0, wild, true, 0.5) && !seesStanding(0.0, 800.0, wild, true, 0.5), "race mult 0.5 unwalled: 750");
	// Just outside the north gate (the live outlaws read 0): the observer at the origin, the town's gates south of it.
	Region north;
	north.x = 0.0;
	north.z = -400.0;
	north.radius = 300.0;
	north.gates = {{0.0, -100.0, 0.0, 1.0}, {0.0, -700.0, 0.0, -1.0}};
	pc::Environment outside = at(12.0);
	outside.walled = {north};
	check(!outside.inWalls(0.0, 0.0) && outside.inWalls(0.0, -400.0), "a walled town with the observer just outside its gate");
	check(seesStanding(0.0, 700.0, outside), "just outside the gate: the full 1500 reach (live: outlaws outside read 0)");
	// No gates, no walls: the Hub. Its Bar interior is 1500 too (the live reading was 0 inside the Bar).
	check(seesStanding(0.0, 700.0, wild, true, 1.0, 5, 5), "unwalled, inside a building with the target: still the 1500 reach");
}

void noise()
{
	// A target that stands behind and stays still is never noticed, however long we wait.
	{
		Pair p;
		p.tgt = make(0.0, -30.0);
		for (int i = 0; i < 60; ++i)
			p.scan(1.0);
		check(!p.aware(), "stationary behind the observer: no noise, never noticed");
	}
	// A running target 40 behind is noticed within a few rolls.
	{
		Pair p;
		p.tgt = make(0.0, -40.0);
		p.tgt.velZ = -10.0;
		p.tgt.moveMode = 2;
		int scans = 0;
		while (!p.aware() && scans < 40)
		{
			p.scan(1.0);
			++scans;
		}
		check(p.aware() && scans <= 10, "a runner 40 behind is heard (noise 600 x 0.5): noticed after " + std::to_string(scans) + " scans");
	}
	// A sneaker with s = 1 makes 80 x 0.5 = 40: at 100 behind never, at 5 behind soon.
	{
		Pair p;
		p.tgt = make(0.0, -100.0);
		p.tgt.velZ = -1.0;
		p.tgt.sneak = true;
		p.tgt.skills.stealth = 100.0;
		for (int i = 0; i < 60; ++i)
			p.scan(1.0);
		check(!p.aware(), "a sneaker 100 behind is not heard (noise 40)");
	}
	// The roll is throttled to once per 2 s per pair: scans of 0.5 s repeat the verdict.
	{
		Pair p;
		p.tgt = make(0.0, -150.0);
		p.tgt.velZ = -10.0;
		p.tgt.moveMode = 2;
		for (int i = 0; i < 4; ++i)
			p.scan(0.5);
		const state::SightRecord* r = pc::find(p.obs, 2);
		check(r != nullptr && r->rolls <= 2, "the noise roll runs at most once per 2 s per pair");
	}
}

void meter()
{
	// Live k1: sneaking target with s = 1 in front of an observer with attn 0.4, scans every ~1.1 s.
	Pair p;
	p.obs.attention = 0.4;
	p.tgt = make(0.0, 250.0);
	p.tgt.sneak = true;
	p.tgt.skills.stealth = 100.0;
	p.tgt.velZ = 10.0; // moving: v = 0.6, fill = (0.5 + 5 x 0.4) x 2.8 = 7.0
	double t = 0.0;
	double noticedAt = -1.0;
	while (t < 40.0)
	{
		p.scan(1.1);
		t += 1.1;
		if (p.aware())
		{
			noticedAt = t;
			break;
		}
	}
	check(noticedAt > 0.0 && near(noticedAt, 1.1 + 7.0 + 1.1, 1.2), "moving sneaker: noticed after fill time + one scan (" + std::to_string(noticedAt) + " s)");
	check(p.noticed.size() == 1 && p.noticed[0].observer == 1 && p.noticed[0].target == 2, "one Noticed transition");
	p.scan(1.1);
	check(p.noticed.size() == 1, "no second Noticed while it stays aware");

	// The live band: standing (v 0.45) -> fill 9.1; the first scan creates the spot, so noticed within 9.1 + 2 scans.
	{
		Pair q;
		q.obs.attention = 0.4;
		q.tgt = make(0.0, 250.0);
		q.tgt.sneak = true;
		q.tgt.skills.stealth = 100.0;
		double clock = 0.0;
		double at = -1.0;
		while (clock < 40.0)
		{
			q.scan(1.1);
			clock += 1.1;
			if (q.aware())
			{
				at = clock;
				break;
			}
		}
		const double fromFirst = at - 1.1;
		check(at > 0.0 && fromFirst >= 9.1 && fromFirst <= 9.1 + 2.3, "standing sneaker (v 0.45, fill 9.1): noticed " + std::to_string(fromFirst) + " s after the first scan (live 9.4-15.2)");
	}
	// An attentive observer (attn 1) notices a sneaker faster, an inattentive one (0.0) never.
	{
		Pair q;
		q.obs.attention = 0.0;
		q.tgt = make(0.0, 250.0);
		q.tgt.sneak = true;
		q.tgt.skills.stealth = 100.0;
		for (int i = 0; i < 60; ++i)
			q.scan(1.1);
		check(!q.aware() && q.meter() == 0.0, "attention 0: the meter never fills");
	}
	// An observer in combat has attention 0 as well.
	{
		Pair q;
		q.tgt = make(0.0, 250.0);
		q.tgt.sneak = true;
		q.tgt.skills.stealth = 100.0;
		q.obs.target = 99; // fighting someone else
		for (int i = 0; i < 30; ++i)
			q.scan(1.1);
		check(!q.aware(), "an observer in combat does not fill the meter");
	}
	// Line of sight lost: the meter is wiped. Out of the cone: it drains by dt / 260.
	{
		Pair q;
		q.tgt = make(0.0, 250.0);
		q.tgt.sneak = true;
		q.tgt.skills.stealth = 100.0;
		q.tgt.velZ = 5.0;
		for (int i = 0; i < 4; ++i)
			q.scan(1.0);
		const double built = q.meter();
		check(built > 0.1 && built < 1.0, "the meter builds while the cone and the line of sight hold (" + std::to_string(built) + ")");
		q.tgt.x = 0.0;
		q.tgt.z = -250.0; // behind: out of the cone
		q.scan(1.0); // the last scan's spot was active: this interval still fills
		const double filled = q.meter();
		q.scan(10.0);
		check(near(q.meter(), filled - 10.0 / 260.0, 1e-9), "out of the cone: the meter drains by dt / 260 (it barely moves)");
		q.tgt.z = 250.0;
		q.scan(1.0);
		bool blocked = true;
		q.env.lineOfSight = [&](std::uint64_t, const state::Fighter&, std::uint64_t, const state::Fighter&) { return !blocked; };
		q.scan(1.0);
		check(pc::find(q.obs, 2) == nullptr || pc::find(q.obs, 2)->meter == 0.0, "line of sight lost: the meter is erased");
	}
	// A target that stops sneaking is known at once (cone + reach + line of sight).
	{
		Pair q;
		q.tgt = make(0.0, 250.0);
		q.tgt.skills.stealth = 100.0;
		q.scan(1.0);
		check(q.aware(), "a non-sneaking target in reach needs no meter");
	}
}

// The force-notice writes state 1 whatever the record was (140858f40: +0x38 = 1 on every call; kenshi-h2v.27.10): a suspicious (state 2)
// record turns red, and so does a stale seen one. True comes back for a promotion, false for a record that was already noticed.
void forceNotice()
{
	Pair p;
	p.tgt = make(0.0, 200.0);
	state::SightRecord stale;
	stale.target = 7;
	stale.seen = true;
	stale.state = 2;
	p.obs.sight.push_back(stale);
	check(perception::notice(p.obs, 7) && perception::find(p.obs, 7)->state == 1, "a stale seen record is promoted to state 1");
	check(!perception::notice(p.obs, 7) && perception::find(p.obs, 7)->state == 1, "a noticed record stays noticed, no second transition");
	state::SightRecord yellow;
	yellow.target = 8;
	yellow.active = true;
	yellow.state = 2;
	p.obs.sight.push_back(yellow);
	std::sort(p.obs.sight.begin(), p.obs.sight.end(), [](const state::SightRecord& a, const state::SightRecord& b) { return a.target < b.target; });
	check(perception::notice(p.obs, 8) && perception::find(p.obs, 8)->state == 1 && perception::find(p.obs, 8)->seen, "a yellow record turns red");
}

void memory()
{
	for (const bool fighting : {false, true})
	{
		Pair p;
		p.tgt = make(0.0, 200.0);
		p.scan(1.0);
		check(p.aware(), "seen");
		p.tgt.z = -200.0; // behind and still: unseen, silent
		if (fighting)
			p.obs.target = 99; // the observer is in combat (another target)
		bool awareAt29 = false;
		bool awareAt59 = false;
		bool awareAt61 = true;
		for (int s = 1; s <= 62; ++s)
		{
			p.scan(1.0);
			if (s == 29)
				awareAt29 = p.aware();
			if (s == 59)
				awareAt59 = p.aware();
			if (s == 61)
				awareAt61 = p.aware();
		}
		if (fighting)
			check(awareAt59 && !awareAt61, "in combat the memory is 60 s");
		else
			check(awareAt29 && !awareAt59, "unseen for 30 s: forgotten");
	}
}

void determinism()
{
	const auto run = []()
	{
		std::vector<state::SightRecord> trail;
		Pair p;
		p.obs.attention = 0.4;
		p.tgt = make(0.0, -60.0);
		p.tgt.velZ = -4.0;
		p.tgt.moveMode = 2;
		for (int i = 0; i < 40; ++i)
		{
			p.tgt.z -= 0.3;
			p.scan(0.9);
			if (const state::SightRecord* r = pc::find(p.obs, 2))
				trail.push_back(*r);
		}
		return trail;
	};
	const auto a = run();
	const auto b = run();
	bool same = a.size() == b.size();
	for (std::size_t i = 0; same && i < a.size(); ++i)
		same = a[i].seen == b[i].seen && a[i].meter == b[i].meter && a[i].rolls == b[i].rolls && a[i].sighted == b[i].sighted
			&& a[i].sinceRoll == b[i].sinceRoll;
	check(same && !a.empty(), "two identical runs give identical records");
}

// The arena inside a Simulation: the sneaker is noticed at the same tick with and without a save and load in the middle of the meter.
// The Simulation fills the arena's walled regions from the gate sites: the Hub (no gates) reads 1500, a walled town 500 inside its gates.
void simulationTowns()
{
	const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);
	const auto gd = std::make_shared<const kdata::GameData>(kworld::loadMergedGameData(order));
	SpawnOptions options;
	options.seed = 5;
	options.wildernessByDensity = false;
	Simulation sim = Simulation::newGame(KENSHI_DATA, order, options, nullptr, RegionConfig{});
	const std::shared_ptr<const CombatData> data = CombatData::build(*gd, KENSHI_DATA);
	sim.enableCombat(data, CombatSettings{});
	std::uint64_t rng = 3;
	for (const kdata::Record* r : gd->byType(kworld::itemTypeByName("CHARACTER")))
	{
		if (r->name == "Escaped Servant")
		{
			state::Fighter f = data->makeFighter(*gd, r->stringID, "", rng);
			f.x = -50000.0;
			f.z = -1700.0;
			sim.spawnFighter(std::move(f));
			break;
		}
	}
	sim.run(2);
	const pc::Environment& env = sim.combat()->environment();
	std::printf("    %zu walled regions in the perception environment\n", env.walled.size());
	check(!env.walled.empty(), "the Simulation lists the walled regions for perception");
	// The Hub has no gates: the Bar (-50642, -1030), its street and the bandits' den are all open ground (regression: 1500).
	check(!env.inWalls(-50642.0, -1030.0) && !env.inWalls(-50500.0, -1000.0) && !env.inWalls(-48880.0, -823.0), "the Hub (no gates) is not a walled region");
	// The United Cities walled town near (53500, -94500): gates north (+z) and south (-z) at that town (world.cpp gateNormal).
	bool found = false;
	for (const pc::Environment::WalledRegion& r : env.walled)
	{
		if (std::hypot(r.x - 53500.0, r.z + 94500.0) < 3000.0 && r.gates.size() >= 2)
		{
			found = true;
			std::printf("    walled town at (%.0f, %.0f), radius %.0f, %zu gates\n", r.x, r.z, r.radius, r.gates.size());
			check(env.inWalls(r.x, r.z), "inside the walled town's centre: 500");
			for (const pc::Environment::WalledRegion::Gate& g : r.gates)
				check(!env.inWalls(g.x + g.nx * 40.0, g.z + g.nz * 40.0), "40 u outside a gate: not inside the walls (1500)");
		}
	}
	check(found, "a walled town with two gates near the United Cities one");
	check(!env.inWalls(0.0, -200000.0), "far outside the map is in no region");
}

void arena()
{
	const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);
	const auto gd = std::make_shared<const kdata::GameData>(kworld::loadMergedGameData(order));
	const std::shared_ptr<const CombatData> data = CombatData::build(*gd, KENSHI_DATA);
	std::string servant;
	for (const kdata::Record* r : gd->byType(kworld::itemTypeByName("CHARACTER")))
	{
		if (r->name == "Escaped Servant")
			servant = r->stringID;
	}
	test::require(!servant.empty(), "no Escaped Servant");
	CombatSettings settings;
	settings.substeps = 2;

	const auto build = [&]()
	{
		Simulation sim = Simulation::load(KENSHI_DATA, 5);
		sim.setClockTick(tickAt(kNewGameDay, kNewGameHour, kNewGameMinute));
		sim.enableCombat(data, settings);
		std::uint64_t rng = 3;
		state::Fighter watcher = data->makeFighter(*gd, servant, "", rng);
		watcher.team = 1;
		watcher.x = 0.0;
		watcher.z = 0.0;
		watcher.yaw = 0.0;
		watcher.attention = 0.4;
		state::Fighter sneaker = data->makeFighter(*gd, servant, "", rng);
		sneaker.team = 0;
		sneaker.x = 0.0;
		sneaker.z = 250.0;
		sneaker.sneak = true;
		sneaker.skills.stealth = 100.0;
		sim.spawnFighter(std::move(watcher));
		sim.spawnFighter(std::move(sneaker));
		return sim;
	};
	struct Trace
	{
		std::uint64_t noticedTick = 0;
		std::uint64_t hash = 0;
		double meter = 0.0;
		std::uint64_t targetTick = 0;
	};
	const auto drive = [&](Simulation& sim, Trace& trace, std::uint64_t ticks)
	{
		sim.setCombatObserver([&](const CombatEvent& e)
		{
			if (e.kind == CombatEvent::Kind::Noticed && e.who == 1 && e.other == 2 && trace.noticedTick == 0)
				trace.noticedTick = sim.clockTick();
		});
		for (std::uint64_t i = 0; i < ticks; ++i)
		{
			sim.tick();
			if (trace.targetTick == 0 && sim.combat()->find(1)->target != 0)
				trace.targetTick = sim.clockTick();
		}
	};
	Trace straight;
	{
		Simulation sim = build();
		drive(sim, straight, 20 * 60);
		straight.hash = sim.stateHash();
	}
	check(straight.noticedTick != 0, "the arena's watcher notices the sneaker (tick " + std::to_string(straight.noticedTick) + ")");
	const double seconds = static_cast<double>(straight.noticedTick - tickAt(kNewGameDay, kNewGameHour, kNewGameMinute)) * kTickSeconds;
	check(seconds > 8.0 && seconds < 14.0, "noticed after " + std::to_string(seconds) + " s (fill 8 s + a scan)");
	check(straight.targetTick != 0 && straight.targetTick <= straight.noticedTick + 40, "a hostile it is aware of becomes its target within 2 s");

	Trace split;
	{
		Simulation sim = build();
		drive(sim, split, 20 * 5); // 5 s: mid-meter (about 0.5)
		const state::Fighter* w = sim.combat()->find(1);
		const state::SightRecord* r = pc::find(*w, 2);
		check(r != nullptr && r->meter > 0.2 && r->meter < 0.9 && !r->seen, "mid-meter at the save (" + std::to_string(r != nullptr ? r->meter : -1.0) + ")");
		std::ostringstream out;
		sim.save(out);
		std::istringstream in(out.str());
		Simulation restored = Simulation::restore(World::empty(), in, RegionConfig{}, true); // forced: the empty world has no load order to match
		restored.setGameData(gd);
		restored.enableCombat(data, settings);
		const state::SightRecord* rr = pc::find(*restored.combat()->find(1), 2);
		check(rr != nullptr && rr->meter == r->meter && rr->fill == r->fill, "the meter survives the save");
		drive(restored, split, 20 * 55);
		split.hash = restored.stateHash();
	}
	check(split.noticedTick == straight.noticedTick && split.targetTick == straight.targetTick, "noticed (and the target picked) at the same tick after a save and load mid-meter");
	check(split.hash == straight.hash, "the state hash after a save and load mid-meter equals the straight run");
}

} // namespace

int main()
{
	try
	{
		rules();
		ranges();
		townRange();
		noise();
		meter();
		memory();
		forceNotice();
		determinism();
		simulationTowns();
		arena();
		std::printf("ksim_perception ok\n");
	} catch (const std::exception& error) {
		std::fprintf(stderr, "error: %s\n", error.what());
		return 1;
	}
	return 0;
}
