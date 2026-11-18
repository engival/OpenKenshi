// CHAR_DYNAMIC fwdSpeed/latSpeed are the body's actual motion, not the walker's commanded speed (SIMLINK "CHAR_DYNAMIC").
// The client animates the legs from these two alone, so a body moved by a separation nudge (or any other push) must report the
// speed it was moved at, or it slides with its legs still. On the old report (fighter.forwardSpeed/lateralSpeed, which a push
// never writes) a pushed fighter said 0 while the body moved 20 u/s.
//
// The rig is a bare combat arena with no navmesh (ksim_crowd_spacing's): two characters standing inside each other's comfort
// radius are pushed apart by the depenetration at the head of the step, with no walk of their own. The test pins that
// stepFwd/stepLat equal the body's measured displacement divided by dt, rotated into the character's facing, within 1e-6 u/s;
// the displacement is what the wire carries, so a value that matches it is what the client will animate.

#include "ksim/combat.hpp"

#include "kworld/gamedata.hpp"

#include "test_util.hpp"

#include <cmath>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>
#include <tuple>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;

constexpr double kDt = 0.05;
constexpr double kTolerance = 1.0e-6; // u/s: the same arithmetic on the same doubles, so only rounding differs

struct Rig
{
	const kdata::GameData& gd;
	std::shared_ptr<const CombatData> data;
	CombatArena arena;
	const kdata::Record* servant = nullptr;
	std::uint64_t rng = 7;
	std::uint64_t nextId = 1;

	Rig(const kdata::GameData& g, std::shared_ptr<const CombatData> d, std::uint64_t seed)
		: gd(g)
		, data(d)
		, arena(d, CombatSettings{}, seed)
	{
		for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("CHARACTER")))
			if (r->name == "Escaped Servant")
				servant = r;
		test::require(servant != nullptr, "no Escaped Servant");
	}

	// A player character that picks no fights of its own: it stands where it is put unless a step moves it.
	std::uint64_t spawn(double x, double z, double yaw)
	{
		state::Fighter f = data->makeFighter(gd, servant->stringID, "", rng);
		f.x = x;
		f.z = z;
		f.yaw = yaw;
		f.playerControlled = true;
		const std::uint64_t id = nextId++;
		arena.add(id, std::move(f));
		return id;
	}

	CombatArena::Member& member(std::uint64_t id)
	{
		for (CombatArena::Member& m : arena.membersMutable())
			if (m.id == id)
				return m;
		throw std::runtime_error("no member " + std::to_string(id));
	}
};

// The body's displacement over one step, rotated into the facing the step ended with, units/s.
struct Reported
{
	double fwd = 0.0;
	double lat = 0.0;
};

Reported displacement(CombatArena::Member& m, double beforeX, double beforeZ)
{
	const double dx = m.fighter.x - beforeX;
	const double dz = m.fighter.z - beforeZ;
	const double sin = std::sin(m.fighter.yaw);
	const double cos = std::cos(m.fighter.yaw);
	return { (dx * sin + dz * cos) / kDt, (dx * cos - dz * sin) / kDt };
}

// One step from the given positions; returns the two members' reports and the displacement measured outside the arena.
void pushedPair(const kdata::GameData& gd, std::shared_ptr<const CombatData> data, double x0, double z0, double x1, double z1)
{
	Rig rig(gd, data, 7);
	const double kYaw = 1.5707963267948966; // +x
	const std::uint64_t a = rig.spawn(x0, z0, kYaw);
	const std::uint64_t b = rig.spawn(x1, z1, kYaw);
	CombatArena::Member& ma = rig.member(a);
	CombatArena::Member& mb = rig.member(b);
	const double ax = ma.fighter.x, az = ma.fighter.z;
	const double bx = mb.fighter.x, bz = mb.fighter.z;
	rig.arena.step(kDt);

	for (CombatArena::Member* m : { &ma, &mb })
	{
		const double px = m == &ma ? ax : bx;
		const double pz = m == &ma ? az : bz;
		const Reported measured = displacement(*m, px, pz);
		test::require(std::fabs(m->stepFwd - measured.fwd) <= kTolerance && std::fabs(m->stepLat - measured.lat) <= kTolerance,
			"the reported fwd/lat (" + std::to_string(m->stepFwd) + ", " + std::to_string(m->stepLat)
				+ ") do not match the body's displacement (" + std::to_string(measured.fwd) + ", "
				+ std::to_string(measured.lat) + ")");
		// The old report: the walker never ordered this move, so it said 0 while the push moved the body.
		test::require(m->fighter.forwardSpeed == 0.0 && m->fighter.lateralSpeed == 0.0,
			"the fixture walks, so it does not test a push-only move");
	}
	// The push is a real move, not a rounding artefact.
	const double moved = std::hypot(ma.stepFwd, ma.stepLat);
	test::require(moved > 1.0, "the pair was not pushed apart (reported speed " + std::to_string(moved) + " u/s)");
}

} // namespace

int main()
{
	try
	{
		const kdata::GameData gd = kworld::loadMergedGameData(KENSHI_DATA);
		const std::shared_ptr<const CombatData> data = CombatData::build(gd, KENSHI_DATA);

		// Along the facing: the push is forward/backward, so fwd carries it and lat stays 0.
		pushedPair(gd, data, 0.0, 0.0, 6.0, 0.0);
		// Across the facing: the same push, with the characters facing +x and standing 6 u apart in z, is all lateral.
		{
			Rig rig(gd, data, 7);
			const double kYaw = 1.5707963267948966; // +x
			const std::uint64_t a = rig.spawn(0.0, 0.0, kYaw);
			const std::uint64_t b = rig.spawn(0.0, 6.0, kYaw);
			const double ax = rig.member(a).fighter.x, az = rig.member(a).fighter.z;
			const double bx = rig.member(b).fighter.x, bz = rig.member(b).fighter.z;
			rig.arena.step(kDt);
			for (auto [id, px, pz] : { std::tuple{ a, ax, az }, std::tuple{ b, bx, bz } })
			{
				CombatArena::Member& m = rig.member(id);
				const Reported measured = displacement(m, px, pz);
				test::require(std::fabs(m.stepFwd - measured.fwd) <= kTolerance
					&& std::fabs(m.stepLat - measured.lat) <= kTolerance,
					"a lateral push is not reported as lateral");
				test::require(std::fabs(m.stepFwd) <= kTolerance, "a lateral push leaked into fwd: "
					+ std::to_string(m.stepFwd));
				test::require(std::fabs(m.stepLat) > 1.0, "a lateral push reported no lateral speed");
			}
		}

		std::cout << "ksim_velocity_report: ok\n";
		return 0;
	}
	catch (const std::exception& e)
	{
		std::cerr << "ksim_velocity_report: " << e.what() << "\n";
		return 1;
	}
}
