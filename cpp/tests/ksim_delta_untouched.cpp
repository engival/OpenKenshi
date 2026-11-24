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

// Repinned for kenshi-k16.7.89 (the furniture body ring moves the mesh on purpose; 4397699947472014831 before).
// Repinned for kenshi-k16.6.253 (the follow slot's direct steer only on a line a body fits, and not left at 0 u along a wall: 6893984046434549243 before; followers' placement moves, the delta store is untouched by it).
// Repinned for kenshi-k16.6.182 / k16.6.200 (the walk's heading memory, saved from v109): the members' placement after the walk moves; the delta
// store is untouched by it, and the hash is the same on navworld v12 and v13 (was 2317286899095492733 for kenshi-k16.6.139's formation direct steer,
// 12384742747856631192 from the base efb91c7e).
// Repinned for kenshi-k16.6.320 (the 1 Hz squad passes on id % 20 slices, and a save taking the arena's Fighters: 3961074968184821634 before; the
// members' placement after the walk moves, the delta store is untouched by it; same hash on ds/stagger-slices alone and on ksim 8097e86).
// Repinned for kenshi-k16.8.26 (a follower's route re-plans on the slot's 2 u drift and is kept to its end when planned for this slot; merged on the k16.7.89 ring's code,
// replacing its pin 17417856568400189656; 7989452899866182258 before that merge); followers' placement only, the delta store is untouched by it.
// Repinned for kenshi-k16.8.24 (chases outside buildings slew the velocity: the members' placement after the walk moves, the delta store is untouched by it; 6976828997182875623 before).
// Repinned for kenshi-k16.8.24 (chases outside buildings slew the velocity: the members' placement after the walk moves, the delta store is untouched by it; 17183893509936566136 before).
// Repinned for kenshi-k16.6.614 (a pass's promotions are built a few a tick, so a re-promoted squad is built a few ticks after its pass and the placement after the
// walk moves; the delta store is untouched by it: 10960152686983995781 before).
// Repinned for kenshi-e9s9.18 on the k16.7.127 bake (recipe 45) (a hop keeps the surface layer within 8 u; a chase re-plans when off its route's surface: the members'
// placement after the walk moves, the delta store is untouched by it; 12574167448220524344 on ksim 9d63ba01 before, 18035350237351644041 on the earlier base).
constexpr std::uint64_t kPinned = 967332254289828886ull; // the members' hash after the round trip

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
