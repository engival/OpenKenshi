#include "ksim/snapshot.hpp"

#include "ksim/clock.hpp"
#include "ksim/state.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <ostream>
#include <utility>
#include <vector>

namespace ksim {

namespace {

void writeString(std::ostream& out, std::string_view value)
{
	out << '"';
	for (const char c : value)
	{
		switch (c)
		{
		case '"':
			out << "\\\"";
			break;
		case '\\':
			out << "\\\\";
			break;
		case '\n':
			out << "\\n";
			break;
		case '\r':
			out << "\\r";
			break;
		case '\t':
			out << "\\t";
			break;
		default:
			if (static_cast<unsigned char>(c) < 0x20)
			{
				char escaped[8];
				std::snprintf(escaped, sizeof(escaped), "\\u%04x", c);
				out << escaped;
			} else {
				out << c;
			}
		}
	}
	out << '"';
}

void writeNumber(std::ostream& out, double value)
{
	char text[32];
	std::snprintf(text, sizeof(text), "%.3f", value);
	out << text;
}

} // namespace

void writeSquadSnapshotJson(std::ostream& out, const flecs::world& ecs,
	const std::function<std::string(std::string_view sid)>& nameOf)
{
	const auto idOf = [](flecs::entity entity)
	{
		return entity.get<state::Id>()->value;
	};

	std::map<flecs::entity_t, std::vector<std::pair<std::uint64_t, std::string>>> members;
	ecs.each([&](flecs::entity character, const state::CharacterInstance& instance)
	{
		const flecs::entity squad = character.target<state::MemberOf>();
		if (squad.is_valid())
			members[squad.id()].emplace_back(idOf(character), instance.templateSid);
	});

	std::vector<std::pair<std::uint64_t, flecs::entity>> squads;
	ecs.each([&](flecs::entity squad, const state::SquadInstance&)
	{
		squads.emplace_back(idOf(squad), squad);
	});
	std::sort(squads.begin(), squads.end(),
		[](const auto& left, const auto& right)
		{
			return left.first < right.first;
		});

	const std::uint64_t tick = ecs.get<state::Clock>()->tick;
	const GameTime time = gameTimeAt(tick);
	out << "{\"clock\": {\"tick\": " << tick << ", \"day\": " << time.day
		<< ", \"hour\": " << time.hour << ", \"minute\": " << time.minute << "},\n";
	out << "\"squads\": [";
	bool firstSquad = true;
	for (const auto& [id, squad] : squads)
	{
		const state::SquadInstance& instance = *squad.get<state::SquadInstance>();
		out << (firstSquad ? "\n" : ",\n");
		firstSquad = false;
		out << "{\"id\": " << id << ", \"sid\": ";
		writeString(out, instance.templateSid);
		out << ", \"name\": ";
		writeString(out, nameOf(instance.templateSid));
		out << ", \"source\": ";
		writeString(out, state::spawnSourceName(instance.source));
		out << ", \"faction\": ";
		if (instance.faction.id != 0)
			writeString(out, flecs::entity(ecs.c_ptr(), instance.faction.id)
				.get<state::FactionState>()->templateSid);
		else
			out << "null";
		out << ", \"town\": ";
		if (instance.homeTown.id != 0)
			writeString(out, flecs::entity(ecs.c_ptr(), instance.homeTown.id)
				.get<state::TownState>()->rootSid);
		else
			out << "null";
		if (const state::Position* position = squad.get<state::Position>())
		{
			out << ", \"x\": ";
			writeNumber(out, position->x);
			out << ", \"z\": ";
			writeNumber(out, position->z);
		} else {
			out << ", \"x\": null, \"z\": null";
		}
		out << ", \"members\": [";
		auto found = members.find(squad.id());
		if (found != members.end())
		{
			std::sort(found->second.begin(), found->second.end());
			bool firstMember = true;
			for (const auto& [memberId, sid] : found->second)
			{
				out << (firstMember ? "" : ", ") << "{\"id\": " << memberId << ", \"sid\": ";
				writeString(out, sid);
				out << "}";
				firstMember = false;
			}
		}
		out << "]}";
	}
	out << "\n]}\n";
}

} // namespace ksim
