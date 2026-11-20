// The manhunters' hunt (task CA; SLAVERY.md stages 1-2, kinspect dt7). The Manhunters' dialogue package 54936 roots 54937 "Man Hunter
// slave recapture shout" under EV_I_SEE_NEUTRAL_SQUAD / EV_I_SEE_ENEMY_PLAYER, gated by DC_IN_A_NON_PLAYER_TOWN == 0 (and, for the
// Slave Hunters' 54967, DC_COPS_AROUND == 0): a patrol hunts in the wild and never in a town. The line's DA_ATTACK_CHASE_FOREVER starts
// the fight; after the KO the capture chain runs through the world package's CAPTURE_NEW_SLAVES 167 row (task BW's selector).
//
// 1. the wild: a real manhunter patrol (SQUAD_TEMPLATE 55016-rebirth.mod, character 54588) placed by the player, no forced dialogue.
//    The I_SEE producer fires, the gate passes, the root is picked, and a manhunter targets the player (the chase-forever fight); the
//    patrol then puts the player down and a member takes 167 on the body, which ends in the cuff (slave state 1).
// 2. a non-player town (the Hub): the same patrol and player inside the Hub's radius. The I_SEE events fire and the root is offered,
//    but the town condition refuses it: no pick of 54937 and no manhunter ever targets the player.
//
// The root's second line (54941) is a 40/100 permanent-chance line: a speaker whose roll fails says nothing, and nothing is stamped,
// so the 30 s I_SEE re-fire tries again (several speakers per attempt). The fixture waits through those windows rather than forcing it.
// Needs the world bake (argv[1]); exit 77 without it. KSIM_MANHUNTER_VERBOSE=1 prints the timeline.
#include "ksim/ai/ai.hpp"
#include "ksim/ai/behaviours.hpp"
#include "ksim/core/clock.hpp"
#include "ksim/combat/combat_data.hpp"
#include "ksim/core/command.hpp"
#include "ksim/dialogue/dialogue_book.hpp"
#include "ksim/dialogue/dialogue_events.hpp"
#include "ksim/world/factions.hpp"
#include "ksim/ai/routines.hpp"
#include "ksim/net/simlink_scenario.hpp"
#include "ksim/core/simulation.hpp"
#include "ksim/core/state.hpp"

#include "knav/navmesh.hpp"
#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "nav_source.hpp"
#include "sim_access.hpp"
#include "ksim/dev/build_paths.hpp"

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;

// The wilderness point ksim_capture_live / ksim_after_fight use (outside every town).
constexpr double kWildX = -48880.0;
constexpr double kWildZ = -823.0;
// The manhunter patrol (rebirth.mod): character 54588 "Manhunter", faction 54587, 5-14 members.
constexpr std::string_view kPatrol = "55016-rebirth.mod";
// The package root the hunt starts from (Dialogue.mod).
constexpr std::string_view kRoot = "54937-Dialogue.mod";
// The Hub's TOWN record, for the town-radius precondition of the second case.
constexpr std::string_view kHubTown = "18919-Newwworld.mod";

std::ostream& say()
{
	static std::ostream nowhere(nullptr);
	return std::getenv("KSIM_MANHUNTER_VERBOSE") != nullptr ? std::cerr : nowhere;
}

flecs::entity entityOf(Simulation& sim, std::uint64_t id)
{
	flecs::entity found;
	ecsOf(sim).each([&](flecs::entity e, const state::Id& i, const state::Fighter&) { found = i.value == id ? e : found; });
	return found;
}

struct Person
{
	std::uint64_t id = 0;
	flecs::entity entity;
};

std::vector<Person> membersOf(Simulation& sim, flecs::entity squad)
{
	std::vector<Person> found;
	ecsOf(sim).query_builder<const state::Id>().with<state::MemberOf>(squad).build().each([&](flecs::entity member, const state::Id& id)
	{
		found.push_back({id.value, member});
	});
	std::sort(found.begin(), found.end(), [](const Person& a, const Person& b) { return a.id < b.id; });
	return found;
}

