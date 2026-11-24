// Startup benchmark (kenshi-k16.15.44, measurement only): wall time per phase from process start to the first sim tick, for the
// three ways a test opens the game: `rig` (scenario_rig::Rig), `town` (newGame + CombatData), `simlink` (merged data + CombatData +
// a baked Hub mesh). Not a ctest; built only with -DKSIM_STARTUP_BENCH=ON. Usage: ksim_startup_bench <rig|town|simlink> [navworld]
#include "ksim/ai/ai.hpp"
#include "ksim/ai/behaviours.hpp"
#include "ksim/combat/combat_data.hpp"
#include "ksim/core/simulation.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/net/simlink_scenario.hpp"
#include "ksim/world/factions.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"
#include "nav_source.hpp"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>

using Clock = std::chrono::steady_clock;
static Clock::time_point last;

static long rssMb()
{
	std::ifstream in("/proc/self/status");
	std::string line;
	while (std::getline(in, line))
		if (line.rfind("VmRSS:", 0) == 0)
			return std::stol(line.substr(6)) / 1024;
	return -1;
}

static void lap(const std::string& name)
{
	const auto now = Clock::now();
	std::printf("PHASE %-34s %9.1f ms  rss %ld MB\n", name.c_str(), std::chrono::duration<double, std::milli>(now - last).count(), rssMb());
	last = Clock::now();
}

// FNV-1a over every field of every merged record, in merge order: the before/after identity check for parser / merge changes.
struct Fnv
{
	std::uint64_t h = 1469598111934665603ull;
	void bytes(const void* p, std::size_t n)
	{
		for (std::size_t i = 0; i < n; ++i)
			h = (h ^ static_cast<const unsigned char*>(p)[i]) * 1099511628211ull;
	}
	void str(const std::string& v) { const std::size_t n = v.size(); bytes(&n, sizeof n); bytes(v.data(), n); }
	template <typename T> void raw(const T& v) { bytes(&v, sizeof v); }
};

static std::uint64_t hashGameData(const kdata::GameData& gd)
{
	Fnv f;
	for (const kdata::Record& r : gd.records())
	{
		f.raw(r.type); f.raw(r.id); f.str(r.name); f.str(r.stringID); f.raw(r.hasFlags); f.raw(r.flags); f.raw(r.hasTags);
		for (const auto& t : r.tags) { f.str(t.key); f.raw(t.keep); }
		for (const auto& x : r.bools) { f.str(x.key); f.raw(x.value); }
		for (const auto& x : r.floats) { f.str(x.key); f.raw(x.value); }
		for (const auto& x : r.ints) { f.str(x.key); f.raw(x.value); }
		for (const auto& x : r.vec3s) { f.str(x.key); f.raw(x.value); }
		for (const auto& x : r.vec4s) { f.str(x.key); f.raw(x.value); }
		for (const auto& x : r.strings) { f.str(x.key); f.str(x.value); }
		for (const auto& x : r.files) { f.str(x.key); f.str(x.value); }
		for (const auto& c : r.references)
		{
			f.str(c.category);
			for (const auto& e : c.entries) { f.str(e.targetID); f.raw(e.v0); f.raw(e.v1); f.raw(e.v2); f.raw(e.hasV1V2); f.raw(e.opaqueEntry); f.raw(e.opaque); }
		}
		for (const auto& i : r.instances)
		{
			f.str(i.id); f.raw(i.idInt); f.raw(i.idIsString); f.str(i.targetStringID); f.raw(i.transform);
			for (const auto& st : i.states) f.str(st);
			for (auto st : i.stateInts) f.raw(st);
		}
	}
	return f.h;
}

int main(int argc, char** argv)
{
	const std::string mode = argc > 1 ? argv[1] : "rig";
	const std::string world = argc > 2 ? argv[2] : "";
	const auto start = Clock::now();
	last = start;
	using namespace ksim;
	SpawnOptions spawn;
	spawn.seed = mode == "rig" ? 7 : 1;
	if (mode == "town")
		spawn.wildernessByDensity = false;
	spawn.stageHook = [](const std::string& text) { lap("newGame: " + text.substr(0, 22)); };
	if (mode == "hash")
	{
		// phase 1 alone, vanilla order and the rig's full order: time + fingerprint
		for (const kworld::LoadOrder& order : {kworld::vanillaLoadOrder(KENSHI_DATA), kworld::resolveLoadOrder(KENSHI_DATA, "")})
		{
			last = Clock::now();
			const kdata::GameData gd = kworld::loadMergedGameData(order);
			lap("loadMergedGameData");
			std::printf("HASH %zu records %016llx\n", gd.size(), static_cast<unsigned long long>(hashGameData(gd)));
		}
		return 0;
	}
	if (mode == "simlink")
	{
		const kworld::LoadOrder order = kworld::resolveLoadOrder(KENSHI_DATA);
		lap("resolve load order");
		const auto gd = std::make_shared<const kdata::GameData>(kworld::loadMergedGameData(order));
		lap("loadMergedGameData (kdata)");
		const auto data = CombatData::build(*gd, KENSHI_DATA);
		lap("CombatData::build");
		const auto nav = MeshNavigator::bake(KENSHI_DATA, {simlink::kHubZoneX, simlink::kHubZoneZ}, {simlink::kHubZoneX, simlink::kHubZoneZ}, 4);
		lap("MeshNavigator::bake (Hub)");
	} else {
		const kworld::LoadOrder order = mode == "rig" ? kworld::resolveLoadOrder(KENSHI_DATA, "") : kworld::vanillaLoadOrder(KENSHI_DATA);
		lap("resolve load order");
		Simulation sim = Simulation::newGame(KENSHI_DATA, order, spawn, nullptr, RegionConfig{});
		lap("newGame: tail (weather/traders/econ)");
		sim.markPristine();
		const auto gameData = sim.gameData();
		const auto combat = CombatData::build(*gameData, KENSHI_DATA);
		lap("CombatData::build");
		CombatSettings settings;
		settings.substeps = 2;
		sim.enableCombat(combat, settings);
		lap("enableCombat");
		if (mode == "rig")
		{
			// the nav window the caravan / chase scenes open: the 3 x 3 zones round Heft
			std::vector<kworld::ZoneCell> zones;
			for (int dz = -1; dz <= 1; ++dz)
				for (int dx = -1; dx <= 1; ++dx)
					zones.push_back({simlink::kHubZoneX + dx, simlink::kHubZoneZ + dz});
			const auto mesh = test::openWorldNav(world, zones);
			lap("openWorldNav (terrain+nav cache)");
			sim.enableNavigation(mesh);
			lap("enableNavigation");
			AiSystem::Config ai;
			ai.registry = std::make_shared<BehaviourRegistry>(sim.world(), BehaviourRegistry::readRules(KSIM_AI_TABLE));
			lap("BehaviourRegistry (AI table)");
			ai.navigator = mesh;
			ai.factions = FactionBook::build(sim.gameData());
			lap("FactionBook::build");
			sim.enableAi(std::move(ai));
			lap("enableAi");
		}
		sim.newWorld();
		sim.run(1);
		lap("newWorld + first tick");
	}
	std::printf("TOTAL %.1f ms  rss %ld MB\n", std::chrono::duration<double, std::milli>(Clock::now() - start).count(), rssMb());
	return 0;
}
