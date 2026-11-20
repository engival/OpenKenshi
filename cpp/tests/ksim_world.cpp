// World model checks: prefab counts and relationships against the reference
// kmod.py overlay, stringID lookup, and placed building transforms against
// kworld::Zones::place.

#include "ksim/world/world.hpp"

#include "kworld/gamedata.hpp"
#include "kworld/heightmap.hpp"
#include "kworld/zones.hpp"
#include "kworld_test_util.hpp"
#include "ksim/dev/build_paths.hpp"

#include <array>
#include <cstddef>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {

struct Expected
{
	std::map<std::string, std::size_t> typeCounts;
	std::map<std::string, std::size_t> relationCounts;
	std::vector<std::pair<std::string, std::string>> townOwners;
	struct FactionRelation
	{
		std::string from;
		std::string to;
		std::int32_t value = 0;
	};
	std::vector<FactionRelation> factionRelations;
	// (source, relation category) -> (target -> (v0, v1, v2)).
	std::map<std::pair<std::string, std::string>,
		std::map<std::string, std::array<std::int32_t, 3>>> squads;
};

std::vector<std::string> splitTabs(const std::string& line)
{
	std::vector<std::string> fields;
	std::istringstream stream(line);
	std::string field;
	while (std::getline(stream, field, '\t'))
		fields.push_back(field);
	return fields;
}

std::string ksimPythonScript(const char* name)
{
	return std::string(KSIM_SOURCE_DIR) + "/tests/ksim_py/" + name;
}

Expected referenceData()
{
	const std::string command = "python3 -I "
		+ test::shellQuote(ksimPythonScript("ksim_expected.py")) + " "
		+ test::shellQuote(test::refTool("kmod.py")) + " "
		+ test::shellQuote(KENSHI_DATA) + " 2>&1";
	const std::string output = test::runCommand(command);

	Expected expected;
	std::istringstream lines(output);
	std::string line;
	while (std::getline(lines, line))
	{
		if (line.empty())
			continue;
		const auto fields = splitTabs(line);
		if (fields.empty())
			continue;
		if (fields[0] == "count" && fields.size() == 3)
		{
			expected.typeCounts[fields[1]] = static_cast<std::size_t>(
				std::stoul(fields[2]));
		}
		else if (fields[0] == "rel" && fields.size() == 3)
		{
			expected.relationCounts[fields[1]] = static_cast<std::size_t>(
				std::stoul(fields[2]));
		}
		else if (fields[0] == "townowner" && fields.size() == 3)
		{
			expected.townOwners.emplace_back(fields[1], fields[2]);
		}
		else if (fields[0] == "factionrel" && fields.size() == 4)
		{
			expected.factionRelations.push_back(
				{fields[1], fields[2], std::stoi(fields[3])});
		}
		else if (fields[0] == "squad" && fields.size() == 7)
		{
			expected.squads[{fields[1], fields[2]}][fields[3]] = {
				std::stoi(fields[4]), std::stoi(fields[5]),
				std::stoi(fields[6])};
		}
		else
		{
			throw test::Failure("unexpected ksim_expected.py line: " + line);
		}
	}
	return expected;
}

std::size_t typeField(const ksim::WorldCounts& counts, const std::string& name)
{
	if (name == "FACTION") return counts.factions;
	if (name == "TOWN") return counts.towns;
	if (name == "SQUAD_TEMPLATE") return counts.squadTemplates;
	if (name == "CHARACTER") return counts.characters;
	if (name == "HUMAN_CHARACTER") return counts.humanCharacters;
	if (name == "ANIMAL_CHARACTER") return counts.animals;
	if (name == "BUILDING") return counts.buildings;
	if (name == "RACE") return counts.races;
	if (name == "AI_PACKAGE") return counts.aiPackages;
	throw test::Failure("unknown type in reference output: " + name);
}