bool isManhunter(const std::vector<Person>& members, std::uint64_t id)
{
	return std::any_of(members.begin(), members.end(), [id](const Person& p) { return p.id == id; });
}

struct Game
{
	std::string world;
	kworld::LoadOrder order;
	std::shared_ptr<const CombatData> combat;
	std::shared_ptr<const DialogueBook> book;
	std::shared_ptr<MeshNavigator> mesh;
	std::unique_ptr<Simulation> sim;
	std::uint64_t player = 0;
	std::vector<std::string> taskLog;

	explicit Game(const std::string& bake)
		: world(bake)
		, order(kworld::resolveLoadOrder(KENSHI_DATA, ""))
	{
		std::vector<kworld::ZoneCell> hubZones;
		for (int dz = -1; dz <= 1; ++dz)
			for (int dx = -1; dx <= 1; ++dx)
				hubZones.push_back({simlink::kHubZoneX + dx, simlink::kHubZoneZ + dz});
		mesh = test::openWorldNav(bake, hubZones);
	}

	void newGame(std::uint64_t seed = 1)
	{
		SpawnOptions options;
		options.seed = seed;
		sim = std::make_unique<Simulation>(Simulation::newGame(KENSHI_DATA, order, options, nullptr, RegionConfig{}));
		sim->markPristine();
		combat = CombatData::build(*sim->gameData(), KENSHI_DATA);
		CombatSettings s;
		s.substeps = 2;
		// The body must survive the beating long enough for the cuff (the capture chain wants a down, not a corpse).
		s.chanceOfDeath = 0.0;
		sim->enableCombat(combat, s);
		book = DialogueBook::build(sim->gameData());
		sim->enableDialogue(book);
		sim->enableNavigation(mesh);
		AiSystem::Config ai;
		ai.registry = std::make_shared<BehaviourRegistry>(sim->world(), BehaviourRegistry::readRules(KSIM_AI_TABLE));
		ai.navigator = KnavNavigator::open(KENSHI_DATA, world, 4, 7, 6, 9, 4);
		ai.factions = FactionBook::build(sim->gameData());
		sim->enableAi(std::move(ai));
		sim->setTaskLog([this](const std::string& line) { taskLog.push_back(line); });
		std::string sid;
		for (const kdata::Record* record : sim->gameData()->byType(kworld::itemTypeByName("NEW_GAME_STARTOFF")))
			if (record->name == "Wanderer")
				sid = record->stringID;
		sim->enqueue(Command::newGame(sid, "Tester", 1));
		sim->run(3);
		for (const CombatArena::Member& member : sim->combat()->members())
			if (member.fighter.playerControlled)
				player = member.id;
	}

	state::Fighter& me() { return *fighterOf(*sim, player); }

	void teleport(double x, double z)
	{
		state::Fighter& f = me();
		f.x = x;
		f.z = z;
		f.y = sim->groundHeight(x, z).value_or(f.y);
		f.clearRoute();
		f.building = 0;
	}
};

// The Hub's own TownState (where the world model places the town), null when the world has none.
const state::TownState* townState(Simulation& sim, std::string_view rootSid)
{
	const state::TownState* found = nullptr;
	ecsOf(sim).each([&](const state::TownState& town) { found = town.rootSid == rootSid ? &town : found; });
	return found;
}

// Waits for a placed squad's members to be built at Full detail (they are tokens until then).
std::vector<Person> builtMembers(Simulation& sim, flecs::entity squad, double seconds = 20.0)
{
	std::vector<Person> members;
	for (int i = 0; i < static_cast<int>(20 * seconds); ++i)
	{
		members = membersOf(sim, squad);
		if (members.size() >= 3
			&& std::all_of(members.begin(), members.end(), [&](const Person& p) { return sim.combat()->find(p.id) != nullptr; }))
			break;
		sim.run(1);
	}
	return membersOf(sim, squad);
}

