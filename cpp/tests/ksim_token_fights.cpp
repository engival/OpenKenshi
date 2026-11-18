// Off-screen fights between hostile tokens (kenshi-k16.6.46; COMBAT_OPTIONS.md 9), behind Simulation::setOffscreenFights.
//   - The roll: P(A wins) = SA^2 / (SA^2 + SB^2) from fixed strengths, the frequencies over many ticks within tolerance, the winner's own loss
//     with the loser's chance, a pure function of the key, ceil(n/2) losses.
//   - A world: two staged tokens, one neutral and one factionless (hostile to everyone), meet; the log line appears with the losses it states;
//     with world.offscreen_medical off the fallen are corpses with cause 2, with it on they are KO'd bleeders in their MemberDelta; the pair does
//     not fight again within the game hour; setting off, nothing happens; the same state hash at 1 and 4 workers; a save right after a fight loads
//     to the same state.

#include "delta_scene.hpp"

#include "ksim/save.hpp"
#include "ksim/token_fight.hpp"

#include <cstdio>
#include <iostream>
#include <regex>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif
#ifndef KSIM_BINARY_DIR
#error KSIM_BINARY_DIR must be defined
#endif

namespace {

using namespace delta_scene;

std::shared_ptr<MeshNavigator> gNav;
const kworld::LoadOrder* gOrder = nullptr;
std::shared_ptr<const CombatData> gCombat;

void roll()
{
	using namespace tokenfight;
	test::require(winChance(0.0, 0.0) == 0.5, "no strength either side: even odds");
	test::require(std::abs(winChance(30.0, 10.0) - 0.9) < 1.0e-12, "P(A) = SA^2 / (SA^2 + SB^2): 900 / 1000");
	test::require(std::abs(winChance(10.0, 30.0) - 0.1) < 1.0e-12, "and the other way round");
	test::require(loserLosses(1) == 1 && loserLosses(2) == 1 && loserLosses(5) == 3 && loserLosses(8) == 4, "the loser loses ceil(n / 2)");
	const Outcome again = resolve(7, 3, 9, 1000, 20.0, 10.0);
	const Outcome swapped = resolve(7, 9, 3, 1000, 20.0, 10.0);
	test::require(again.aWins == swapped.aWins && again.winnerFalls == swapped.winnerFalls, "the key is (min id, max id, tick)");
	test::require(resolve(7, 3, 9, 1000, 20.0, 10.0).aWins == again.aWins, "a pure function of its key");
	// 20 : 10 -> P(A) = 0.8; over many ticks the frequency is within 3 sigma (n = 20000: sigma 0.0028).
	constexpr int n = 20000;
	int aWon = 0;
	int winnerFell = 0;
	int weakWon = 0;
	for (int i = 0; i < n; ++i)
	{
		const Outcome out = resolve(7, 3, 9, 100 + 20 * static_cast<std::uint64_t>(i), 20.0, 10.0);
		aWon += out.aWins ? 1 : 0;
		weakWon += out.aWins ? 0 : 1;
		winnerFell += (out.aWins && out.winnerFalls) ? 1 : 0;
	}
	const double freq = static_cast<double>(aWon) / n;
	std::cout << "P(A) 0.8: frequency " << freq << " over " << n << "\n";
	test::require(std::abs(freq - 0.8) < 0.01, "the stronger side wins with its odds");
	// The winner falls with 1 - P(winner wins) = 0.2 when A wins: P(A wins and falls) = 0.8 * 0.2 = 0.16.
	const double fell = static_cast<double>(winnerFell) / n;
	test::require(std::abs(fell - 0.16) < 0.01, "the winner's weakest falls with the loser's chance");
	test::require(weakWon > 0, "the weaker side wins sometimes");
	std::cout << "roll ok\n";
}

struct Game
{
	std::unique_ptr<Simulation> box;
	Scene scene;
	flecs::entity other; // the control's squad
	std::vector<std::string> lines;
};

std::unique_ptr<Simulation> boxed(int workers, std::uint64_t seed)
{
	SpawnOptions options;
	options.settleSeconds = 0.0; // pins the t0 state: no new-game settle (TOWN_LIFE_SLICE 5.40)
	options.seed = seed;
	options.wildernessByDensity = false;
	RegionConfig config;
	config.workerThreads = workers;
	std::unique_ptr<Simulation> sim(new Simulation(Simulation::newGame(KENSHI_DATA, *gOrder, options, nullptr, config)));
	startGame(*sim, gCombat, gNav);
	return sim;
}

// The two squads of the scene as tokens, side by side, one factionless (a team of its own, hostile to the neutral one).
Game stage(bool fights, bool medical, int workers = 1, std::uint64_t seed = 23)
{
	Game g;
	g.box = boxed(workers, seed);
	Simulation& sim = *g.box;
	sim.setOffscreenFights(fights);
	sim.setOffscreenMedical(medical);
	g.scene = pick(sim, 3);
	leave(sim, g.scene);
	g.other = g.scene.control.front().target<state::MemberOf>();
	state::SquadInstance instance = *g.other.get<state::SquadInstance>();
	instance.faction = {};
	g.other.set<state::SquadInstance>(instance);
	return g;
}

void collect(Game& g)
{
	g.box->setTaskLog([&g](const std::string& line)
	{
		if (line.rfind("offscreen fight", 0) == 0)
			g.lines.push_back(line);
	});
}

// Brings the two together and runs until a fight is logged.
void meet(Game& g)
{
	const state::Position at = *g.scene.squad.get<state::Position>();
	g.box->moveSquad(g.other, at.x + 30.0, at.z);
	runUntil(*g.box, [&] { return !g.lines.empty(); }, 400, "the two tokens never fought");
}

struct Losses
{
	std::size_t a = 0;
	std::size_t b = 0;
	std::uint64_t idA = 0;
	std::uint64_t idB = 0;
	std::uint64_t winner = 0;
};

Losses parse(const std::string& line)
{
	static const std::regex shape("^offscreen fight (\\d+) vs (\\d+) at -?\\d+,-?\\d+: (\\d+) wins p=[0-9.e-]+ losses A=(\\d+) B=(\\d+)$");
	std::smatch m;
	test::require(std::regex_match(line, m, shape), "the log line has its shape: " + line);
	return {std::stoull(m[4]), std::stoull(m[5]), std::stoull(m[1]), std::stoull(m[2]), std::stoull(m[3])};
}

std::size_t corpsesWithCause(Game& g, flecs::entity squad, std::uint8_t cause)
{
	std::size_t n = 0;
	for (const flecs::entity member : membersOf(*g.box, squad))
	{
		const state::CorpseDecay* decay = member.get<state::CorpseDecay>();
		const state::Fighter* f = member.get<state::Fighter>();
		n += (decay != nullptr && decay->cause == cause && f != nullptr && f->dead) ? 1 : 0;
	}
	return n;
}

std::size_t bleeders(Game& g, flecs::entity squad)
{
	std::size_t n = 0;
	for (const flecs::entity member : membersOf(*g.box, squad))
	{
		const state::MemberDelta* d = member.get<state::MemberDelta>();
		n += (!member.has<state::Fighter>() && d != nullptr && (d->mask & state::MemberDelta::kBody) != 0 && d->unconscious && !d->dead && !d->wounds.empty()) ? 1 : 0;
	}
	return n;
}

std::size_t living(Game& g, flecs::entity squad)
{
	std::size_t n = 0;
	for (const flecs::entity member : membersOf(*g.box, squad))
		n += (!member.has<state::Fighter>() || !member.get<state::Fighter>()->dead) ? 1 : 0;
	return n;
}

void world()
{
	// Medical off: the fallen die now, cause 2.
	{
		Game g = stage(true, false);
		collect(g);
		// Both have a home and a goal, so the loser's rout can be seen (the test world runs no group layer).
		flecs::entity town;
		ecsOf(*g.box).each([&](flecs::entity e, const state::TownState&) { town = town.is_valid() ? town : e; });
		for (const flecs::entity squad : {g.scene.squad, g.other})
		{
			state::SquadInstance instance = *squad.get<state::SquadInstance>();
			instance.homeTown.id = town.id();
			squad.set<state::SquadInstance>(instance);
			state::GroupAI ai;
			ai.goal = state::GoalKind::Idle;
			ai.homeTick = 777;
			squad.set<state::GroupAI>(ai);
		}
		const std::size_t before[2] = {living(g, g.scene.squad), living(g, g.other)};
		meet(g);
		const Losses l = parse(g.lines.front());
		const bool subjectIsA = idOf(g.scene.squad) == l.idA;
		const flecs::entity a = subjectIsA ? g.scene.squad : g.other;
		const flecs::entity b = subjectIsA ? g.other : g.scene.squad;
		std::cout << g.lines.front() << "\n";
		test::require(l.idA < l.idB, "A is the lower Id");
		test::require(corpsesWithCause(g, a, state::CorpseDecay::kCauseTokenFight) == l.a, "A's losses are corpses with cause 2");
		test::require(corpsesWithCause(g, b, state::CorpseDecay::kCauseTokenFight) == l.b, "B's losses are corpses with cause 2");
		test::require(l.a + l.b > 0, "somebody fell");
		const std::size_t was = subjectIsA ? before[0] : before[1];
		const std::size_t wasOther = subjectIsA ? before[1] : before[0];
		const std::size_t loserWas = l.winner == l.idA ? wasOther : was;
		const std::size_t loserLost = l.winner == l.idA ? l.b : l.a;
		test::require(loserLost == (loserWas + 1) / 2, "the loser lost ceil(n / 2) of its living members");
		test::require(living(g, a) + l.a == (subjectIsA ? before[0] : before[1]), "A's living members are what is left");
		const flecs::entity loserSquad = l.winner == l.idA ? b : a;
		const flecs::entity winnerSquad = l.winner == l.idA ? a : b;
		const bool routed = loserSquad.get<state::GroupAI>()->goal == state::GoalKind::None && loserSquad.get<state::GroupAI>()->homeTick == 0;
		test::require(living(g, loserSquad) == 0 || routed, "the loser routs: its group layer picks the way home");
		test::require(winnerSquad.get<state::GroupAI>()->goal == state::GoalKind::Idle && winnerSquad.get<state::GroupAI>()->homeTick == 777, "the winner keeps its goal");
		// A save right after the fight loads to the same state.
		const std::string path = std::string(KSIM_BINARY_DIR) + "/ksim_token_fights.ksav";
		g.box->saveGame(path);
		Simulation loaded = Simulation::loadGame(KENSHI_DATA, *gOrder, path, false, nullptr, RegionConfig{});
		std::remove(path.c_str());
		test::require(loaded.stateHash() == g.box->stateHash(), "save and load right after a fight round-trips the state hash");
		// The pair does not fight again within the game hour.
		g.box->run(tokenfight::kPairTicks / 2);
		test::require(g.lines.size() == 1, "the same pair fights at most once a game hour");
	}

	// Medical on: the fallen are KO'd bleeders in their MemberDelta; nobody is dead yet.
	{
		Game g = stage(true, true);
		collect(g);
		meet(g);
		const Losses l = parse(g.lines.front());
		const bool subjectIsA = idOf(g.scene.squad) == l.idA;
		const flecs::entity a = subjectIsA ? g.scene.squad : g.other;
		const flecs::entity b = subjectIsA ? g.other : g.scene.squad;
		test::require(corpsesWithCause(g, a, state::CorpseDecay::kCauseTokenFight) + corpsesWithCause(g, b, state::CorpseDecay::kCauseTokenFight) == 0, "medical on: nobody dies in the fight itself");
		test::require(bleeders(g, a) == l.a && bleeders(g, b) == l.b, "medical on: the fallen are KO'd and bleeding in their MemberDelta");
		test::require(l.a + l.b > 0, "somebody fell");
	}

	// Setting off: the same meeting does nothing.
	{
		Game g = stage(false, false);
		collect(g);
		const state::Position at = *g.scene.squad.get<state::Position>();
		g.box->moveSquad(g.other, at.x + 30.0, at.z);
		const std::size_t a = living(g, g.scene.squad);
		g.box->run(400);
		test::require(g.lines.empty() && living(g, g.scene.squad) == a, "setting off: no fight");
	}

	// Two neutral tokens (the same team) do not fight.
	{
		Game g = stage(true, false);
		collect(g);
		state::SquadInstance instance = *g.other.get<state::SquadInstance>();
		instance.faction = g.scene.squad.get<state::SquadInstance>()->faction;
		g.other.set<state::SquadInstance>(instance);
		const state::Position at = *g.scene.squad.get<state::Position>();
		g.box->moveSquad(g.other, at.x + 30.0, at.z);
		g.box->run(400);
		test::require(g.lines.empty(), "squads of one faction do not fight");
	}

	// The same state hash at 1 and 4 workers.
	{
		std::uint64_t hash[2] = {0, 0};
		std::string line[2];
		int i = 0;
		for (const int workers : {1, 4})
		{
			Game g = stage(true, true, workers);
			collect(g);
			meet(g);
			g.box->run(600);
			hash[i] = g.box->stateHash();
			line[i++] = g.lines.front();
		}
		test::require(line[0] == line[1], "the same fight at 1 and 4 workers");
		test::require(hash[0] == hash[1], "the same state hash at 1 and 4 workers");
	}
	std::cout << "token fights ok\n";
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		roll();
		test::require(argc > 1, "argv[1]: the Hub navmesh cache");
		gNav = MeshNavigator::load(argv[1]);
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);
		gOrder = &order;
		{
			Simulation probe = Simulation::newGame(KENSHI_DATA, order, SpawnOptions{}, nullptr, RegionConfig{});
			gCombat = CombatData::build(*probe.gameData(), KENSHI_DATA);
		}
		world();
	} catch (const std::exception& e) {
		std::cerr << "ksim_token_fights: FAILED: " << e.what() << "\n";
		return 1;
	}
	return 0;
}
