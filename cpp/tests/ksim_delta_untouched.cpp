// An untouched squad's demotion and promotion leaves its members as they were before the member delta store (kenshi-k16.6.51): a member that
// differs in nothing from its template build stores no record and is rebuilt exactly as before. The subject squad and the control squad of the
// scene are hashed member by member (the persistent state and the placement) after the round trip; the pin was taken on the base (ksim
// efb91c7e) with this very scenario, where it passes too. The whole world's hash is not compared: other Hub squads were in fights and
// looted before the walk, and keep that now (that is the delta store), where the base rebuilt them from the template.

#include "delta_scene.hpp"

#include <cstdio>
#include <iostream>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace delta_scene;

// Repinned for kenshi-k16.6.253 (the follow slot's direct steer only on a line a body fits, and not left at 0 u along a wall: 6893984046434549243 before; followers' placement moves, the delta store is untouched by it).
// Repinned for kenshi-k16.6.182 / k16.6.200 (the walk's heading memory, saved from v109): the members' placement after the walk moves; the delta
// store is untouched by it, and the hash is the same on navworld v12 and v13 (was 2317286899095492733 for kenshi-k16.6.139's formation direct steer,
// 12384742747856631192 from the base efb91c7e).
constexpr std::uint64_t kPinned = 3961074968184821634ull; // the members' hash after the round trip

void mix(std::uint64_t& hash, const std::string& text)
{
	for (const char c : text)
	{
		hash ^= static_cast<std::uint8_t>(c);
		hash *= 1099511628211ull;
	}
}

void mix(std::uint64_t& hash, double value)
{
	char buffer[40];
	std::snprintf(buffer, sizeof(buffer), "%.17g;", value);
	mix(hash, std::string(buffer));
}

std::uint64_t hashOf(Simulation& sim, const std::vector<flecs::entity>& members)
{
	std::uint64_t hash = 14695981039346656037ull;
	for (const flecs::entity member : members)
	{
		const state::Fighter& f = fighterOf(sim, member);
		mix(hash, f.name + "|" + f.raceSid + "|" + f.weapon.sid + "|" + std::to_string(f.team) + "|" + std::to_string(f.faction) + "|" + std::to_string(f.slaveState) + f.captorSid + "|");
		for (const state::FighterArmour& armour : f.armour)
			mix(hash, armour.sid + ",");
		for (const state::FighterPart& part : f.parts)
			mix(hash, part.health);
		for (const double v : {f.x, f.z, f.y, f.blood, f.skills.strength, f.skills.dexterity, f.skills.toughness, f.skills.katana, f.skills.hackers, f.skills.blunt, f.skills.unarmed})
			mix(hash, v);
	}
	return hash;
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc > 1, "argv[1]: the Hub navmesh cache");
		const std::shared_ptr<MeshNavigator> nav = MeshNavigator::load(argv[1]);
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);
		std::shared_ptr<const CombatData> combat;
		{
			SpawnOptions options;
			options.settleSeconds = 0.0; // pins the t0 state: no new-game settle (TOWN_LIFE_SLICE 5.40)
			options.seed = 23;
			options.wildernessByDensity = false;
			Simulation probe = Simulation::newGame(KENSHI_DATA, order, options, nullptr, RegionConfig{});
			combat = CombatData::build(*probe.gameData(), KENSHI_DATA);
		}
		Simulation sim = newGame(order, combat, nav, 23);
		const Scene scene = pick(sim, 3);
		std::vector<flecs::entity> members = scene.members;
		members.insert(members.end(), scene.control.begin(), scene.control.end());
		leave(sim, scene);
		sim.run(300);
		ret(sim, scene);
		const std::uint64_t hash = hashOf(sim, members);
		std::cout << "members' hash after an untouched squad's round trip: " << hash << "\n";
		test::require(hash == kPinned, "the members' hash is the base build's");
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_delta_untouched: FAILED: " << e.what() << "\n";
		return 1;
	}
}
