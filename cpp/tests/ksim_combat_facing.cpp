// Combat facing (kinspect s70_static.md section 3, move_kinematics.md; task AC). The body slerps towards its target at 4 rad/s in the
// combat-ready stance (AnimationClassHuman::vf8, anim+0x244 == 1), 20 rad/s otherwise. A technique start used to snap the yaw to the
// opponent; now the technique's AIM (Fighter::aimYaw, the look hand) is set there and the body yaw turns at knav::kTurnRateCombat
// while a technique plays, converging on the opponent (the swing reads the aim, perception and the block behind tests read the
// body: ENGAGEMENT.md 9.4). Pins, on a bare arena with no navmesh:
//  - the body yaw of a fighter with a technique running changes by at most 4 rad/s x dt per tick (no snap);
//  - a fighter started facing away from its opponent turns round and ends up facing it (and keeps facing it through the swing).

#include "ksim/combat.hpp"

#include "kworld/gamedata.hpp"

#include "test_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;

constexpr double kDt = 0.05;
constexpr double kPi = 3.14159265358979323846;

double wrap(double a)
{
	while (a > kPi)
		a -= 2.0 * kPi;
	while (a < -kPi)
		a += 2.0 * kPi;
	return a;
}

} // namespace

int main()
{
	try
	{
		const kdata::GameData gd = kworld::loadMergedGameData(KENSHI_DATA);
		const std::shared_ptr<const CombatData> data = CombatData::build(gd, KENSHI_DATA);
		const kdata::Record* servant = nullptr;
		for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("CHARACTER")))
		{
			if (r->name == "Escaped Servant")
				servant = r;
		}
		test::require(servant != nullptr, "no Escaped Servant");

		for (const double startYaw : {-kPi / 2.0, kPi, 0.0})
		{
			CombatArena arena(data, CombatSettings{}, 21);
			std::uint64_t rng = 3;
			state::Fighter a = data->makeFighter(gd, servant->stringID, "", rng);
			a.x = 0.0;
			a.z = 0.0;
			a.yaw = startYaw; // the opponent is at +x: bearing pi/2
			a.team = 1;
			state::Fighter b = data->makeFighter(gd, servant->stringID, "", rng);
			b.x = 30.0;
			b.z = 0.0;
			b.team = 0;
			b.playerControlled = true; // stands still
			a.playerControlled = false;
			a.orderTarget = 2;
			arena.add(1, std::move(a));
			arena.add(2, std::move(b));

			double prev = arena.find(1)->yaw;
			double maxStep = 0.0;
			int swingTicks = 0;
			int facedAt = -1; // technique ticks until the bearing error first drops under 0.05 rad
			bool swung = false;
			for (int i = 0; i < 400; ++i)
			{
				arena.step(kDt);
				const state::Fighter& fa = *arena.find(1);
				const state::Fighter& fb = *arena.find(2);
				const double step = std::fabs(wrap(fa.yaw - prev));
				prev = fa.yaw;
				if (fa.technique >= 0)
				{
					++swingTicks;
					maxStep = std::max(maxStep, step);
					swung = true;
					if (facedAt < 0 && std::fabs(wrap(std::atan2(fb.x - fa.x, fb.z - fa.z) - fa.yaw)) < 0.05)
						facedAt = swingTicks;
				}
			}
			test::require(swung, "the fighter never started a technique");
			test::require(maxStep <= 4.0 * kDt + 1.0e-9, "the yaw turned faster than 4 rad/s x dt in one tick while a technique ran");
			const double off = std::fabs(wrap(kPi / 2.0 - startYaw));
			test::require(facedAt > 0, "the yaw never converged on the opponent");
			test::require(facedAt <= static_cast<int>(std::ceil(off / (4.0 * kDt))) + 3, "the yaw converged slower than 4 rad/s allows plus a few ticks of margin");
			test::require(facedAt >= static_cast<int>(std::floor(off / (4.0 * kDt))) - 1, "the yaw converged faster than 4 rad/s (a snap)");
			std::printf("ok  facing from %.2f rad: %d technique ticks, max turn %.4f rad/tick (limit %.4f), faced after %d technique ticks\n",
				startYaw, swingTicks, maxStep, 4.0 * kDt, facedAt);
		}
	}
	catch (const std::exception& e)
	{
		std::fprintf(stderr, "ksim_combat_facing: %s\n", e.what());
		return 1;
	}
	return 0;
}