// Case 1: the wild hunt (dialogue -> chase-forever fight -> KO -> the 167 capture chain).
void wildHunt(const std::string& bake)
{
	std::cout << "== the manhunter patrol in the wild\n";
	Game game(bake);
	game.newGame();
	Simulation& sim = *game.sim;
	sim.run(200);
	const std::uint32_t root = game.book->dialogueIndex(kRoot);
	test::require(root != kNoIndex, "the Manhunters' root 54937 is in the merged dialogue book");
	std::vector<std::pair<std::string, std::uint64_t>> heard; // (line sid, voice)
	sim.setSpeechObserver([&](const Simulation::SpeechEvent& e)
	{
		heard.emplace_back(e.line, e.speaker);
		say() << "  speech " << e.speaker << " -> " << e.target << " " << e.line << " '" << e.text << "' " << e.seconds << " s"
			<< (e.end ? " (end)" : "") << "\n";
	});

	game.teleport(kWildX, kWildZ);
	sim.run(2);
	const flecs::entity squad = sim.placeWildernessSquad(kPatrol, kWildX + 6.0, kWildZ + 6.0);
	std::vector<Person> members = builtMembers(sim, squad);
	test::require(members.size() >= 3, "the manhunter patrol is built (5-14 members in the data)");
	std::cout << "patrol " << members.size() << " members\n";
	for (const Person& p : members)
	{
		const state::Fighter* f = sim.combat()->find(p.id);
		const state::CharacterInstance* who = p.entity.get<state::CharacterInstance>();
		say() << "  member " << p.id << " character " << (who != nullptr ? who->templateSid : std::string("?")) << " at " << f->x << ","
			<< f->z << "\n";
	}

	// The dialogue and the fight. The root may be refused by the line's own 40/100 chance; the I_SEE re-fire (30 s) retries, and every
	// attempt evaluates every aware manhunter, so the wait spans several windows.
	std::uint64_t dialogueTick = 0;
	std::uint64_t dialogueSpeaker = 0;
	std::uint64_t attackTick = 0;
	std::uint64_t attacker = 0;
	std::size_t seen = 0;
	std::uint32_t offered = 0; // picks whose candidate pool held the root (the gate ran)
	for (int i = 0; i < 20 * 240 && attackTick == 0; ++i)
	{
		sim.run(1);
		// The line's "chance permanent" is rolled once per (speaker, record) by a hash (dialogue_events.cpp), so a patrol whose members all roll
		// above it never speaks: another patrol (fresh ids, fresh rolls) joins every 40 s until one does, up to three more.
		if (dialogueTick == 0 && i % 800 == 799 && i < 20 * 160)
		{
			const std::vector<Person> more = builtMembers(sim, sim.placeWildernessSquad(kPatrol, kWildX + 6.0, kWildZ + 6.0));
			members.insert(members.end(), more.begin(), more.end());
			std::cout << "no member passed the root's permanent chance: another patrol, " << members.size() << " members\n";
		}
		const std::deque<DialoguePick>& log = sim.dialogue()->log();
		if (log.size() < DialogueSystem::kLogCap)
		{
			for (; seen < log.size(); ++seen)
			{
				const DialoguePick& pick = log[seen];
				if (!isManhunter(members, pick.event.speaker) || (pick.event.event != ev::ISeeNeutralSquad && pick.event.event != ev::ISeeEnemyPlayer))
					continue;
				if (pick.candidates > 0)
					++offered;
				if (pick.result == DialoguePick::Result::Picked && pick.dialogue == root && dialogueTick == 0)
				{
					dialogueTick = sim.clockTick();
					dialogueSpeaker = pick.event.speaker;
					say() << "  picked 54937 by " << dialogueSpeaker << " at tick " << dialogueTick << ", line "
						<< (pick.line != kNoIndex ? std::string(game.book->sidOf(game.book->line(pick.line).sid)) : std::string("none")) << "\n";
				}
			}
		}
		for (const Person& p : members)
		{
			const state::Fighter* f = sim.combat()->find(p.id);
			if (f != nullptr && f->target == game.player)
			{
				attackTick = sim.clockTick();
				attacker = p.id;
				break;
			}
		}
	}
	{
		const DialogueCoverage& cov = sim.dialogue()->coverage();
		say() << "  coverage: interjectionsSkipped " << cov.interjectionsSkipped << " replyLinesSkipped " << cov.replyLinesSkipped
			<< " repetitionFails " << cov.repetitionFails << " condFailClosed ";
		for (int kind = 0; kind < kDialogueCondCount; ++kind)
			if (cov.condFailClosed[static_cast<std::size_t>(kind)] != 0)
				say() << dialogueCondName(kind) << "=" << cov.condFailClosed[static_cast<std::size_t>(kind)] << " ";
		say() << "\n";
	}
	test::require(offered > 0, "the I_SEE events reach the Manhunters' root (the gate ran)");
	test::require(dialogueTick != 0, "the hunt dialogue fires (root 54937 is picked for a manhunter)");
	test::require(attackTick != 0, "the chase-forever fight starts (a manhunter targets the player)");
	// The attack line 54953 is reached through an interjection node: a squad mate (not the conversation's owner) says its T_INTERJECTOR1
	// child (54946 or 55096) -- the slots slice 2 skipped, so without the interjector's voice the hunt stops at 54941.
	const bool interjected = std::any_of(heard.begin(), heard.end(), [&](const std::pair<std::string, std::uint64_t>& said)
	{
		return (said.first == "54946-Dialogue.mod" || said.first == "55096-rebirth.mod") && said.second != dialogueSpeaker;
	});
	test::require(interjected, "the interjector says the T_INTERJECTOR1 line that leads to the attack");
	test::require(attackTick >= dialogueTick, "the fight follows the dialogue");
	std::cout << "root 54937 picked by member " << dialogueSpeaker << "; a manhunter (" << attacker << ") targets the player "
		<< (attackTick - dialogueTick) * kTickSeconds << " s later\n";
	{
		const auto* talk = sim.conversation(dialogueSpeaker);
		say() << "  conversation " << (talk != nullptr ? talk->dialogue : std::string("none")) << "\n";
	}

	// The KO.
	std::uint64_t koTick = 0;
	for (int i = 0; i < 20 * 180 && koTick == 0; ++i)
	{
		sim.run(1);
		const state::Fighter& me = game.me();
		if (me.unconscious || me.down)
			koTick = sim.clockTick();
	}
	test::require(koTick != 0, "the patrol puts the player down");
	std::cout << "the player is down " << (koTick - attackTick) * kTickSeconds << " s after the attack\n";

	// The capture chain (BW's 167 selector), then the cuff.
	std::uint64_t chainTick = 0;
	std::uint64_t chainMember = 0;
	std::uint64_t cuffTick = 0;
	for (int i = 0; i < 20 * 120 && cuffTick == 0; ++i)
	{
		sim.run(1);
		for (const Person& p : members)
		{
			const state::Routine* r = entityOf(sim, p.id).get<state::Routine>();
			if (r != nullptr && r->goal == routines::task::CaptureSlaves && r->chainTarget == game.player && chainTick == 0)
			{
				chainTick = sim.clockTick();
				chainMember = p.id;
			}
		}
		if (game.me().slaveState == 1 && cuffTick == 0)
			cuffTick = sim.clockTick();
	}
	test::require(chainTick != 0, "a manhunter takes CAPTURE_NEW_SLAVES 167 on the KO'd player (the world selector)");
	test::require(cuffTick != 0, "the chain cuffs the player: slave state 1");
	test::require(game.me().captorSid == std::string("54587-rebirth.mod"), "the captor faction is the Manhunters");
	std::cout << "capture 167 taken by member " << chainMember << " " << (chainTick - koTick) * kTickSeconds << " s after the KO; cuffed "
		<< (cuffTick - chainTick) * kTickSeconds << " s later, slave of '" << game.me().captorSid << "'\n";
	for (const std::string& line : game.taskLog)
		if (line.rfind("capture", 0) == 0)
			std::cout << "  log: " << line << "\n";
}

