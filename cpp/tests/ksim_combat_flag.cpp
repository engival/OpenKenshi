// kenshi-k16.6.164: the order's first ticks (a hub / between fighter with the target over 150 u away) are not combat either, sneaking or standing.
// kenshi-k16.6.129: CHAR_DYNAMIC flags bit 0 (combat mode) on the wire follows the original's is-combat rule: clear through the approach to an attack
// target (state 11, jog / run lower clips), set from the hub / circle / wait / attack / defend states or a technique on (COMBAT_ANIM.md 3). A fighter
// ordered to attack a target 400 u away on open ground, watched through a Session as a client sees it. Needs the world bake (argv[1]; exit 77 without).
#include "ksim/combat.hpp"
#include "ksim/combat_data.hpp"
#include "ksim/command.hpp"
#include "ksim/simlink.hpp"
#include "ksim/simulation.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/coords.hpp"
#include "kworld/gamedata.hpp"
#include "nav_source.hpp"
#include "sim_access.hpp"
#include "test_util.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

using namespace ksim;

namespace ks = ksim::simlink;

namespace {

constexpr double kOriginX = -50750.0;
constexpr double kOriginZ = -1100.0;

template <typename T>
T at(const std::vector<std::uint8_t>& b, std::size_t pos)
{
	T v;
	std::memcpy(&v, b.data() + pos, sizeof(T));
	return v;
}

} // namespace