std::size_t relationField(const ksim::WorldCounts& counts,
	const std::string& name)
{
	if (name == "faction_relations") return counts.factionRelations;
	if (name == "membership") return counts.membership;
	if (name == "town_ownership") return counts.townOwnership;
	if (name == "town_residents") return counts.townResidents;
	if (name == "town_bar_squads") return counts.townBarSquads;
	if (name == "town_roaming_squads") return counts.townRoamingSquads;
	if (name == "town_default_resident") return counts.townDefaultResident;
	if (name == "faction_residents") return counts.factionResidents;
	if (name == "faction_default_resident") return counts.factionDefaultResident;
	if (name == "faction_bar_squads") return counts.factionBarSquads;
	if (name == "faction_squads") return counts.factionSquads;
	if (name == "faction_squad_default") return counts.factionSquadDefault;
	if (name == "faction_roaming_squads") return counts.factionRoamingSquads;
	if (name == "squad_members") return counts.squadMembers;
	if (name == "squad_members2") return counts.squadMembers2;
	if (name == "squad_leaders") return counts.squadLeaders;
	if (name == "squad_choosefrom") return counts.squadChooseFrom;
	if (name == "squad_animals") return counts.squadAnimals;
	if (name == "squad_animals2") return counts.squadAnimals2;
	if (name == "race_options") return counts.raceOptions;
	if (name == "ai_package_links") return counts.aiPackageLinks;
	if (name == "ai_inherits") return counts.aiInherits;
	if (name == "ai_leads_to") return counts.aiLeadsTo;
	if (name == "overrides_town") return counts.overridesTown;
	throw test::Failure("unknown relation in reference output: " + name);
}

template <typename Relation>
void checkSquadRelation(const ksim::World& world, const std::string& sourceID,
	const std::map<std::string, std::array<std::int32_t, 3>>& expected)
{
	const flecs::entity source = world.find(sourceID);
	test::require(source.is_valid(), "missing squad template " + sourceID);

	std::map<std::string, std::array<std::int32_t, 3>> actual;
	source.each<Relation>([&](flecs::entity target)
	{
		const ksim::tmpl::Source* identity = target.get<ksim::tmpl::Source>();
		test::require(identity != nullptr, "squad target has no Source");
		const Relation* value = source.get<Relation>(target);
		test::require(value != nullptr, "list pair has no value");
		actual[identity->stringID] = {value->v0, value->v1, value->v2};
	});
	test::require(actual.size() == expected.size(),
		"pair count mismatch on squad template " + sourceID);
	for (const auto& [targetID, values] : expected)
	{
		const auto found = actual.find(targetID);
		test::require(found != actual.end(),
			"missing squad pair " + sourceID + " -> " + targetID);
		test::require(found->second == values,
			"pair values differ on " + sourceID + " -> " + targetID);
	}
}

// Full tuple so duplicate instanceIDs (present in the zone data) still
// compare one-for-one.
using PlacementKey = std::tuple<std::string, std::string, std::string,
	double, double, double, double, double, double, double, float, int, int>;

PlacementKey placedKey(const kworld::PlacedInstance& instance)
{
	return {instance.layer, instance.instanceID, instance.buildingID,
		instance.x, instance.y, instance.z, instance.qw, instance.qx,
		instance.qy, instance.qz, instance.scale, instance.cell.x,
		instance.cell.z};
}

void checkPlacements(const ksim::World& world)
{
	const auto zones = kworld::Zones::load(KENSHI_DATA);
	const auto gameData = kworld::loadMergedGameData(KENSHI_DATA);
	const auto heightmap = kworld::Heightmap::open(
		test::dataPath("newland/land/fullmap.tif"));
	const auto placed = zones.placeMerged(heightmap, gameData);

	std::map<PlacementKey, std::size_t> expected;
	for (const auto& instance : placed)
		++expected[placedKey(instance)];

	std::size_t entityCount = 0;
	std::size_t withPrefab = 0;
	std::size_t missing = 0;
	world.ecs().each<ksim::tmpl::PlacedBuilding>(
		[&](flecs::entity entity, const ksim::tmpl::PlacedBuilding& building)
		{
			++entityCount;
			const auto* position = entity.get<ksim::tmpl::Position>();
			const auto* rotation = entity.get<ksim::tmpl::Rotation>();
			const auto* scale = entity.get<ksim::tmpl::Scale>();
			const auto* cell = entity.get<ksim::tmpl::PlacementCell>();
			test::require(position != nullptr && rotation != nullptr
				&& scale != nullptr && cell != nullptr,
				"placed entity lacks a transform component");

			PlacementKey key{building.layer, building.instanceID,
				building.buildingID, position->x, position->y, position->z,
				rotation->w, rotation->x, rotation->y, rotation->z,
				scale->value, cell->x, cell->z};
			const auto found = expected.find(key);
			test::require(found != expected.end() && found->second != 0,
				"placed entity does not match kworld::Zones::placeMerged");
			--found->second;

			const flecs::entity prefab = world.find(building.buildingID);
			if (prefab.is_valid()
				&& prefab.get<ksim::tmpl::Building>() != nullptr)
			{
				test::require(entity.has(flecs::IsA, prefab),
					"placed building does not inherit its BUILDING prefab");
				++withPrefab;
			}
			else
			{
				const auto* absent =
					entity.get<ksim::tmpl::MissingBuildingPrefab>();
				test::require(absent != nullptr
					&& absent->buildingID == building.buildingID,
					"placement without prefab lacks MissingBuildingPrefab");
				++missing;
			}
		});

	test::require(entityCount == placed.size(),
		"placed entity count differs from kworld::Zones::placeMerged");
	for (const auto& [key, remaining] : expected)
		test::require(remaining == 0, "kworld placement has no ksim entity");

	const ksim::WorldCounts& counts = world.counts();
	test::require(counts.placedBuildings == entityCount,
		"placed count differs from entity count");
	test::require(counts.placedWithPrefab == withPrefab,
		"placed-with-prefab count differs");
	test::require(entityCount - withPrefab == missing,
		"missing-prefab count differs");
}

} // namespace

