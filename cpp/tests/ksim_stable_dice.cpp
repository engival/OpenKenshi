// A fighter's combat dice hang on its stable identity, not on its slot in the global Id sequence (task CC). Before this, a fighter's
// combat stream (kCombatStream), its comfort radii (kAvoidStream), the ragdoll, the follow formation and the sensor's scan stagger and
// noise roll were all keyed on state::Id, so anything created before it (an interior-layout storage box, a new spawn) moved every Id
// and re-rolled every fight: the fight fixtures had to be re-pinned after every such change, and the suite could not tell a behaviour
// change from a dice change. They now hang on Fighter::diceSeed, the character's own stable key.
//
// The test loads the same KSCEN fight twice (flat1.kscen, the hand-written 1v1, the player ordered to attack at tick 80) and gives
// the second run one extra unrelated entity before the scenario spawns (a bare state::Id entity: a storage box would do the same to
// the Ids). The two runs must then be bit-identical, tick by tick, through the whole swing/impact/health sequence: positions, yaw,
// technique state, blood, every body part and the fighter's own rng state.
//
// The fight needs the world navmesh bake (argv[1], exit 77 without it, like ksim_kscen_fight).
#include "ksim/core/clock.hpp"
#include "ksim/combat/combat.hpp"
#include "ksim/combat/combat_data.hpp"
#include "ksim/core/command.hpp"
#include "ksim/save/scenario_file.hpp"
#include "ksim/net/simlink.hpp"
#include "ksim/net/simlink_scenario.hpp"
#include "ksim/core/simulation.hpp"
#include "ksim/core/state.hpp"

#include "knav/navmesh.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include "nav_source.hpp"
#include "ksim/dev/build_paths.hpp"

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;

constexpr std::uint64_t kOrderTick = 80; // the client's script: the attack order goes in here
constexpr std::uint64_t kFightTicks = 2400; // the window after the order: 120 s, the ksim_kscen_fight window

std::uint64_t fnv(std::uint64_t hash, const void* data, std::size_t size)
{
	const auto* bytes = static_cast<const unsigned char*>(data);
	for (std::size_t i = 0; i < size; ++i)
	{
		hash ^= bytes[i];
		hash *= 1099511628211ull;
	}
	return hash;
}

// One hash per named field, so a divergence names the field that moved (and not the first one only through a digest).
struct Field
{
	std::string name;
	std::uint64_t hash;
};

template <typename T>
	requires std::is_trivially_copyable_v<T>
void field(std::vector<Field>& out, const char* name, const T& value)
{
	out.push_back({name, fnv(1469598103934665603ull, &value, sizeof(T))});
}

// An id field as a role: the run's ids differ by design, only which fighter it names matters.
std::uint64_t roleOf(std::uint64_t id, std::uint64_t player, std::uint64_t enemy)
{
	return id == 0 ? 0 : id == player ? 1 : id == enemy ? 2 : 3;
}

// The fighter's whole combat-relevant state: if any of this differs, the fight took another turn. `player` / `enemy` are the run's
// two ids, so the fields that hold an Id compare by role.
std::vector<Field> fighterFields(const state::Fighter& f, std::uint64_t player, std::uint64_t enemy)
{
	std::vector<Field> out;
	field(out, "x", f.x);
	field(out, "y", f.y);
	field(out, "z", f.z);
	field(out, "yaw", f.yaw);
	field(out, "aimYaw", f.aimYaw);
	field(out, "forwardSpeed", f.forwardSpeed);
	field(out, "lateralSpeed", f.lateralSpeed);
	field(out, "velX", f.velX);
	field(out, "velZ", f.velZ);
	field(out, "state", f.state);
	field(out, "pending", f.pending);
	field(out, "technique", f.technique);
	field(out, "clipTime", f.clipTime);
	field(out, "clipSpeed", f.clipSpeed);
	field(out, "techniqueFinished", f.techniqueFinished);
	field(out, "impactIndex", f.impactIndex);
	field(out, "lastImpact", f.lastImpact);
	field(out, "swingProgress", f.swingProgress);
	field(out, "recovering", f.recovering);
	field(out, "recoverTimer", f.recoverTimer);
	field(out, "actionTimer", f.actionTimer);
	field(out, "reactTimer", f.reactTimer);
	field(out, "reactClip", f.reactClip);
	field(out, "reactTime", f.reactTime);
	field(out, "stateTime", f.stateTime);
	field(out, "stumbleTimer", f.stumbleTimer);
	field(out, "escapeTimer", f.escapeTimer);
	field(out, "recoilX", f.recoilX);
	field(out, "recoilZ", f.recoilZ);
	field(out, "circleTimer", f.circleTimer);
	field(out, "circleOffset", f.circleOffset);
	field(out, "previousTechnique", f.previousTechnique);
	field(out, "cachedReach", f.cachedReach);
	field(out, "blood", f.blood);
	field(out, "bleeding", f.bleeding);
	field(out, "koTimer", f.koTimer);
	field(out, "unconscious", f.unconscious);
	field(out, "dead", f.dead);
	field(out, "down", f.down);
	field(out, "target", roleOf(f.target, player, enemy));
	field(out, "targetAge", f.targetAge);
	field(out, "orderTarget", roleOf(f.orderTarget, player, enemy));
	field(out, "diceSeed", f.diceSeed);
	field(out, "rng", f.rng);
	field(out, "comfortRolls", f.comfortRolls);
	field(out, "senseClock", f.senseClock);
	field(out, "engaged", static_cast<std::uint64_t>(f.engaged.size()));
	field(out, "slots", static_cast<std::uint64_t>(f.slots.size()));
	for (const state::FighterPart& part : f.parts)
		out.push_back({part.name.c_str(), fnv(1469598103934665603ull, &part.health, sizeof(part.health))});
	field(out, "wounds", static_cast<std::uint64_t>(f.wounds.size()));
	for (const double wound : f.wounds)
		field(out, "wound", wound);
	return out;
}

