// The spawner against the world-model oracle (docs/reference/world_model/
// data/*.tsv, written by tools/world_model.py): factions, relations, placed
// towns and their active variant/owner, squads per source and per faction,
// and squad positions at their home town.

#include "ksim/world/spawn.hpp"

#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"
#include "kworld/towns.hpp"

#include "test_util.hpp"

#include <cmath>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

using Row = std::map<std::string, std::string>;

std::vector<Row> readTsv(const std::string& name)
{
	const std::string path = std::string(KSIM_SOURCE_DIR)
		+ "/../docs/reference/world_model/data/" + name;
	std::ifstream in(path);
	test::require(static_cast<bool>(in), "cannot open " + path);
	const auto split = [](const std::string& line)
	{
		std::vector<std::string> fields;
		std::stringstream stream(line);
		std::string field;
		while (std::getline(stream, field, '\t'))
			fields.push_back(field);
		if (!line.empty() && line.back() == '\t')
			fields.emplace_back();
		return fields;
	};
	std::string line;
	std::getline(in, line);
	const std::vector<std::string> header = split(line);
	std::vector<Row> rows;
	while (std::getline(in, line))
	{
		const std::vector<std::string> fields = split(line);
		Row row;
		for (std::size_t i = 0; i < header.size(); ++i)
			row[header[i]] = i < fields.size() ? fields[i] : std::string();
		rows.push_back(std::move(row));
	}
	return rows;
}

template <typename Key>
void requireSameCounts(const std::map<Key, std::size_t>& expected,
	const std::map<Key, std::size_t>& actual, const std::string& what)
{
	for (const auto& [key, count] : expected)
	{
		const auto found = actual.find(key);
		const std::size_t got = found == actual.end() ? 0 : found->second;
		test::require(got == count, what + " '" + key + "': "
			+ std::to_string(got) + " != " + std::to_string(count));
	}
	for (const auto& [key, count] : actual)
	{
		test::require(expected.count(key) != 0, what + " '" + key
			+ "' unexpected (" + std::to_string(count) + ")");
	}
}

} // namespace

