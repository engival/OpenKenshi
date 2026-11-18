// Runtime save v1 (docs/formats/RUNTIME_SAVE.md): a spawned default world
// saved and loaded into a fresh world hashes identically; the hash ignores
// flecs entity ids, sees every saved value, and differs across seeds; a
// load-order mismatch is refused unless forced; Simulation newGame -> run ->
// saveGame -> loadGame keeps the clock, the state hash, the region-engine
// mirror and the pending commands. ksim_continue checks the continuation.

#include "ksim/save/save.hpp"
#include "ksim/core/simulation.hpp"
#include "ksim/world/spawn.hpp"
#include "ksim/core/state.hpp"

#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"
#include "kworld/towns.hpp"

#include "test_util.hpp"

#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string saveBytes(const flecs::world& ecs, const std::vector<std::string>& order)
{
	std::ostringstream out;
	ksim::writeSave(out, ecs, order);
	return out.str();
}

void loadBytes(const std::string& bytes, flecs::world& ecs,
	const std::vector<std::string>& order, bool force = false)
{
	std::istringstream in(bytes);
	ksim::readSave(in, ecs, order, force);
}

} // namespace

int main()
{
	try
	{
		const kworld::LoadOrder order = kworld::resolveLoadOrder(KENSHI_DATA);
		const kdata::GameData gameData = kworld::loadMergedGameData(order);
		const kworld::Towns towns = kworld::Towns::join(gameData, KENSHI_DATA);
		std::vector<std::string> names;
		for (const kworld::LoadOrderEntry& entry : order.files)
			names.push_back(entry.name);

		flecs::world spawned;
		ksim::spawnDefaultWorld(spawned, gameData, towns);
		const std::uint64_t hash = ksim::stateHash(spawned);
		const std::string bytes = saveBytes(spawned, names);

		// Round trip into a world whose entity ids are shifted.
		flecs::world loaded;
		for (int i = 0; i < 1000; ++i)
			loaded.entity();
		loadBytes(bytes, loaded, names);
		test::require(ksim::stateHash(loaded) == hash, "loaded world hashes differently");
		test::require(saveBytes(loaded, names) == bytes, "re-saved bytes differ");
		test::require(loaded.get<ksim::state::Clock>()->tick
			== spawned.get<ksim::state::Clock>()->tick, "clock not restored");

		// Any saved value change shows in the hash.
		flecs::world changed;
		loadBytes(bytes, changed, names);
		bool moved = false;
		changed.each([&moved](ksim::state::Position& position)
		{
			if (!moved)
				position.x += 1.0;
			moved = true;
		});
		test::require(moved, "no squad has a Position");
		test::require(ksim::stateHash(changed) != hash, "a moved squad keeps the hash");

		// Same seed same hash; another seed differs.
		flecs::world again;
		ksim::spawnDefaultWorld(again, gameData, towns);
		test::require(ksim::stateHash(again) == hash, "same seed hashes differently");
		flecs::world other;
		ksim::SpawnOptions otherSeed;
		otherSeed.seed = 2;
		ksim::spawnDefaultWorld(other, gameData, towns, otherSeed);
		test::require(ksim::stateHash(other) != hash, "another seed hashes the same");

		// Load order mismatch: refused, or loaded with force and reported.
		std::vector<std::string> wrongOrder = names;
		wrongOrder.push_back("other.mod");
		bool refused = false;
		try
		{
			flecs::world wrong;
			loadBytes(bytes, wrong, wrongOrder);
		}
		catch (const ksim::SaveError&)
		{
			refused = true;
		}
		test::require(refused, "a load-order mismatch was not refused");
		flecs::world forced;
		std::istringstream forcedIn(bytes);
		test::require(!ksim::readSave(forcedIn, forced, wrongOrder, true).loadOrderMatches,
			"forced load did not report the mismatch");
		test::require(ksim::stateHash(forced) == hash, "forced load hashes differently");

		// Truncation and bad magic are errors, not partial loads.
		for (const std::string& broken : {bytes.substr(0, bytes.size() / 2),
			std::string("XSAV") + bytes.substr(4)})
		{
			bool threw = false;
			try
			{
				flecs::world bad;
				loadBytes(broken, bad, names);
			}
			catch (const ksim::SaveError&)
			{
				threw = true;
			}
			test::require(threw, "a broken save loaded");
		}

		// Through Simulation: the clock advances with ticks and survives.
		const std::string path = std::string(KSIM_BINARY_DIR) + "/ksim_save_test.ksav";
		std::uint64_t savedHash = 0;
		std::uint64_t savedClock = 0;
		{
			ksim::SpawnOptions settleOff; // the test pins the t0 clock
			settleOff.settleSeconds = 0;
			ksim::Simulation simulation = ksim::Simulation::newGame(KENSHI_DATA, order, settleOff);
			simulation.run(100);
			savedClock = simulation.clockTick();
			test::require(savedClock == spawned.get<ksim::state::Clock>()->tick + 100,
				"clock did not advance with ticks");
			test::require(simulation.world().ecs().count<ksim::state::Mover>()
				== simulation.world().ecs().count<ksim::state::Position>(),
				"a positioned instance has no Mover mirror");
			test::require(simulation.world().ecs().get<ksim::state::Scheduler>() != nullptr,
				"no Scheduler singleton");
			simulation.enqueue(ksim::Command::setPlayerAreas({{0.0, 0.0}}));
			savedHash = simulation.stateHash();
			simulation.saveGame(path);
		}
		ksim::Simulation restored = ksim::Simulation::loadGame(KENSHI_DATA, order, path);
		test::require(restored.clockTick() == savedClock, "loadGame clock differs");
		test::require(restored.stateHash() == savedHash, "loadGame hashes differently");
		test::require(restored.pendingCommands() == 1, "pending command not restored");
		test::require(restored.regions().entityCount()
			== static_cast<std::size_t>(restored.world().ecs().count<ksim::state::Position>()),
			"the region engine did not adopt every positioned instance");
		std::remove(path.c_str());

		std::cout << "ksim_save: " << bytes.size() << " bytes, hash " << std::hex
			<< hash << std::dec << "\n";
	}
	catch (const std::exception& error)
	{
		std::cerr << "ksim_save: " << error.what() << "\n";
		return 1;
	}
	return 0;
}