struct Run
{
	std::uint64_t player = 0;
	std::uint64_t enemy = 0;
	std::uint64_t playerSwings = 0;
	std::uint64_t enemySwings = 0;
	std::uint64_t playerHits = 0;
	std::uint64_t enemyHits = 0;
	// One tick's fields: the player's, then the enemy's.
	std::vector<std::vector<Field>> trace;
};

Run fight(const kworld::LoadOrder& order, const std::string& bake, bool extraEntity)
{
	SpawnOptions spawn;
	spawn.seed = 1;
	Simulation sim = Simulation::newGame(KENSHI_DATA, order, spawn);
	const std::shared_ptr<const kdata::GameData> gameData = sim.gameData();
	sim.enableWeather(*gameData);
	const std::shared_ptr<const CombatData> combatData = CombatData::build(*gameData, KENSHI_DATA);
	CombatSettings settings;
	settings.substeps = 2;
	sim.enableCombat(combatData, settings);
	std::vector<kworld::ZoneCell> zones;
	for (int dz = -1; dz <= 1; ++dz)
	{
		for (int dx = -1; dx <= 1; ++dx)
			zones.push_back({simlink::kHubZoneX + dx, simlink::kHubZoneZ + dz});
	}
	sim.enableNavigation(test::openWorldNav(bake, zones));
	// The unrelated entity, before the scenario's characters exist: it takes the next Id and moves every Id after it. A storage box
	// carries more components, but a bare state::Id entity moves the sequence the same way.
	if (extraEntity)
		state::createInstance(const_cast<flecs::world&>(sim.world().ecs()));
	simlink::ServerContext context;
	context.gameData = gameData;
	const scenario::File file = scenario::parseFile(KSIM_KSCEN_FIXTURE);
	const scenario::Placed placed = scenario::apply(sim, *gameData, *combatData, context, file, 1);
	test::require(placed.players.size() == 1 && placed.others.size() == 1, "the fixture is a 1v1");
	Run run;
	run.player = placed.players.front();
	run.enemy = placed.others.front();
	sim.setCombatObserver([&](const CombatEvent& e)
	{
		if (e.kind == CombatEvent::Kind::TechniqueStart)
		{
			if (e.who == run.player) ++run.playerSwings;
			if (e.who == run.enemy) ++run.enemySwings;
		}
		if (e.kind == CombatEvent::Kind::Hit)
		{
			if (e.who == run.enemy) ++run.playerHits;
			if (e.who == run.player) ++run.enemyHits;
		}
	});
	const std::uint64_t start = sim.clockTick();
	for (std::uint64_t i = 0; i < kOrderTick + kFightTicks; ++i)
	{
		if (sim.clockTick() - start == kOrderTick)
			sim.enqueue(Command::fighterAttack(state::Id{run.player}, state::Id{run.enemy}));
		sim.tick();
		const state::Fighter* player = sim.combat()->find(run.player);
		const state::Fighter* enemy = sim.combat()->find(run.enemy);
		test::require(player != nullptr && enemy != nullptr, "both fighters are in the arena");
		std::vector<Field> fields = fighterFields(*player, run.player, run.enemy);
		const std::vector<Field> other = fighterFields(*enemy, run.player, run.enemy);
		fields.insert(fields.end(), other.begin(), other.end());
		run.trace.push_back(std::move(fields));
	}
	return run;
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		if (argc < 2 || !test::worldRegionReady(std::string(argv[1]), simlink::kHubZoneX, simlink::kHubZoneZ))
		{
			std::cout << "ksim_stable_dice: SKIPPED: needs the world navmesh bake (pass its directory)\n";
			return 77;
		}
		const std::string bake(argv[1]);
		const kworld::LoadOrder order = kworld::resolveLoadOrder(KENSHI_DATA, "");

		const Run plain = fight(order, bake, false);
		const Run shifted = fight(order, bake, true);
		// The point of the test: the extra entity did move the Ids (otherwise nothing was proven).
		test::require(shifted.player == plain.player + 1,
			"the extra entity moved the fighters' Ids by one (plain " + std::to_string(plain.player) + ", shifted " + std::to_string(shifted.player) + ")");
		// The fight is real: both sides swung and landed.
		test::require(plain.playerSwings > 0 && plain.enemySwings > 0 && plain.playerHits > 0 && plain.enemyHits > 0,
			"the fight went both ways (player " + std::to_string(plain.playerSwings) + " swings / " + std::to_string(plain.playerHits) + " hits, enemy "
				+ std::to_string(plain.enemySwings) + " / " + std::to_string(plain.enemyHits) + ")");
		test::require(plain.trace.size() == shifted.trace.size(), "the two runs traced the same number of ticks");
		for (std::size_t i = 0; i < plain.trace.size(); ++i)
		{
			const std::vector<Field>& a = plain.trace[i];
			const std::vector<Field>& b = shifted.trace[i];
			test::require(a.size() == b.size(), "the two runs hashed the same fields");
			for (std::size_t f = 0; f < a.size(); ++f)
			{
				if (a[f].hash == b[f].hash)
					continue;
				test::require(false, "the fight diverged at tick " + std::to_string(i) + " in field '" + a[f].name + "' (player Id "
					+ std::to_string(plain.player) + " vs " + std::to_string(shifted.player)
					+ "): an unrelated entity must not move a fighter's dice");
			}
		}
		std::cout << "ksim_stable_dice: " << plain.trace.size() << " ticks bit-identical with an unrelated entity spawned first (Id +1)\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_stable_dice: " << e.what() << "\n";
		return 1;
	}
}