int main(int argc, char** argv)
{
	if (argc < 2 || !std::filesystem::is_directory(argv[1]))
	{
		std::cerr << "ksim_combat_flag: SKIPPED: no navworld bake\n";
		return 77;
	}
	try
	{
		const kdata::GameData gd = kworld::loadMergedGameData(KENSHI_DATA);
		const std::shared_ptr<const CombatData> data = CombatData::build(gd, KENSHI_DATA);
		Simulation sim = Simulation::load(KENSHI_DATA, 5, RegionConfig{});
		sim.enableCombat(data, CombatSettings{});
		const kworld::ZoneCell zone = kworld::zoneCellOf(kOriginX, kOriginZ);
		std::vector<kworld::ZoneCell> zones;
		for (int dz = -1; dz <= 1; ++dz)
			for (int dx = -1; dx <= 1; ++dx)
				zones.push_back({zone.x + dx, zone.z + dz});
		sim.enableNavigation(test::openWorldNav(argv[1], zones));
		sim.setClockTick(static_cast<std::uint64_t>(kTicksPerGameDay * 0.5));
		sim.enqueue(Command::setPlayerAreas({{kOriginX, kOriginZ}}));
		sim.run(2);
		const kdata::Record* servant = nullptr;
		for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("CHARACTER")))
			if (r->name == "Escaped Servant")
				servant = r;
		test::require(servant != nullptr, "no Escaped Servant");
		std::uint64_t rng = 7;
		const auto spawn = [&](double x, double z, int team, bool player, bool sneak = false)
		{
			state::Fighter f = data->makeFighter(gd, servant->stringID, "", rng);
			f.x = kOriginX + x;
			f.z = kOriginZ + z;
			f.y = sim.navigation()->walker().snap({f.x, 0.0, f.z}, 2000.0)->y;
			f.team = team;
			f.playerControlled = player;
			f.sneak = sneak;
			return sim.spawnFighter(std::move(f)).get<state::Id>()->value;
		};
		std::uint64_t attacker = spawn(0.0, 0.0, 0, true);
		std::uint64_t target = spawn(0.0, 400.0, 1, false);
		const std::uint64_t farTarget = target; // 1000+ u from the second pair

		// One session as a client: the attacker's CHAR_DYNAMIC flags after every tick.
		ks::ServerContext context;
		const auto shared = std::make_shared<const kdata::GameData>(gd);
		sim.setGameData(shared);
		context.gameData = shared;
		ks::Session session(sim, context);
		std::vector<std::uint8_t> bytes;
		ks::BinaryWriter w(bytes);
		session.begin(w);
		std::vector<std::uint8_t> hello;
		ks::BinaryWriter h(hello);
		h.begin(ks::kHello);
		h.u32("magic", ks::kMagic);
		h.u16("protocol", ks::kProtocol);
		h.u8("role", 0);
		h.u64("dataHash", context.dataHash);
		h.str("name", "test");
		h.end();
		test::require(session.onFrame(ks::kHello, hello.data() + 6, hello.size() - 6, w), "hello refused");
		session.sendTick(w, true);
		std::uint32_t wireFlags = 0;
		const auto parse = [&]
		{
			for (std::size_t p = 0; p < bytes.size();)
			{
				const std::uint32_t len = at<std::uint32_t>(bytes, p);
				const std::uint16_t type = at<std::uint16_t>(bytes, p + 4);
				const std::size_t b = p + 6;
				p += 4 + len;
				if (type == ks::kDynamic && at<std::uint64_t>(bytes, b + 8) == attacker)
					wireFlags = at<std::uint32_t>(bytes, b + 40);
			}
			bytes.clear();
		};
		parse();

		// A sneaking and a standing attacker, ordered onto the target 300 u away: the order's first ticks (the target set, the state not yet Approach)
		// are not combat either (kenshi-k16.6.164): clear on every tick from the order until in range, set once in range.
		for (const bool sneak : {false, true})
		{
			if (sneak) // a second pair, off to the side
			{
				attacker = spawn(0.0, -1500.0, 0, true, true);
				target = spawn(0.0, -1200.0, 1, false);
				}

			sim.run(400); // idle a while first: the order lands on a settled character
			sim.enqueue(Command::fighterAttack(state::Id{attacker}, state::Id{target}));
			int approachTicks = 0;
			int approachCombat = 0; // ticks of Approach with the wire's combat bit set
			int farCombat = 0; // ticks with the target over 150 u away and the bit set
			int firstCombat = -1;
			double distAtCombat = 0.0;
			for (int tick = 0; tick < 20 * 20; ++tick)
			{
				sim.tick();
				ks::BinaryWriter out(bytes);
				session.sendTick(out, false);
				parse();
				const state::Fighter* a = sim.combat()->find(attacker);
				const state::Fighter* t = sim.combat()->find(target);
				const double dist = std::hypot(a->x - t->x, a->z - t->z);
				const bool combat = (wireFlags & ks::kFlagCombat) != 0;
				if (a->state == state::CombatState::Approach && a->target != 0 && a->technique < 0)
				{
					++approachTicks;
					approachCombat += combat ? 1 : 0;
				}
				farCombat += dist > 150.0 && combat ? 1 : 0;
				if (combat && firstCombat < 0)
				{
					firstCombat = tick;
					distAtCombat = dist;
				}
			}
			std::printf("sneak %d: approach ticks %d (wire combat bit set on %d), far ticks with the bit %d; first set at tick %d, %.1f u from the target\n", sneak, approachTicks, approachCombat, farCombat, firstCombat, distAtCombat);
			test::require(approachTicks >= 20, "the attacker approaches for at least a second (state 11): " + std::to_string(approachTicks));
			test::require(approachCombat == 0, "kFlagCombat is clear on the wire through the approach");
			test::require(farCombat == 0, "kFlagCombat is clear while the target is over 150 u away");
			test::require(firstCombat >= 0, "kFlagCombat comes on once the attacker is in range");
		}
		// The pre-approach states with a far target (a Hub / Between fighter the order has landed on, before the move-in starts): not combat.
		// Set by hand, then written without a tick, since the state machine leaves them in the same tick.
		state::Fighter* held = fighterOf(sim, attacker);
		for (const state::CombatState pre : {state::CombatState::Hub, state::CombatState::Between})
		{
			held->state = pre;
			held->target = farTarget;
			held->technique = -1;
			ks::BinaryWriter out(bytes);
			session.sendTick(out, false);
			parse();
			test::require((wireFlags & ks::kFlagCombat) == 0, "kFlagCombat is clear in the hub / between with the target 300 u away");
			held->target = target;
			held->x = sim.combat()->find(target)->x;
			held->z = sim.combat()->find(target)->z + 40.0;
			session.sendTick(out, false);
			parse();
			test::require((wireFlags & ks::kFlagCombat) != 0, "kFlagCombat is set in the hub / between with the target in reach");
		}
		return 0;
	} catch (const std::exception& e)
	{
		std::cerr << "ksim_combat_flag: " << e.what() << "\n";
		return 1;
	}
}