// Case 2: the same patrol in a non-player town (the Hub): the town condition refuses the hunt.
void hubRefusal(const std::string& bake)
{
	std::cout << "== the manhunter patrol in a non-player town (the Hub)\n";
	Game game(bake);
	game.newGame();
	Simulation& sim = *game.sim;
	sim.run(200);
	const std::uint32_t root = game.book->dialogueIndex(kRoot);
	test::require(root != kNoIndex, "the Manhunters' root 54937 is in the merged dialogue book");
	// The world model's placement of the Hub, and its TOWN record's radius (the dialogue's town test is that circle).
	const state::TownState* hub = townState(sim, kHubTown);
	test::require(hub != nullptr, "the Hub has a TownState in the spawned world");
	const kdata::Record* hubRecord = sim.gameData()->find(kHubTown);
	test::require(hubRecord != nullptr, "the Hub's TOWN record is in the merged data");
	const double radius = hubRecord->floatOr("size radius", 0.0);
	game.teleport(hub->x, hub->z);
	sim.run(2);
	const flecs::entity squad = sim.placeWildernessSquad(kPatrol, hub->x + 6.0, hub->z + 6.0);
	const std::vector<Person> members = builtMembers(sim, squad);
	test::require(members.size() >= 3, "the manhunter patrol is built in the town");
	for (const Person& p : members)
	{
		const state::Fighter* f = sim.combat()->find(p.id);
		test::require(f != nullptr && std::hypot(f->x - hub->x, f->z - hub->z) <= radius,
			"every manhunter stands inside the Hub's radius (the non-player town)");
	}
	std::cout << "patrol " << members.size() << " members inside the Hub (radius " << radius << ")\n";

	// Two I_SEE windows (the fire and the 30 s re-fire), watching for a passed root and for a target on the player.
	bool offered = false;
	bool picked = false;
	std::uint64_t attackTick = 0;
	std::size_t seen = 0;
	for (int i = 0; i < 20 * 45 && attackTick == 0; ++i)
	{
		sim.run(1);
		const std::deque<DialoguePick>& log = sim.dialogue()->log();
		if (log.size() < DialogueSystem::kLogCap)
		{
			for (; seen < log.size(); ++seen)
			{
				const DialoguePick& pick = log[seen];
				if (!isManhunter(members, pick.event.speaker) || (pick.event.event != ev::ISeeNeutralSquad && pick.event.event != ev::ISeeEnemyPlayer))
					continue;
				offered = offered || pick.candidates > 0;
				if (pick.result == DialoguePick::Result::Picked && pick.dialogue == root)
					picked = true;
			}
		}
		for (const Person& p : members)
		{
			const state::Fighter* f = sim.combat()->find(p.id);
			if (f != nullptr && f->target == game.player)
				attackTick = sim.clockTick();
		}
	}
	test::require(offered, "the I_SEE events fire in town too (the hunt's own gate is what refuses it)");
	test::require(!picked, "the town condition refuses root 54937: it is never picked in the Hub");
	test::require(attackTick == 0, "no manhunter targets the player in the Hub");
	std::cout << "root 54937 offered and refused (in a non-player town); no pick, no attack in 45 s\n";
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		std::setvbuf(stdout, nullptr, _IONBF, 0);
		std::cout.setf(std::ios::unitbuf);
		const std::string bake = argc > 1 ? argv[1] : "";
		if (bake.empty() || !std::filesystem::exists(bake))
		{
			std::cerr << "ksim_manhunter_hunt: needs the world bake (argv[1])\n";
			return 77;
		}
		wildHunt(bake);
		hubRefusal(bake);
	}
	catch (const std::exception& e)
	{
		std::fprintf(stderr, "ksim_manhunter_hunt: %s\n", e.what());
		return 1;
	}
	return 0;
}