int main()
{
	try
	{
		const ksim::World world = ksim::World::load(KENSHI_DATA);
		const ksim::WorldCounts& counts = world.counts();
		const Expected expected = referenceData();

		for (const auto& [name, value] : expected.typeCounts)
		{
			const std::size_t actual = typeField(counts, name);
			test::require(actual == value,
				"type count differs for " + name + ": " + std::to_string(actual)
				+ " != " + std::to_string(value));
		}
		for (const auto& [name, value] : expected.relationCounts)
		{
			const std::size_t actual = relationField(counts, name);
			test::require(actual == value,
				"relation count differs for " + name + ": "
				+ std::to_string(actual) + " != " + std::to_string(value));
		}
		test::require(counts.prefabs == world.prefabs().size(),
			"prefab total differs from lookup map size");

		// stringID lookup round trip, and Source on every prefab.
		for (const auto& [stringID, entity] : world.prefabs())
		{
			const flecs::entity found = world.find(stringID);
			test::require(found.is_valid() && found.id() == entity.id(),
				"stringID lookup round trip failed for " + stringID);
			const ksim::tmpl::Source* source = entity.get<ksim::tmpl::Source>();
			test::require(source != nullptr,
				"prefab has no Source: " + stringID);
			test::require(source->stringID == stringID,
				"Source.stringID differs from lookup key: " + stringID);
			test::require(!source->type.empty(),
				"Source.type is empty: " + stringID);
		}
		test::require(!world.find("no-such-record-sID").is_valid(),
			"lookup of an absent stringID returned an entity");

		for (const auto& [townID, factionID] : expected.townOwners)
		{
			const flecs::entity town = world.find(townID);
			const flecs::entity faction = world.find(factionID);
			test::require(town.is_valid(), "missing town " + townID);
			test::require(faction.is_valid(), "missing faction " + factionID);
			test::require(town.has<ksim::tmpl::OwnedBy>(faction),
				"town owner pair missing: " + townID + " -> " + factionID);
		}

		for (const auto& relation : expected.factionRelations)
		{
			const flecs::entity from = world.find(relation.from);
			const flecs::entity to = world.find(relation.to);
			test::require(from.is_valid(), "missing faction " + relation.from);
			test::require(to.is_valid(), "missing faction " + relation.to);
			const auto stored = world.storedRelation(from.id(), to.id());
			test::require(stored.has_value() && *stored == relation.value,
				"starting relation differs: " + relation.from + " -> "
				+ relation.to);
		}

		for (const auto& [key, entries] : expected.squads)
		{
			const std::string& sourceID = key.first;
			const std::string& category = key.second;
			if (category == "squad")
				checkSquadRelation<ksim::tmpl::SquadMember>(world, sourceID, entries);
			else if (category == "squad2")
				checkSquadRelation<ksim::tmpl::SquadMember2>(world, sourceID, entries);
			else if (category == "leader")
				checkSquadRelation<ksim::tmpl::SquadLeader>(world, sourceID, entries);
			else if (category == "choosefrom list")
				checkSquadRelation<ksim::tmpl::SquadChooseFrom>(world, sourceID,
					entries);
			else if (category == "animals")
				checkSquadRelation<ksim::tmpl::SquadAnimal>(world, sourceID, entries);
			else if (category == "animals2")
				checkSquadRelation<ksim::tmpl::SquadAnimal2>(world, sourceID, entries);
			else
				throw test::Failure("unexpected squad category " + category);
		}

		checkPlacements(world);

		std::cout << "ksim_world: prefabs " << counts.prefabs
			<< ", placements " << counts.placedBuildings
			<< ", missing prefabs "
			<< counts.placedBuildings - counts.placedWithPrefab << "\n";
	}
	catch (const std::exception& error)
	{
		std::cerr << "ksim_world: " << error.what() << "\n";
		return 1;
	}
	return 0;
}
