// Assault -> Terrorism (task ds/terrorism; kinspect dt43, setCrime 140852c80 / predicate 140796240(victim, 1)): an Assault on a victim whose
// unit data says `NPC class` 5 (diplomat) standing in a BIOME_GROUP of its faction's FACTION `biomes` is crime 9, and the rewrite skips
// the pending-different-crime refusal. Out of territory, or a victim of another class, it stays 5. Murder / Treason / Smuggling (4 / 6 / 8)
// have no writer in the exe and are never written here. The pure rule is crime::setCrime's flag; the world half is Simulation::reportCrime.

#include "dt27_rig.hpp"

#include "ksim/law/crime.hpp"
#include "ksim/nav/placement.hpp"
#include "ksim/core/rng.hpp"

#include <algorithm>
#include <set>

namespace {

using namespace ksim;
using namespace dt27;
namespace cr = ksim::crime;

void rule()
{
	state::CrimeRecord r;
	test::require(cr::setCrime(r, cr::kAssault, "A", 7, true) && r.crime == cr::kTerrorism, "a flagged assault is terrorism");
	state::CrimeRecord plain;
	test::require(cr::setCrime(plain, cr::kAssault, "A", 7, false) && plain.crime == cr::kAssault, "an unflagged assault stays assault");
	state::CrimeRecord pending;
	cr::setCrime(pending, cr::kTheft, "A", 0);
	test::require(cr::setCrime(pending, cr::kAssault, "A", 7, true) && pending.crime == cr::kTerrorism && pending.victimChar == 7,
		"a pending theft does not block the terrorism");
	state::CrimeRecord committed;
	cr::setCrime(committed, cr::kTheft, "A", 0);
	cr::commit(committed, "A", 9, 5.0);
	test::require(cr::setCrime(committed, cr::kAssault, "A", 7, true) && committed.crime == cr::kTerrorism && !committed.committed,
		"a committed theft does not block it either, and the new crime is uncommitted");
	state::CrimeRecord other;
	cr::setCrime(other, cr::kTheft, "A", 0);
	test::require(!cr::setCrime(other, cr::kBurglary, "A", 0, true) && other.crime == cr::kTheft, "the flag only rewrites an assault");
}

void world()
{
	Rig rig({}, 0);
	const kdata::GameData& gd = *rig.gd;
	const WildGround ground(KENSHI_DATA);
	// A faction whose `biomes` has an area on the map, and an area it does not list.
	std::string faction;
	std::uint32_t inside = 0;
	std::set<std::uint32_t> listed;
	for (const kdata::Record* record : gd.byType(kworld::itemTypeByName("FACTION")))
	{
		const kdata::RefCategory* biomes = record->findRefs("biomes");
		if (biomes == nullptr || record->boolOr("not real", false) || record->intOr("fundamental type", 0) == 8)
			continue;
		std::set<std::uint32_t> colours;
		std::uint32_t found = 0;
		for (const kdata::RefEntry& entry : biomes->entries)
			if (const kdata::Record* group = gd.find(entry.targetID))
			{
				const std::uint32_t colour = static_cast<std::uint32_t>(group->intOr("index", 0));
				colours.insert(colour);
				if (found == 0 && ground.zones(colour) > 0.0)
					found = colour;
			}
		if (found != 0)
		{
			faction = record->stringID;
			inside = found;
			listed = colours;
			break;
		}
	}
	test::require(!faction.empty(), "no FACTION with a mapped `biomes` area");
	// Outside: a point of the areas map near the Hub whose colour the faction does not list (the map has colour 0 where no group lies).
	std::uint32_t outside = 0;
	state::GroundPoint away;
	for (double dz = -60000.0; dz <= 60000.0 && outside == 0; dz += 2500.0)
		for (double dx = -60000.0; dx <= 60000.0 && outside == 0; dx += 2500.0)
		{
			const std::uint32_t colour = ground.colourAt(kX + dx, kZ + dz);
			if (colour != 0 && listed.count(colour) == 0)
			{
				outside = colour;
				away = {kX + dx, kZ + dz};
			}
		}
	test::require(outside != 0, "no mapped area outside the faction's territory");
	RngStream dice(5);
	const state::GroundPoint home = ground.pick(inside, dice);

	const std::uint64_t victim = rig.spawn(rig.body, home.x, home.z);
	const std::uint64_t attacker = rig.spawn(rig.body, home.x + 2.0, home.z);
	state::Fighter& v = rig.fighterOf(victim);
	const auto recordOf = [&](std::uint64_t id) { return rig.sim.crimeRecord(id); };
	const auto assault = [&](std::uint8_t cls, const state::GroundPoint& at) -> std::uint8_t
	{
		rig.entityOf(attacker).remove<state::CrimeRecord>();
		state::Fighter& victimState = rig.fighterOf(victim);
		victimState.npcClass = cls;
		victimState.x = at.x;
		victimState.z = at.z;
		rig.sim.reportCrime(attacker, cr::kAssault, faction, victim, cr::kTimerAssault);
		const state::CrimeRecord* record = recordOf(attacker);
		return record != nullptr ? record->crime : 0;
	};
	(void)v;
	test::require(assault(cr::kClassDiplomat, home) == cr::kTerrorism, "a diplomat in territory: assault should be terrorism");
	test::require(assault(cr::kClassDiplomat, away) == cr::kAssault, "a diplomat out of territory: assault stays assault");
	test::require(assault(cr::kClassTrader, home) == cr::kAssault, "a non-diplomat in territory: assault stays assault");
	// A pending other crime on the attacker does not block the 9.
	rig.entityOf(attacker).remove<state::CrimeRecord>();
	rig.fighterOf(victim).npcClass = cr::kClassDiplomat;
	rig.fighterOf(victim).x = home.x;
	rig.fighterOf(victim).z = home.z;
	rig.sim.reportCrime(attacker, cr::kTheft, faction, 0, 3.0);
	test::require(recordOf(attacker) != nullptr && recordOf(attacker)->crime == cr::kTheft, "the theft is not pending");
	rig.sim.reportCrime(attacker, cr::kAssault, faction, victim, cr::kTimerAssault);
	test::require(recordOf(attacker)->crime == cr::kTerrorism, "a pending theft blocked the terrorism");
	std::cout << "  terrorism: faction " << faction << ", territory colour " << inside << ", outside colour " << outside << "\n";
}

} // namespace

int main()
{
	try
	{
		rule();
		world();
		std::cout << "ksim_terrorism: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_terrorism: FAILED: " << e.what() << "\n";
		return 1;
	}
}