int main()
{
	try
	{
		if (!std::filesystem::exists(std::filesystem::path(KSIM_SOURCE_DIR) / "../docs/reference/world_model/data/factions.tsv"))
		{
			std::cout << "ksim_spawn: SKIPPED: the world-model oracle tables are not in this tree (game-derived, not shipped)\n";
			return 77;
		}
		const kworld::LoadOrder order = kworld::resolveLoadOrder(KENSHI_DATA);
		const kdata::GameData gameData = kworld::loadMergedGameData(order);
		const kworld::Towns towns = kworld::Towns::join(gameData, KENSHI_DATA);

		flecs::world ecs;
		const ksim::SpawnReport report = ksim::spawnDefaultWorld(ecs, gameData, towns);

		// Factions and starting relations.
		std::map<std::string, std::string> factionName;
		for (const Row& row : readTsv("factions.tsv"))
			factionName[row.at("sid")] = row.at("name");
		test::require(report.factions == factionName.size(),
			"faction count " + std::to_string(report.factions) + " != "
			+ std::to_string(factionName.size()));
		test::require(report.relations == readTsv("faction_relations.tsv").size(),
			"relation count differs from faction_relations.tsv");
		std::size_t relationPairs = 0;
		ecs.each<ksim::state::FactionState>(
			[&](flecs::entity faction, const ksim::state::FactionState&)
			{
				faction.each<ksim::state::Relation>([&](flecs::entity)
				{
					++relationPairs;
				});
			});
		test::require(relationPairs == report.relations,
			"Relation pairs differ from the report");

		// Placed towns: active variant, owner and position.
		std::map<std::string, Row> placedTowns;
		for (const Row& row : readTsv("towns.tsv"))
		{
			if (!row.at("x").empty())
				placedTowns[row.at("root_sid")] = row;
		}
		test::require(report.towns == placedTowns.size(),
			"town count " + std::to_string(report.towns) + " != "
			+ std::to_string(placedTowns.size()));
		std::map<flecs::entity_t, const Row*> townRow;
		ecs.each<ksim::state::TownState>(
			[&](flecs::entity entity, const ksim::state::TownState& town)
			{
				const auto found = placedTowns.find(town.rootSid);
				test::require(found != placedTowns.end(),
					"unexpected town root " + town.rootSid);
				test::require(town.activeSid == found->second.at("active_sid"),
					"active variant differs for " + town.rootSid);
				std::string owner;
				if (town.owner.id != 0)
					owner = flecs::entity(ecs, town.owner.id)
						.get<ksim::state::FactionState>()->templateSid;
				test::require(owner == found->second.at("owner_sid"),
					"owner differs for " + town.rootSid);
				townRow[entity.id()] = &found->second;
			});

		// Squads per source: town lists from town_squads.tsv (placed roots),
		// wilderness from the first active, eligible non-town source per
		// template in spawn_sources.tsv.
		const std::map<std::string, std::string> townCategory = {
			{"residents", "town resident"},
			{"bar squads", "town bar squad"},
			{"default resident", "town default resident"},
			{"roaming squads", "town roaming squad"},
		};
		std::map<std::string, std::size_t> expectedBySource;
		std::map<std::string, std::size_t> expectedByFaction;
		for (const Row& row : readTsv("town_squads.tsv"))
		{
			const auto category = townCategory.find(row.at("category"));
			if (category == townCategory.end()
				|| placedTowns.count(row.at("root_sid")) == 0)
				continue;
			++expectedBySource[category->second];
			const std::string& squadFaction = row.at("squad_faction");
			const std::string& townFaction = row.at("town_faction");
			++expectedByFaction[!squadFaction.empty() ? squadFaction
				: townFaction.empty() ? std::string("(none)")
				: factionName.at(townFaction)];
		}
		std::set<std::string> wilderness;
		for (const Row& row : readTsv("spawn_sources.tsv"))
		{
			const std::string& type = row.at("source_type");
			if (type == "town roaming squad" || row.at("active_at_start") != "yes"
				|| row.at("squad_world_state_eligible") != "True"
				|| !wilderness.insert(row.at("squad_sid")).second)
				continue;
			++expectedBySource[type];
			const std::string& faction = row.at("faction");
			++expectedByFaction[faction.empty() ? std::string("(none)") : faction];
		}

		std::map<std::string, std::size_t> bySource;
		std::map<std::string, std::size_t> byFaction;
		std::size_t members = 0;
		ecs.each<ksim::state::SquadInstance>(
			[&](flecs::entity squad, const ksim::state::SquadInstance& instance)
			{
				++bySource[std::string(ksim::state::spawnSourceName(instance.source))];
				std::string faction = "(none)";
				if (instance.faction.id != 0)
					faction = factionName.at(flecs::entity(ecs, instance.faction.id)
						.get<ksim::state::FactionState>()->templateSid);
				++byFaction[faction];

				const auto* position = squad.get<ksim::state::Position>();
				if (instance.homeTown.id == 0)
				{
					test::require(position == nullptr,
						"wilderness squad has a position");
				} else {
					const Row& town = *townRow.at(instance.homeTown.id);
					test::require(position != nullptr
						&& std::fabs(position->x - std::stod(town.at("x"))) < 1e-3
						&& std::fabs(position->z - std::stod(town.at("z"))) < 1e-3,
						"squad is not at its home town");
				}
			});
		ecs.each([&](flecs::entity character, const ksim::state::CharacterInstance&)
		{
			const flecs::entity squad = character.target<ksim::state::MemberOf>();
			test::require(squad.is_valid()
				&& squad.get<ksim::state::SquadInstance>() != nullptr,
				"character is not a member of a squad");
			++members;
		});
		requireSameCounts(expectedBySource, bySource, "squads from");
		requireSameCounts(expectedByFaction, byFaction, "squads of faction");
		test::require(members == report.characters, "character count differs");

		// A `unique` character ("will only ever spawn once") exists at most once, however many town lists name it.
		std::map<std::string, std::size_t> uniques;
		ecs.each([&](const ksim::state::CharacterInstance& character)
		{
			const kdata::Record* record = gameData.find(character.templateSid);
			if (record != nullptr && record->boolOr("unique", false))
				++uniques[character.templateSid];
		});
		for (const auto& [sid, count] : uniques)
			test::require(count == 1, "unique character " + sid + " spawned " + std::to_string(count) + " times");
		std::cout << "  " << uniques.size() << " unique characters, each once\n";

		// Same seed, same members; another seed draws differently.
		flecs::world again;
		test::require(ksim::spawnDefaultWorld(again, gameData, towns).characters
			== report.characters, "same seed drew a different character count");
		flecs::world other;
		ksim::SpawnOptions otherSeed;
		otherSeed.seed = 2;
		const std::size_t otherCharacters =
			ksim::spawnDefaultWorld(other, gameData, towns, otherSeed).characters;

		std::cout << "ksim_spawn: factions " << report.factions << ", relations "
			<< report.relations << ", towns " << report.towns << ", squads "
			<< report.squads << ", characters " << report.characters
			<< " (seed 2: " << otherCharacters << "), town squads failing their"
			<< " world state " << report.townSquadsFailingWorldState << "\n";
		for (const auto& [source, count] : bySource)
			std::cout << "  " << source << " " << count << "\n";
	}
	catch (const std::exception& error)
	{
		std::cerr << "ksim_spawn: " << error.what() << "\n";
		return 1;
	}
	return 0;
}
