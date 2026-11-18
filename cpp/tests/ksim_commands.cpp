// The command queue is the only external write path: enqueueing changes
// nothing until the command's tick, invalid commands are dropped, and a
// faction relation command writes the live state::Relation pair between
// FactionState instances, never the template's starting value.

#include "ksim/simulation.hpp"

#include "kworld/loadorder.hpp"

#include "test_util.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

struct Faction
{
	std::uint64_t id = 0;
	flecs::entity entity;
};

// FactionState instances in Id order.
std::vector<Faction> factionInstances(const ksim::Simulation& simulation)
{
	std::vector<Faction> factions;
	simulation.world().ecs().each([&factions](flecs::entity entity,
		const ksim::state::Id& id, const ksim::state::FactionState&)
	{
		factions.push_back(Faction{id.value, entity});
	});
	std::sort(factions.begin(), factions.end(),
		[](const Faction& left, const Faction& right) { return left.id < right.id; });
	return factions;
}

const ksim::state::Relation* relationOf(const Faction& from, const Faction& to)
{
	return from.entity.get<ksim::state::Relation>(to.entity);
}

// The template's stored starting value for an instance pair; nullopt when
// unlisted.
std::optional<std::int32_t> templateRelation(const ksim::Simulation& simulation,
	const Faction& from, const Faction& to)
{
	const ksim::World& world = simulation.world();
	return world.storedRelation(
		world.find(from.entity.get<ksim::state::FactionState>()->templateSid).id(),
		world.find(to.entity.get<ksim::state::FactionState>()->templateSid).id());
}

} // namespace

int main()
{
	try
	{
		ksim::Simulation simulation = ksim::Simulation::newGame(KENSHI_DATA,
			kworld::vanillaLoadOrder(KENSHI_DATA));

		// The public world handle is const: a command queue is structurally
		// the only way in.
		static_assert(std::is_const_v<
			std::remove_reference_t<decltype(simulation.world())>>);

		const std::vector<Faction> factions = factionInstances(simulation);
		test::require(factions.size() > 2, "too few faction instances for the test");

		// One spawned pair and one unlisted pair.
		std::pair<Faction, Faction> listed;
		std::pair<Faction, Faction> fresh;
		for (const Faction& left : factions)
		{
			for (const Faction& right : factions)
			{
				if (left.id == right.id)
					continue;
				if (relationOf(left, right) != nullptr)
				{
					if (listed.first.id == 0)
						listed = {left, right};
				} else if (fresh.first.id == 0) {
					fresh = {left, right};
				}
			}
		}
		test::require(listed.first.id != 0, "no spawned faction relation");
		test::require(fresh.first.id != 0, "no unlisted faction pair");
		const auto& [from, to] = listed;
		const std::int32_t oldValue = relationOf(from, to)->value;
		const std::int32_t newValue = oldValue == -100 ? 100 : -100;
		const std::optional<std::int32_t> templateBefore =
			templateRelation(simulation, from, to);

		const std::uint64_t initialHash = simulation.stateHash();
		const std::uint64_t startTick = simulation.clockTick();
		simulation.enqueue(ksim::Command::setFactionRelation(
			ksim::state::Id{from.id}, ksim::state::Id{to.id}, newValue));
		test::require(simulation.pendingCommands() == 1,
			"enqueue did not queue the command");
		test::require(relationOf(from, to)->value == oldValue,
			"enqueue mutated state before the tick");
		test::require(simulation.stateHash() == initialHash,
			"enqueue changed the state hash");

		simulation.tick();
		test::require(simulation.clockTick() == startTick + 1, "tick did not advance the Clock");
		test::require(simulation.pendingCommands() == 0,
			"queue was not cleared by the tick");
		test::require(relationOf(from, to)->value == newValue,
			"faction relation command was not applied");
		test::require(templateRelation(simulation, from, to) == templateBefore,
			"the command wrote the template's starting relation");
		test::require(simulation.stateHash() != initialHash,
			"a relation change did not change the state hash");
		test::require(simulation.systems()[0].runs == 1,
			"Full-detail system did not run");

		// A command stamped for a later tick waits for it.
		ksim::Command later = ksim::Command::setFactionRelation(
			ksim::state::Id{fresh.first.id}, ksim::state::Id{fresh.second.id}, 42);
		later.tick = simulation.clockTick() + 3;
		simulation.enqueue(later);
		simulation.run(2);
		test::require(simulation.pendingCommands() == 1
			&& relationOf(fresh.first, fresh.second) == nullptr,
			"a future command applied early");
		const std::uint64_t beforeCreate = simulation.stateHash();
		simulation.tick();
		const ksim::state::Relation* created = relationOf(fresh.first, fresh.second);
		test::require(created != nullptr && created->value == 42,
			"the unlisted faction relation was not created at its tick");
		test::require(simulation.stateHash() != beforeCreate,
			"creating a faction relation did not change the state hash");

		// A command stamped in the past applies at the next tick.
		ksim::Command past = ksim::Command::setFactionRelation(
			ksim::state::Id{fresh.first.id}, ksim::state::Id{fresh.second.id}, 7);
		past.tick = 1;
		simulation.enqueue(past);
		simulation.tick();
		test::require(relationOf(fresh.first, fresh.second)->value == 7,
			"a past-stamped command did not apply at the next tick");

		// Out-of-range values, an unknown Id and a non-faction Id are all
		// dropped.
		std::uint64_t squadId = 0;
		simulation.world().ecs().each([&squadId](const ksim::state::Id& id,
			const ksim::state::SquadInstance&)
		{
			if (squadId == 0)
				squadId = id.value;
		});
		test::require(squadId != 0, "no squad instance");
		simulation.enqueue(ksim::Command::setFactionRelation(
			ksim::state::Id{from.id}, ksim::state::Id{to.id}, 101));
		simulation.enqueue(ksim::Command::setFactionRelation(
			ksim::state::Id{from.id}, ksim::state::Id{0}, 10));
		simulation.enqueue(ksim::Command::setFactionRelation(
			ksim::state::Id{squadId}, ksim::state::Id{to.id}, 10));
		simulation.enqueue(ksim::Command::setSquadCentre(-1, 0.0, 0.0));
		simulation.tick();
		test::require(relationOf(from, to)->value == newValue,
			"an invalid faction relation command was applied");
		test::require(simulation.pendingCommands() == 0,
			"invalid commands stayed queued");

		std::cout << "ksim_commands: ok, hash " << std::hex
			<< simulation.stateHash() << std::dec << "\n";
	}
	catch (const std::exception& error)
	{
		std::cerr << "ksim_commands: " << error.what() << "\n";
		return 1;
	}
	return 0;
}
