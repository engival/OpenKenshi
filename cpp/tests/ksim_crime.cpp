// Crime foundation (crime.hpp, docs/reference/CRIME_STATIC.md, CRIME_LIVE.md, TRESPASS_LIVE.md): the pure rules against the
// measured numbers, then the world step on hand-built characters: an unwitnessed theft clears, a witnessed one commits the
// measured 2500 for the law witness's faction, a non-law witness only fires the dialogue hook, bounty expiry (a 1000 bounty
// lasts exactly 40 h), a save and load in the middle of a record, two player factions keeping separate bounties, the assault
// hook, the wanted-reaction thresholds, the trespass sensor with the house locked, and the witness case-4 betrayal write on an
// allied thief.

#include "ksim/core/clock.hpp"
#include "ksim/law/crime.hpp"
#include "ksim/world/factions.hpp"
#include "ksim/ai/goap.hpp"
#include "ksim/ai/perception.hpp"
#include "ksim/core/simulation.hpp"

#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>
#include <sstream>
#include <string>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;
namespace cr = ksim::crime;

void check(bool condition, const std::string& what)
{
	test::require(condition, what);
	std::printf("ok  %s\n", what.c_str());
}

void rules()
{
	check(cr::bountyAmount(cr::kTheft) == 2500 && cr::bountyAmount(cr::kTrespassing) == 100 && cr::bountyAmount(cr::kEscapingPrison) == 2500
		&& cr::bountyAmount(cr::kAssault) == 500 && cr::bountyAmount(cr::kBurglary) == 1000 && cr::bountyAmount(cr::kKidnapping) == 5000
		&& cr::bountyAmount(cr::kTreason) == 50000 && cr::bountyAmount(cr::kLooting) == 500,
		"bounty amounts: theft 2500, trespass 100, escaping prison 2500, assault 500, burglary 1000, kidnapping 5000, treason 50000");
	check(cr::expiryHours(1000) == 40.0 && cr::expiryHours(100) == 4.0 && cr::expiryHours(2500) == 100.0 && cr::expiryHours(9999) > 399.0
		&& cr::expiryHours(10000) < 0.0, "expiry: amount x 0.04 h, never from 10000");
	check(cr::sentenceHours(2500) == 50.0 && cr::sentenceHours(500) == 10.0 && cr::sentenceHours(25000) == 500.0
		&& cr::sentenceHours(25001) == 999999.0, "sentence: int(amount x 0.02), FOREVER from 25001");
	check(cr::effectiveClass(2, true) == 9 && cr::effectiveClass(2, false) == 2, "a player character is an adventurer");
	check(cr::isLaw(1, false) && cr::isLaw(2, true) && !cr::isLaw(2, false) && !cr::isLaw(4, true), "law: class 1, or military in its own town");
	check(cr::wantedThreshold(1, false) == 1 && cr::wantedThreshold(2, true) == 2000 && cr::wantedThreshold(2, false) == 4000
		&& cr::wantedThreshold(9, false) == 1000 && cr::wantedThreshold(6, false) > 1000000 && cr::wantedThreshold(4, false) == 10000,
		"wanted thresholds: law 1, military 2000 / 4000, adventurer 1000, slave never, else 10000");

	state::CrimeRecord r;
	check(cr::setCrime(r, cr::kTheft, "A", 0) && r.timer == 3.0, "setCrime tops the timer up to 3 s");
	check(!cr::setCrime(r, cr::kBurglary, "A", 0) && r.crime == cr::kTheft && r.timer == 20.0,
		"a different pending crime is kept and the timer bumped to 20 s");
	check(cr::commit(r, "A", 9, 5.0) && cr::bountyOf(r, "A") == 2500 && cr::findBounty(r, "A")->crimes == 8, "commit adds 2500, crimes mask 0x8");
	check(!cr::commit(r, "A", 9, 5.0), "one continuous episode commits once");
	check(!cr::setCrime(r, cr::kBurglary, "A", 0) && cr::setCrime(r, cr::kAssault, "A", 0) == false,
		"an assault does not overwrite a committed theft (the live run adds no second bounty)");
	state::CrimeRecord b;
	cr::setCrime(b, cr::kTheft, "A", 0);
	check(!cr::commit(b, "A", cr::kClassBandit, 0) && !cr::commit(b, "A", cr::kClassLaw, 0) && !cr::commit(b, "A", cr::kClassDiplomat, 0),
		"bandits, diplomats and law criminals get no bounty");
	state::CrimeRecord e;
	cr::setCrime(e, cr::kEnslaving, "A", 0);
	check(!cr::commit(e, "A", 9, 0), "enslaving is never committed");
	// The timer test comes before the decrement: 3 -> 2 -> 1 -> 0, cleared on the fourth tick. The live dt jittered around 1.0
	// (3.0, 2.11, 1.11, 0.10, -0.92, cleared); an exact 1.0 reaches 0 and clears one tick sooner.
	state::CrimeRecord t;
	cr::reportCrime(t, cr::kTheft, "A", 0, 3.0);
	int ticks = 0;
	while (t.crime != cr::kNone && ticks < 20)
	{
		cr::tick(t, 1.0, 0.0);
		++ticks;
	}
	check(ticks == 4, "an unwitnessed 3 s record clears on the tick after the timer reaches zero (" + std::to_string(ticks) + " ticks)");
	state::CrimeRecord x;
	cr::addFlat(x, "A", 1000, 100.0);
	cr::tick(x, 1.0, 139.9);
	check(cr::bountyOf(x, "A") == 1000, "a 1000 bounty is still there at 39.9 h");
	cr::tick(x, 1.0, 140.01);
	check(cr::bountyOf(x, "A") == 0 && cr::empty(x), "a 1000 bounty is gone at 40.01 h");
	state::CrimeRecord n;
	cr::addFlat(n, "A", 10000, 0.0);
	cr::tick(n, 1.0, 1.0e6);
	check(cr::bountyOf(n, "A") == 10000, "a 10000 bounty never expires");
}

struct Data
{
	std::shared_ptr<const kdata::GameData> gd;
	std::shared_ptr<const CombatData> data;
	std::string servant;
	std::string holy; // offers bounties
	std::string cities; // offers bounties
	std::string nameless; // not a law faction
	std::string drifters; // another faction for a second player
	std::string item;

	Data()
	{
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);
		gd = std::make_shared<const kdata::GameData>(kworld::loadMergedGameData(order));
		data = CombatData::build(*gd, KENSHI_DATA);
		for (const kdata::Record* r : gd->byType(kworld::itemTypeByName("CHARACTER")))
		{
			if (r->name == "Escaped Servant")
				servant = r->stringID;
		}
		for (const kdata::Record* r : gd->byType(kworld::itemTypeByName("FACTION")))
		{
			if (r->name == "The Holy Nation")
				holy = r->stringID;
			if (r->name == "United Cities")
				cities = r->stringID;
			if (r->name == "Nameless")
				nameless = r->stringID;
			if (r->name == "Drifters")
				drifters = r->stringID;
		}
		for (const kdata::Record* r : gd->byType(kworld::itemTypeByName("ITEM")))
		{
			item = r->stringID;
			break;
		}
		std::string names;
		for (const kdata::Record* r : gd->byType(kworld::itemTypeByName("FACTION")))
			names += r->name + "|" + (r->boolOr("offers bounties", false) ? "bounties" : "") + "; ";
		test::require(!servant.empty() && !holy.empty() && !cities.empty() && !nameless.empty() && !drifters.empty() && !item.empty(),
			"missing data records (servant '" + servant + "' holy '" + holy + "' cities '" + cities + "' nameless '" + nameless
			+ "' drifters '" + drifters + "' item '" + item + "'); factions: " + names);
		test::require(gd->find(holy)->boolOr("offers bounties", false) && gd->find(cities)->boolOr("offers bounties", false),
			"Holy Nation / United Cities do not offer bounties");
	}
};

struct Rig
{
	const Data& w;
	std::unique_ptr<Simulation> sim;
	std::uint64_t rng = 3;
	std::vector<crime::DialogueEvent> events;
	CombatSettings settings;
	bool fights = false;
	flecs::entity lastBox;

	explicit Rig(const Data& world, std::uint64_t seed = 7)
		: w(world)
	{
		settings.substeps = 2;
		sim = std::make_unique<Simulation>(Simulation::load(KENSHI_DATA, seed));
		sim->setGameData(w.gd);
		sim->setClockTick(tickAt(kNewGameDay, kNewGameHour, kNewGameMinute));
		sim->enableCombat(w.data, settings);
		sim->setDialogueObserver([this](const crime::DialogueEvent& e) { events.push_back(e); });
	}

	// A character at (x, z) facing yaw.
	std::uint64_t add(const std::string& faction, int cls, int team, double x, double z, double yaw, bool player = false)
	{
		state::Fighter f = w.data->makeFighter(*w.gd, w.servant, "", rng);
		f.team = fights ? team : 0; // one team: nobody fights unless a test asks
		f.x = x;
		f.z = z;
		f.yaw = yaw;
		f.npcClass = static_cast<std::uint8_t>(cls);
		f.playerControlled = player;
		f.attention = 0.5;
		flecs::entity e = sim->spawnFighter(std::move(f));
		e.set<state::Allegiance>(state::Allegiance{faction});
		return e.get<state::Id>()->value;
	}
	state::Fighter& f(std::uint64_t id) { return *const_cast<CombatArena*>(sim->combat())->find(id); }
	void tick(int n = 1)
	{
		for (int i = 0; i < n; ++i)
			sim->tick();
	}
	// A container of `owner` at the character's feet holding one item; the character takes it by the transfer command.
	std::uint64_t steal(std::uint64_t who, const std::string& owner, double thievery = 100.0)
	{
		state::Container box;
		box.recordSid = "box";
		box.x = f(who).x;
		box.z = f(who).z;
		box.y = f(who).y;
		box.useX = box.x;
		box.useY = box.y;
		box.useZ = box.z;
		box.ownerSid = owner;
		state::Inventory stock;
		stock.add(w.item, 1);
		flecs::entity c = sim->spawnContainer(box, stock);
		const std::uint64_t id = c.get<state::Id>()->value;
		f(who).skills.thievery = thievery; // 100: a sure take (steal chance 127%): these tests are about the crime, ksim_steal_chance rolls
		sim->enqueue(Command::containerTransfer(state::Id{who}, id, w.item, 1, true, 1));
		return id;
	}
	// A locked container of `owner` at the character's feet; the character picks it by an interact order (action 3, beginInteract).
	std::uint64_t pick(std::uint64_t who, const std::string& owner)
	{
		state::Container box;
		box.recordSid = "box";
		box.x = f(who).x;
		box.z = f(who).z;
		box.y = f(who).y;
		box.useX = box.x;
		box.useY = box.y;
		box.useZ = box.z;
		box.ownerSid = owner;
		box.locked = true;
		box.lockLevel = f(who).skills.lockpicking + 39.0; // a 6% chance (above the 5% floor below which the order never starts): the crime is every tick, before the roll
		lastBox = sim->spawnContainer(box, state::Inventory{});
		const std::uint64_t id = lastBox.get<state::Id>()->value;
		f(who).orderStop = true;
		sim->world().ecs().each([&](flecs::entity e, const state::Id& i, const state::Fighter&)
		{
			if (i.value == who)
				e.set<state::InteractTask>(state::InteractTask{id, 3, 1, 0});
		});
		return id;
	}
	std::uint64_t count(int ev) const { return sim->dialogueEventCount(ev); }
	// The faction instance for a record sid, made if the bare world has none (the relations the crime step writes need one).
	flecs::entity faction(const std::string& sid) { return sim->world().ecs().entity(sim->importFaction(sid).id); }
};

// Seconds of sim time per tick.
std::string str(double v)
{
	return std::to_string(v);
}

// k16.6.329: SIMLINK EVENT 51 caught_stealing: once per caught act (the theft / lockpick record's start), the first witness named; a
// re-witness of the same record sends nothing; an unwitnessed act sends nothing. (The wire filter, player characters only, is the
// Session's; here the sim's observer.)
void caughtStealing(const Data& w)
{
	using Event = Simulation::CrimeEvent;
	// A witnessed theft: one event with the thief, the guard and the container, however long the guard keeps watching.
	{
		Rig rig(w);
		std::vector<Event> sent;
		rig.sim->setCrimeObserver([&](const Event& e) { sent.push_back(e); });
		const std::uint64_t guard = rig.add(w.holy, 1, 1, 0.0, 0.0, 0.0, false);
		const std::uint64_t thief = rig.add(w.nameless, 9, 0, 0.0, 60.0, 3.14159265, true);
		rig.tick(20 * 3);
		const std::uint64_t box = rig.steal(thief, w.holy);
		rig.tick(20 * 12);
		check(sent.size() == 1 && sent[0].kind == Event::Kind::CaughtStealing, "a witnessed theft sends EVENT 51 once (" + std::to_string(sent.size()) + ")");
		check(sent[0].who == thief && sent[0].other == guard && sent[0].cause == 1 && sent[0].container == box, "51 names the thief, the first witness, cause 1 and the container");
	}
	// Unwitnessed: the guard looks away, nothing is sent.
	{
		Rig rig(w);
		std::vector<Event> sent;
		rig.sim->setCrimeObserver([&](const Event& e) { sent.push_back(e); });
		rig.add(w.holy, 1, 1, 0.0, 0.0, 3.14159265, false);
		const std::uint64_t thief = rig.add(w.nameless, 9, 0, 0.0, 60.0, 0.0, true);
		rig.tick(20 * 3);
		rig.steal(thief, w.holy);
		rig.tick(20 * 12);
		check(sent.empty(), "an unwitnessed theft sends no EVENT 51");
	}
	// Acts the steal roll catches (thievery 0: caught on a draw above 30%) are acts too: one 51 per act, a later act after the record
	// ended sends its own (k16.6.424: the caught path never started the entry, so bar_lure's witnessed take sent only alarm 52).
	{
		Rig rig(w);
		std::vector<Event> sent;
		rig.sim->setCrimeObserver([&](const Event& e) { sent.push_back(e); });
		const std::uint64_t guard = rig.add(w.holy, 1, 1, 0.0, 0.0, 0.0, false);
		const std::uint64_t thief = rig.add(w.nameless, 9, 0, 0.0, 60.0, 3.14159265, true);
		rig.tick(20 * 3);
		bool allNamed = true;
		std::size_t before = 0;
		for (int act = 1; act <= 8; ++act)
		{
			const std::uint64_t box = rig.steal(thief, w.holy, 0.0);
			rig.tick(20 * 5);
			rig.sim->forgiveCrime(thief, w.holy); // the record ends (a watching witness keeps it alive otherwise): the act is over
			rig.tick(20);
			// A failed roll sends cause 0 for the thief at once (witness 0); the witness's own sighting still sends cause 1, naming the
			// witness and the container, once. A landed take sends only the witness's.
			const std::size_t n = sent.size() - before;
			allNamed = allNamed && (n == 1 || n == 2) && sent.back().who == thief && sent.back().cause == 1 && sent.back().other == guard
				&& sent.back().container == box && (n == 1 || (sent[before].cause == 0 && sent[before].other == 0 && sent[before].container == 0));
			before = sent.size();
		}
		check(allNamed, "each of 8 takes at thievery 0: the witness's one EVENT 51 names it and the container, a failed roll adds cause 0 first (" + std::to_string(sent.size()) + ")");
	}
	// A lockpick seen: cause 2, the picked container, once although the crime is reported every tick.
	{
		Rig rig(w);
		std::vector<Event> sent;
		rig.sim->setCrimeObserver([&](const Event& e) { sent.push_back(e); });
		const std::uint64_t guard = rig.add(w.holy, 1, 1, 0.0, 0.0, 0.0, false);
		const std::uint64_t thief = rig.add(w.nameless, 9, 0, 0.0, 60.0, 3.14159265, true);
		rig.tick(20 * 3);
		const std::uint64_t box = rig.pick(thief, w.holy);
		rig.tick(20 * 10);
		check(sent.size() == 1 && sent[0].who == thief && sent[0].other == guard && sent[0].cause == 2 && sent[0].container == box,
			"a seen lockpick sends EVENT 51 once with cause 2 (" + std::to_string(sent.size()) + ")");
	}
	// A save and load mid-act. The save holds no "a witness saw it" bit; a committed record (a witness reacted, the bounty went in) is
	// derived as announced, any other loaded act is pending: the first witness after the load names it. (watched: seen before the save.)
	for (const bool watched : {false, true})
	{
		Rig rig(w);
		std::vector<Event> sent;
		rig.sim->setCrimeObserver([&](const Event& e) { sent.push_back(e); });
		const std::uint64_t guard = rig.add(w.holy, 1, 1, 0.0, 0.0, watched ? 0.0 : 3.14159265, false);
		const std::uint64_t thief = rig.add(w.nameless, 9, 0, 0.0, 60.0, 3.14159265, true);
		rig.tick(20 * 3);
		rig.steal(thief, w.holy, 100.0); // thievery 100: the take lands, no roll event
		rig.tick(watched ? 20 * 5 : 1);
		const state::CrimeRecord* held = rig.sim->crimeRecord(thief);
		check(held != nullptr && held->crime == cr::kTheft && held->committed == watched, std::string("a record is pending at the save, committed ") + (watched ? "yes" : "no"));
		const std::size_t seen = sent.size();
		std::ostringstream out;
		rig.sim->save(out);
		std::istringstream in(out.str());
		Simulation restored = Simulation::restore(World::empty(), in, RegionConfig{}, true);
		restored.setGameData(w.gd);
		restored.enableCombat(w.data, rig.settings);
		std::vector<Event> resent;
		restored.setCrimeObserver([&](const Event& e) { resent.push_back(e); });
		const_cast<state::Fighter*>(restored.combat()->find(guard))->yaw = 0.0; // now it sees the thief
		restored.run(20 * 12);
		check(watched ? seen == 1 && resent.empty() : seen == 0 && resent.size() == 1 && resent[0].who == thief && resent[0].other == guard && resent[0].cause == 1,
			std::string(watched ? "an act announced before a save is not announced again" : "an unannounced act is announced once by its first witness after the load")
			+ " (" + std::to_string(seen) + " before, " + std::to_string(resent.size()) + " after)");
	}
}

void theft(const Data& w)
{
	// Unwitnessed: the only guard looks away, and 30 s later nothing was recorded.
	{
		Rig rig(w);
		const std::uint64_t guard = rig.add(w.holy, 1, 1, 0.0, 0.0, 3.14159265, false);
		const std::uint64_t thief = rig.add(w.nameless, 9, 0, 0.0, 60.0, 0.0, true);
		rig.tick(20 * 3); // sensors settle
		rig.steal(thief, w.holy);
		rig.tick(1);
		const state::CrimeRecord* r = rig.sim->crimeRecord(thief);
		check(r != nullptr && r->crime == cr::kTheft && r->timer == 3.0 && r->victimFaction == w.holy,
			"a theft from a foreign container records crime 3 with the 3 s timer");
		rig.tick(20 * 8);
		r = rig.sim->crimeRecord(thief);
		check(r == nullptr || (r->crime == cr::kNone && r->bounties.empty()), "an unwitnessed theft clears and leaves no bounty");
		check(rig.sim->bountyOn(thief, w.holy) == 0 && rig.count(cr::kEvWitnessLootingAlly) == 0, "no witness event for the unwitnessed theft");
		(void)guard;
	}
	// Witnessed by law: commits 2500 for the guard's faction.
	{
		Rig rig(w);
		const std::uint64_t guard = rig.add(w.holy, 1, 1, 0.0, 0.0, 0.0, false);
		const std::uint64_t thief = rig.add(w.nameless, 9, 0, 0.0, 60.0, 3.14159265, true);
		rig.tick(20 * 3);
		const std::uint64_t t0 = rig.sim->clockTick();
		rig.steal(thief, w.holy);
		std::uint64_t committed = 0;
		for (int i = 0; i < 20 * 12 && committed == 0; ++i)
		{
			rig.tick();
			if (rig.sim->bountyOn(thief, w.holy) != 0)
				committed = rig.sim->clockTick();
		}
		check(committed != 0, "a law witness commits the theft");
		const state::CrimeRecord* r = rig.sim->crimeRecord(thief);
		const state::Bounty* b = r != nullptr ? cr::findBounty(*r, w.holy) : nullptr;
		check(b != nullptr && b->amount == 2500 && b->crimes == (1u << cr::kTheft), "bounty HN 2500, crimes mask 0x8");
		const double seconds = static_cast<double>(committed - t0) * kTickSeconds;
		std::printf("    commit %s s (%s game min) after the theft\n", str(seconds).c_str(), str(seconds / kGameDaySeconds * 1440.0).c_str());
		check(seconds <= 5.0, "committed inside the record's 3 s window plus a tick (" + str(seconds) + " s)");
		check(b != nullptr && std::fabs(b->stamp - gameHoursIn(committed)) < 0.02, "the stamp is the world time in hours");
		check(r->timer > 3.0, "the witness refreshed the timer (" + str(r->timer) + " s)");
		check(rig.count(cr::kEvWitnessLootingAlly) > 0 && rig.events.front().speaker == guard && rig.events.front().subject == thief,
			"EV_WITNESS_LOOTING_ALLY fired by the guard at the thief");
		check(rig.sim->bountyOn(thief, w.cities) == 0, "another faction holds no bounty");
		// Repeated witnessing of the same episode adds no second bounty.
		rig.tick(20 * 6);
		check(rig.sim->bountyOn(thief, w.holy) == 2500, "one episode, one bounty add");
	}
	// A non-law witness only fires the hook.
	{
		Rig rig(w);
		const std::uint64_t citizen = rig.add(w.holy, 4, 1, 0.0, 0.0, 0.0, false);
		const std::uint64_t thief = rig.add(w.nameless, 9, 0, 0.0, 60.0, 3.14159265, true);
		rig.tick(20 * 3);
		rig.steal(thief, w.holy);
		rig.tick(20 * 3);
		const state::CrimeRecord* r = rig.sim->crimeRecord(thief);
		check(rig.count(cr::kEvWitnessLootingAlly) > 0, "a civilian witness fires the dialogue event");
		check(r != nullptr && r->crime == cr::kTheft && r->bounties.empty(), "but assigns no bounty on its own");
		check(r->timer > 3.0, "and refreshes the timer");
		// The witness force-notices the criminal (140854d10): only perception::notice writes sinceNoticed (the arrow's record, kenshi-h2v.27.6).
		const state::SightRecord* told = perception::find(*rig.sim->combat()->find(citizen), thief);
		check(told != nullptr && told->state == 1 && told->sinceNoticed < 10.0, "the witness force-noticed the thief");
	}
	// Military outside its own town does not assign either (no town in this rig); a friend of the thief does not witness.
	{
		Rig rig(w);
		rig.add(w.holy, 2, 1, 0.0, 0.0, 0.0, false);
		const std::uint64_t thief = rig.add(w.nameless, 9, 0, 0.0, 60.0, 3.14159265, true);
		rig.tick(20 * 3);
		rig.steal(thief, w.holy);
		rig.tick(20 * 3);
		check(rig.sim->bountyOn(thief, w.holy) == 0 && rig.count(cr::kEvWitnessLootingAlly) > 0, "military outside its own town only fires the event");
	}
	{
		Rig rig(w);
		rig.add(w.nameless, 1, 1, 0.0, 0.0, 0.0, false); // the thief's own faction
		const std::uint64_t thief = rig.add(w.nameless, 9, 0, 0.0, 60.0, 3.14159265, true);
		rig.tick(20 * 3);
		rig.steal(thief, w.holy);
		rig.tick(20 * 3);
		check(rig.sim->bountyOn(thief, w.holy) == 0 && rig.events.empty(), "an observer of the thief's own faction does not witness");
	}
}

// k16.6.268 (rwh.148 row 2, rwh138 "Betrayal case"): a theft witnessed by a faction that is both friendly to the victim's and an
// ally of the thief's takes -20 off the witness faction's relation toward the thief's per witness firing (140854d10 case 4,
// DAT_1416b1860). The next firing reads the written relation back against the ally line (>= 50), so 70 -> 50 -> 30 takes two
// firings while 60 -> 40 stops after one. The drop lands inside the theft's first second, before the guard's first swing.
void betrayal(const Data& w)
{
	for (const int start : {60, 70}) // the live run's two bonded cases
	{
		Rig rig(w, 1); // one team: nobody swings, so the write is the only relation change
		const flecs::entity holy = rig.faction(w.holy);
		const flecs::entity thiefSide = rig.faction(w.nameless);
		holy.set<state::Relation>(thiefSide, state::Relation{static_cast<double>(start)});
		rig.add(w.holy, 1, 1, 0.0, 0.0, 0.0, false);
		const std::uint64_t thief = rig.add(w.nameless, 9, 0, 0.0, 60.0, 3.14159265, true);
		rig.tick(20 * 3);
		rig.steal(thief, w.holy);
		rig.tick(20 * 3); // three witness firings: the second one sees 50 as still an ally at 70, and 40 is not at 60
		const double relation = factionRelation(*w.gd, holy, thiefSide);
		const double expected = start - 20.0 * (start - 20 >= 50 ? 2 : 1);
		check(relation == expected, "a witnessed theft by an ally writes -20 per witness firing (" + str(start) + " -> "
			+ str(relation) + ")");
	}
	// The drop against the guard's first swing: a fight started with the theft, the guard ordered at the thief. The attack order
	// stands in for the per-character crime tag the guards attack through (rwh138 3, not ported yet); the drop itself is the
	// witness step's.
	{
		Rig rig(w, 1);
		rig.fights = true;
		const flecs::entity holy = rig.faction(w.holy);
		const flecs::entity thiefSide = rig.faction(w.nameless);
		holy.set<state::Relation>(thiefSide, state::Relation{60.0});
		const std::uint64_t guard = rig.add(w.holy, 1, 1, 0.0, 0.0, 0.0, false);
		const std::uint64_t thief = rig.add(w.nameless, 9, 0, 0.0, 100.0, 3.14159265, true);
		rig.tick(20 * 3);
		std::uint64_t swing = 0;
		rig.sim->setCombatObserver([&](const CombatEvent& e)
		{
			if (swing == 0 && e.kind == CombatEvent::Kind::TechniqueStart && e.who == guard)
				swing = rig.sim->clockTick();
		});
		rig.sim->enqueue(Command::fighterAttack(state::Id{guard}, state::Id{thief}));
		rig.steal(thief, w.holy);
		std::uint64_t firstDrop = 0;
		for (int i = 0; i < 20 * 30 && (firstDrop == 0 || swing == 0); ++i)
		{
			rig.tick();
			if (firstDrop == 0 && factionRelation(*w.gd, holy, thiefSide) < 60.0)
				firstDrop = rig.sim->clockTick();
		}
		check(firstDrop != 0 && swing != 0 && firstDrop < swing, "the relation drops before the guard's first swing (drop at tick "
			+ std::to_string(firstDrop) + ", swing at " + std::to_string(swing) + ")");
	}
	// The gates: below the ally line no firing, and the crimes with their own witness branch (here burglary) never reach the block.
	{
		Rig rig(w, 1);
		const flecs::entity holy = rig.faction(w.holy);
		const flecs::entity thiefSide = rig.faction(w.nameless);
		holy.set<state::Relation>(thiefSide, state::Relation{40.0});
		rig.add(w.holy, 1, 1, 0.0, 0.0, 0.0, false);
		const std::uint64_t thief = rig.add(w.nameless, 9, 0, 0.0, 60.0, 3.14159265, true);
		rig.tick(20 * 3);
		rig.steal(thief, w.holy);
		rig.tick(20 * 3);
		check(factionRelation(*w.gd, holy, thiefSide) == 40.0, "at 40 (below the ally line) the witness writes nothing");
	}
	{
		Rig rig(w, 1);
		const flecs::entity holy = rig.faction(w.holy);
		const flecs::entity thiefSide = rig.faction(w.nameless);
		holy.set<state::Relation>(thiefSide, state::Relation{60.0});
		rig.add(w.holy, 1, 1, 0.0, 0.0, 0.0, false);
		const std::uint64_t thief = rig.add(w.nameless, 9, 0, 0.0, 60.0, 3.14159265, true);
		rig.tick(20 * 3);
		rig.pick(thief, w.holy); // burglary: its own branch, not the case-4 block
		rig.tick(20 * 3);
		check(rig.count(cr::kEvWitnessThiefOrLockpick) > 0 && factionRelation(*w.gd, holy, thiefSide) == 60.0,
			"a witnessed burglary by an ally takes no betrayal step");
	}
}

// Picking a foreign container's lock (dt24_picklock section 7.2): crime 2 every tick, so the owner's faction witnesses it.
void lockpick(const Data& w)
{
	// Witnessed: crime 2 on the picker, the witness event fired by the owner-faction observer.
	{
		Rig rig(w);
		const std::uint64_t owner = rig.add(w.holy, 4, 1, 0.0, 0.0, 0.0, false);
		const std::uint64_t thief = rig.add(w.nameless, 9, 0, 0.0, 60.0, 3.14159265, true);
		rig.tick(20 * 3);
		rig.pick(thief, w.holy);
		rig.tick(20 * 3);
		const state::CrimeRecord* r = rig.sim->crimeRecord(thief);
		check(r != nullptr && r->crime == cr::kBurglary && r->victimFaction == w.holy, "picking a foreign container records crime 2");
		check(r != nullptr && r->timer > 3.0, "the witness refreshed the timer");
		check(rig.count(cr::kEvWitnessThiefOrLockpick) > 0 && rig.events.front().speaker == owner && rig.events.front().subject == thief,
			"EV_WITNESS_THIEF_OR_LOCKPICK fired by the owner-faction observer at the picker");
	}
	// Nobody in view: the record expires unwitnessed.
	{
		Rig rig(w);
		rig.add(w.holy, 4, 1, 0.0, 0.0, 3.14159265, false);
		const std::uint64_t thief = rig.add(w.nameless, 9, 0, 0.0, 60.0, 0.0, true);
		rig.tick(20 * 3);
		rig.pick(thief, w.holy);
		rig.tick(20);
		const state::CrimeRecord* r = rig.sim->crimeRecord(thief);
		check(r != nullptr && r->crime == cr::kBurglary, "an unwatched pick still records crime 2");
		rig.lastBox.get_mut<state::Container>()->locked = false; // the picker's order ends: the pending record runs out
		rig.tick(20 * 8);
		r = rig.sim->crimeRecord(thief);
		check(r == nullptr || r->crime == cr::kNone, "the unwitnessed record expires");
		check(rig.count(cr::kEvWitnessThiefOrLockpick) == 0, "no witness event without an observer in view");
	}
	// A box of the picker's own faction: nothing recorded.
	{
		Rig rig(w);
		rig.add(w.nameless, 4, 1, 0.0, 0.0, 0.0, false);
		const std::uint64_t thief = rig.add(w.nameless, 9, 0, 0.0, 60.0, 3.14159265, true);
		rig.tick(20 * 3);
		rig.pick(thief, w.nameless);
		rig.tick(20 * 3);
		const state::CrimeRecord* r = rig.sim->crimeRecord(thief);
		check(r == nullptr || r->crime == cr::kNone, "picking the picker's own faction's box records nothing");
	}
}

// Looting a body (MINE/UNSURE rule, crime.cpp witnesses): a witness ignores the theft when the body is on another combat team
// (a character it was fighting), and still witnesses it when the body is on its own team. Body and guard share a faction, so only the
// team decides (the faction-hostility rule plays no part). Client report 2026-11-05: Hub guards KO'd the player for looting a Dust
// Bandit they had just beaten.
void lootBody(const Data& w)
{
	for (const bool enemyBody : { true, false })
	{
		Rig rig(w);
		const std::uint64_t guard = rig.add(w.holy, 1, 1, 0.0, 0.0, 0.0, false);
		const std::uint64_t thief = rig.add(w.nameless, 9, 0, 0.0, 60.0, 3.14159265, true);
		state::Fighter down = w.data->makeFighter(*w.gd, w.servant, "", rig.rng);
		down.team = enemyBody ? 7 : rig.f(guard).team;
		down.x = 0.0;
		down.z = 61.0;
		down.npcClass = 4;
		down.dead = true; // a KO'd fighter with no injury wakes at once; the rule reads only the team
		flecs::entity bodyEntity = rig.sim->spawnFighter(std::move(down));
		bodyEntity.set<state::Allegiance>(state::Allegiance{w.holy});
		state::Inventory pack;
		pack.add(w.item, 1);
		bodyEntity.set<state::Inventory>(std::move(pack));
		const std::uint64_t body = bodyEntity.get<state::Id>()->value;
		rig.tick(20 * 3);
		rig.sim->enqueue(Command::containerTransfer(state::Id{thief}, body, w.item, 1, true, 1));
		const state::CrimeRecord* r = nullptr;
		for (int i = 0; i < 20 * 3 && (r == nullptr || r->crime != cr::kLooting); ++i)
		{
			rig.tick();
			r = rig.sim->crimeRecord(thief);
		}
		check(r != nullptr && r->crime == cr::kLooting && r->victimChar == body, "looting a dead body records looting (10) with the body as the victim");
		rig.tick(20 * 6);
		const bool witnessed = rig.count(cr::kEvWitnessLootingAlly) > 0 || rig.sim->bountyOn(thief, w.holy) != 0;
		check(witnessed != enemyBody, enemyBody ? "a guard ignores the looting of a body it was fighting"
			: "a guard witnesses the looting of a body on its own team");
	}
}

// The carry inspection (task ds/kidnap, crime.cpp 2b; 1408549f0): a witness who sees a carrier hauling a captive writes kidnapping on it
// (EV_KIDNAPPING_MY_ALLY for a friend of the victim), unless the carried has a bounty with the witness's legal faction or is no captive;
// a prisoner held by the witness's faction is slave freeing whoever carries it; law does not write the stranger case.
void carryInspection(const Data& w)
{
	struct Case
	{
		const char* name;
		int witnessClass;
		bool captive;
		bool bounty;
		bool held;
		std::uint8_t expected;
	};
	const Case cases[] = {
		{"a civilian friend of the carried sees a captive carried", 4, true, false, false, cr::kKidnapping},
		{"a carried non-captive is not a kidnapping", 4, false, false, false, cr::kNone},
		{"a carried character with a bounty for the witness's faction is exempt", 4, true, true, false, cr::kNone},
		{"law does not write the stranger case", 1, true, false, false, cr::kNone},
		{"a prisoner held by the witness's faction is slave freeing for law too", 1, false, false, true, cr::kSlaveFreeing},
	};
	for (const Case& c : cases)
	{
		Rig rig(w);
		const std::uint64_t witness = rig.add(w.cities, c.witnessClass, 1, 0.0, 0.0, 0.0, false);
		const std::uint64_t carrier = rig.add(w.nameless, 9, 0, 0.0, 60.0, 3.14159265, true);
		const std::uint64_t carried = rig.add(c.held ? w.nameless : w.cities, 4, 0, 0.0, 60.0, 0.0, false);
		state::Fighter& body = rig.f(carried);
		body.unconscious = true;
		body.koTimer = 600.0;
		body.carriedBy = carrier;
		body.captive = c.captive;
		if (c.held)
			body.captorSid = w.cities;
		rig.f(carrier).carrying = carried;
		if (c.bounty)
			rig.sim->addBounty(carried, w.cities, 1000);
		const state::CrimeRecord* r = nullptr;
		for (int i = 0; i < 20 * 4 && (r == nullptr || r->crime != c.expected || c.expected == cr::kNone); ++i)
		{
			rig.tick();
			r = rig.sim->crimeRecord(carrier);
		}
		if (c.expected == cr::kNone)
		{
			check(r == nullptr || r->crime == cr::kNone, c.name);
			continue;
		}
		check(r != nullptr && r->crime == c.expected && r->victimChar == carried, c.name);
		rig.tick(20 * 2);
		if (c.expected == cr::kKidnapping)
			check(rig.count(0x3a) > 0 && rig.events.front().speaker == witness && rig.events.front().subject == carrier,
				"the friend of the victim fires EV_KIDNAPPING_MY_ALLY about the carrier");
	}
}

// rwh.162 (k16.6.325): law enforcement is not kidnapping. Guards (a bar's own staff, not law class) lift a KO'd thief (the 240 arrest lift of the
// 223 -> 240 -> 224 chain) and carry him out: the lift sets no kidnap flag (the exe writes CharStatus+0xe9 only in Task_Kidnapping, order 246), so a friend
// of the thief who watches the carry-out writes no crime on the guard, the put-down writes none, and the thief is not tagged Kidnapped.
void lawEnforcementCarry(const Data& w)
{
	for (const std::uint16_t lift : {goap::kLiftPersonSnatchingInTown, goap::kKidnapOrder})
	{
		const bool kidnap = lift == goap::kKidnapOrder;
		Rig rig(w);
		const std::uint64_t guard = rig.add(w.cities, 2, 0, 0.0, 0.0, 0.0, false);
		const std::uint64_t thief = rig.add(w.nameless, 9, 0, 0.0, 6.0, 0.0, true);
		const std::uint64_t friendOfThief = rig.add(w.nameless, 4, 0, 0.0, 40.0, 3.14159265, false);
		const auto knockOut = [&] { rig.f(thief).unconscious = true; rig.f(thief).koTimer = 600.0; };
		if (!kidnap)
			knockOut();
		rig.tick(20 * 3);
		check(rig.sim->orderArrest(guard, thief, lift), "the lift order is accepted");
		if (kidnap)
		{
			rig.tick(20 * 2); // order 246's start fails for a target already down: the victim is downed after it started
			knockOut();
		}
		bool carried = false;
		bool flagged = false;
		for (int i = 0; i < 20 * 20; ++i)
		{
			rig.tick();
			carried = carried || rig.f(thief).carriedBy == guard;
			flagged = flagged || rig.f(thief).captive;
		}
		check(carried, "the guard lifted the thief");
		check(flagged == kidnap, kidnap ? "the kidnapping task's lift sets the kidnap flag" : "a guard's lift of a criminal sets no kidnap flag");
		const state::CrimeRecord* record = rig.sim->crimeRecord(guard);
		check(kidnap || record == nullptr || record->crime == cr::kNone, "the carrying guard has no crime (no Committing Crime!)");
		check(kidnap || !rig.f(thief).captive, "the thief is not tagged Kidnapped (no captive flag while carried)");
		check(rig.sim->crimeRecord(friendOfThief) == nullptr, "the thief's ally, standing by, carries no crime");
	}
}

// kenshi-k16.6.669: the original's rule as is. A player's plain lift (order 225, not 246; the rig has no navigation, so it goes through orderArrest) of a stranger or of a prisoner sets no captive flag and
// records no crime 15 on the carrier. (The parked fix that would flag it is kenshi-w2k.5.)
void plainPlayerLift(const Data& w)
{
	for (const bool prisoner : {false, true})
	{
		Rig rig(w);
		const std::uint64_t player = rig.add(w.nameless, 9, 0, 0.0, 6.0, 0.0, true);
		const std::uint64_t victim = rig.add(prisoner ? w.nameless : w.cities, 4, 0, 0.0, 0.0, 0.0, false);
		state::Fighter& body = rig.f(victim);
		body.unconscious = true;
		body.koTimer = 600.0;
		if (prisoner)
		{
			body.slaveState = 1;
			body.captorSid = w.cities;
		}
		rig.tick(20 * 3);
		check(rig.sim->orderArrest(player, victim, goap::kLiftPersonPlayerOrder), "the player's lift order (225) is accepted");
		bool carried = false;
		bool flagged = false;
		for (int i = 0; i < 20 * 20; ++i)
		{
			rig.tick();
			carried = carried || rig.f(victim).carriedBy == player;
			flagged = flagged || rig.f(victim).captive;
		}
		const char* who = prisoner ? "a prisoner" : "a stranger";
		check(carried, std::string("the player lifted ") + who);
		check(!flagged, std::string("a plain lift of ") + who + " sets no captive flag");
		const state::CrimeRecord* record = rig.sim->crimeRecord(player);
		check(record == nullptr || record->crime != cr::kKidnapping, std::string("a plain lift of ") + who + " records no crime 15");
	}
}

void expiry(const Data& w)
{
	Rig rig(w);
	const std::uint64_t thief = rig.add(w.nameless, 9, 0, 0.0, 0.0, 0.0, true);
	rig.sim->addBounty(thief, w.holy, 1000);
	rig.sim->addBounty(thief, w.cities, 10000);
	const std::uint64_t perHour = kTicksPerGameHour;
	const std::uint64_t start = rig.sim->clockTick();
	rig.sim->setClockTick(start + 39 * perHour + perHour * 9 / 10 - 1); // 39.9 h
	rig.tick(20 * 3);
	check(rig.sim->bountyOn(thief, w.holy) == 1000, "a 1000 bounty survives to 39.9 h through the sim");
	rig.sim->setClockTick(start + 40 * perHour + perHour / 100);
	rig.tick(20 * 3);
	check(rig.sim->bountyOn(thief, w.holy) == 0, "and is gone at 40 h");
	check(rig.sim->bountyOn(thief, w.cities) == 10000, "a 10000 bounty is permanent");
	check(rig.sim->clearBounty(thief, w.cities) && rig.sim->crimeRecord(thief) == nullptr, "clearing the last bounty drops the record");
}

void saveLoad(const Data& w)
{
	const auto build = [&](Rig& rig) -> std::uint64_t
	{
		rig.add(w.holy, 1, 1, 0.0, 0.0, 0.0, false);
		const std::uint64_t thief = rig.add(w.nameless, 9, 0, 0.0, 60.0, 3.14159265, true);
		rig.tick(20 * 3);
		rig.steal(thief, w.holy);
		return thief;
	};
	std::uint64_t straightHash = 0;
	std::uint64_t splitHash = 0;
	std::int32_t straightBounty = 0;
	std::int32_t splitBounty = 0;
	{
		Rig rig(w);
		const std::uint64_t thief = build(rig);
		rig.tick(20 * 30);
		straightHash = rig.sim->stateHash();
		straightBounty = rig.sim->bountyOn(thief, w.holy);
	}
	{
		Rig rig(w);
		const std::uint64_t thief = build(rig);
		rig.tick(20 * 2);
		const state::CrimeRecord* r = rig.sim->crimeRecord(thief);
		check(r != nullptr && r->crime == cr::kTheft, "a record is pending at the save");
		std::ostringstream out;
		rig.sim->save(out);
		std::istringstream in(out.str());
		Simulation restored = Simulation::restore(World::empty(), in, RegionConfig{}, true);
		restored.setGameData(w.gd);
		restored.enableCombat(w.data, rig.settings);
		const state::CrimeRecord* loaded = restored.crimeRecord(thief);
		check(loaded != nullptr && loaded->crime == r->crime && loaded->timer == r->timer && loaded->bounties.size() == r->bounties.size(),
			"the record survives the save");
		restored.run(20 * 28);
		splitHash = restored.stateHash();
		splitBounty = restored.bountyOn(thief, w.holy);
	}
	check(straightBounty == 2500 || straightBounty == 0, "the straight run ended with a consistent bounty");
	check(splitBounty == straightBounty, "the same bounty after a save and load mid-record");
	check(splitHash == straightHash, "the state hash after a save and load mid-record equals the straight run");
}

void twoPlayers(const Data& w)
{
	Rig rig(w);
	rig.add(w.holy, 1, 1, 0.0, 0.0, 0.0, false);
	rig.add(w.cities, 1, 1, 6000.0, 0.0, 0.0, false); // far apart: a law witness commits for its own faction whatever the victim
	const std::uint64_t a = rig.add(w.nameless, 9, 0, 0.0, 60.0, 3.14159265, true);
	const std::uint64_t b = rig.add(w.drifters, 9, 2, 6000.0, 60.0, 3.14159265, true);
	rig.tick(20 * 3);
	rig.steal(a, w.holy);
	rig.tick(20 * 6);
	rig.steal(b, w.cities);
	rig.tick(20 * 6);
	check(rig.sim->bountyOn(a, w.holy) == 2500 && rig.sim->bountyOn(b, w.cities) == 2500, "each player faction's thief carries its own bounty");
	check(rig.sim->bountyOn(a, w.cities) == 0 && rig.sim->bountyOn(b, w.holy) == 0, "and not the other's");
	rig.sim->addBounty(a, w.cities, 500);
	check(rig.sim->bountyOn(a, w.holy) == 2500 && rig.sim->bountyOn(a, w.cities) == 500, "one character holds bounties per faction");
	rig.sim->clearBounty(a, w.holy);
	check(rig.sim->bountyOn(a, w.holy) == 0 && rig.sim->bountyOn(b, w.cities) == 2500, "clearing one player's bounty leaves the other's");
}

void assault(const Data& w)
{
	// Seed 1, not the default 7: the victim and the attacker are enemies from the first tick (rig.fights), so who swings first is a roll. When the
	// victim swings first it is the one with the pending assault, and an attack on a victim with a pending crime records nothing (stepCrime).
	// With the crowd spacing work (melee avoidance, the 7.07 body floor) seed 7 happens to give the victim the first swing; seed 1 does not.
	Rig rig(w, 1);
	rig.fights = true;
	rig.add(w.holy, 1, 1, 0.0, 0.0, 0.0, false); // the guard
	const std::uint64_t victim = rig.add(w.holy, 4, 1, 0.0, 60.0, 3.14159265, false);
	// 8 u from the victim: characters in combat are depenetrated to the footprints' sum (7.07 scaled, combat.cpp kDepenetrateScale), the old 2 u was inside that.
	const std::uint64_t attacker = rig.add(w.nameless, 9, 0, 0.0, 68.0, 0.0, true);
	rig.tick(20 * 3);
	rig.sim->enqueue(Command::fighterAttack(state::Id{attacker}, state::Id{victim}));
	std::uint64_t pending = 0;
	for (int i = 0; i < 20 * 15 && pending == 0; ++i)
	{
		rig.tick();
		const state::CrimeRecord* r = rig.sim->crimeRecord(attacker);
		if (r != nullptr && r->crime == cr::kAssault)
			pending = rig.sim->clockTick();
	}
	check(pending != 0, "an attack records assault (crime 5) against the victim's faction");
	rig.tick(20 * 6);
	const state::CrimeRecord* r = rig.sim->crimeRecord(attacker);
	check(r != nullptr && cr::bountyOf(*r, w.holy) >= 500 && (cr::findBounty(*r, w.holy)->crimes & (1u << cr::kAssault)) != 0,
		"a law witness commits the assault (500)");
	check(rig.count(cr::kEvWitnessGenericAssault) > 0, "EV_WITNESS_GENERIC_ASSAULT fired");
}

// k16.6.269 (rwh138 4.2, rwh139 3): the hit path's law branch. A player character answering a guard (never ordered: it fights back
// under 32 / 15) is charged with Assault + a 500 bounty with the guard's faction on its first landed hit; a bystander that lands
// nothing stays crime 0; the guard (law) is never the criminal. (The relation: no code on this path writes one.)
void joinerAssault(const Data& w)
{
	Rig rig(w, 1);
	rig.fights = true;
	const std::uint64_t guard = rig.add(w.holy, cr::kClassLaw, 1, 0.0, 0.0, 0.0, false);
	const std::uint64_t joiner = rig.add(w.nameless, 9, 0, 0.0, 8.0, 3.14159265, true);
	const std::uint64_t bystander = rig.add(w.nameless, 9, 0, 3000.0, 3000.0, 0.0, true);
	rig.tick(20 * 3);
	const double relationBefore = rig.sim->factionRelationBetween(w.holy, w.nameless);
	rig.sim->enqueue(Command::fighterAttack(state::Id{joiner}, state::Id{guard}));
	const state::CrimeRecord* r = nullptr;
	for (int i = 0; i < 20 * 20 && (r == nullptr || cr::bountyOf(*r, w.holy) < 500); ++i)
	{
		rig.tick();
		r = rig.sim->crimeRecord(joiner);
	}
	check(r != nullptr && r->crime == cr::kAssault && r->victimChar == guard, "a landed hit on a guard is the attacker's own Assault");
	check(r != nullptr && cr::bountyOf(*r, w.holy) >= 500, "with a 500 bounty with the guard's faction");
	check(rig.sim->crimeRecord(guard) == nullptr, "the law character carries no crime");
	// The charged hit is the guard's own on the joiner (law attacker, non-law defender: resisting law); the DEFENDER's store gets the tag.
	const std::vector<std::uint64_t>& tagAfter = rig.f(joiner).hostileSquads;
	check(std::binary_search(tagAfter.begin(), tagAfter.end(), rig.f(guard).squad), "tag 4 (temp enemy): the defender's squad lists the hitter");
	check(rig.sim->factionRelationBetween(w.holy, w.nameless) == relationBefore, "the faction relation did not move on the law-branch hit");
	check(rig.sim->crimeRecord(bystander) == nullptr, "a character that landed nothing carries crime 0");
}

// k16.6.362: the law branch alone. The guard (law) attacks a player character: the generic attack-record path charges the ATTACKER, and a
// law character cannot be a criminal, so only the law branch (k16.6.269) can charge the defender. No other law character is present to
// witness anything (same team: the targeting flips the defender's squad to hostile, tag 4), and the relation is never written on this path. Tag 4 (the arena's targeting hostility) is on the defender's squad.
void lawHitCharges(const Data& w)
{
	Rig rig(w, 1);
	rig.fights = true;
	const flecs::entity holy = rig.faction(w.holy);
	const flecs::entity nameless = rig.faction(w.nameless);
	const double before = factionRelation(*w.gd, holy, nameless);
	const std::uint64_t guard = rig.add(w.holy, cr::kClassLaw, 1, 0.0, 0.0, 0.0, false);
	const std::uint64_t defender = rig.add(w.nameless, 9, 1, 0.0, 8.0, 3.14159265, true);
	rig.tick(20 * 3);
	rig.sim->enqueue(Command::fighterAttack(state::Id{guard}, state::Id{defender}));
	const state::CrimeRecord* r = nullptr;
	for (int i = 0; i < 20 * 20 && (r == nullptr || cr::bountyOf(*r, w.holy) < 500); ++i)
	{
		rig.tick();
		r = rig.sim->crimeRecord(defender);
	}
	check(r != nullptr && r->crime == cr::kAssault && r->victimChar == guard, "a guard's landed hit charges the defender with Assault");
	check(r != nullptr && cr::bountyOf(*r, w.holy) >= 500, "with a 500 bounty with the guard's faction");
	check(rig.sim->crimeRecord(guard) == nullptr, "the guard carries no crime");
	const std::vector<std::uint64_t>& tags = rig.f(defender).hostileSquads;
	check(std::find(tags.begin(), tags.end(), rig.f(guard).squad) != tags.end(), "tag 4: the defender's squad lists the guard's squad hostile");
	check(factionRelation(*w.gd, holy, nameless) == before, "the faction relation is unchanged");
}

// The cuff's crime (dt5, Task_CuffTarget vf2 14034af20): a slaver's restrain of a free, unbountied, player-owned person is crime 1
// ENSLAVING on the cuffer. It stays a pending crime: enslaving is never committed to a bounty (140853ec0).
void cuffing(const Data& w)
{
	// What a cuff leaves on the cuffer's record: a copy of it (the Rig's simulation does not outlive the call) and the victim's id.
	struct Cuff
	{
		std::uint64_t victim = 0;
		std::optional<state::CrimeRecord> record;
	};
	const auto cuff = [&](bool playerOwned, std::int32_t bounty, std::uint8_t slaveState)
	{
		Rig rig(w);
		Cuff out;
		const std::uint64_t slaver = rig.add(w.cities, 3, 1, 0.0, 0.0, 0.0, false);
		out.victim = rig.add(w.nameless, 9, 0, 0.0, 2.0, 0.0, playerOwned);
		if (bounty != 0)
			rig.sim->addBounty(out.victim, w.holy, bounty);
		rig.f(out.victim).slaveState = slaveState;
		rig.sim->reportEnslaving(slaver, out.victim);
		const state::CrimeRecord* held = rig.sim->crimeRecord(slaver);
		if (held != nullptr)
			out.record = *held;
		return out;
	};
	const Cuff cuffed = cuff(true, 0, 0);
	check(cuffed.record && cuffed.record->crime == cr::kEnslaving && cuffed.record->victimFaction == w.nameless
			&& cuffed.record->victimChar == cuffed.victim,
		"crime 1 ENSLAVING on the cuffer, against the victim's faction");
	check(cuffed.record && !cuffed.record->committed && cuffed.record->bounties.empty(),
		"enslaving stays a pending crime, never a bounty");
	check(!cuff(false, 0, 0).record, "a person that is not player-owned gives no crime");
	check(!cuff(true, 1000, 0).record, "a bountied person gives no crime");
	check(!cuff(true, 0, 1).record, "a slave gives no crime");
}

void wanted(const Data& w)
{
	const auto runAs = [&](const std::string& observer, const std::string& holder, int cls, std::int32_t amount, double distance) -> std::uint64_t
	{
		Rig rig(w);
		rig.add(observer, cls, 1, 0.0, 0.0, 0.0, false);
		const std::uint64_t wantedOne = rig.add(w.nameless, 9, 0, 0.0, distance, 3.14159265, true);
		rig.sim->addBounty(wantedOne, holder, amount);
		rig.tick(20 * 8);
		return rig.count(cr::kEvBountySpotted);
	};
	const auto run = [&](int cls, std::int32_t amount, double distance) { return runAs(w.holy, w.holy, cls, amount, distance); };
	// The bounty is held by the observer's LEGAL faction (crime::legalFaction): a faction that offers bounties is its own; the rest name one.
	std::string tied; // a faction with a `legal system` of United Cities that does not offer bounties itself
	for (const kdata::Record* r : w.gd->byType(kworld::itemTypeByName("FACTION")))
	{
		const kdata::RefCategory* legal = r->findRefs("legal system");
		if (tied.empty() && !r->boolOr("offers bounties", false) && legal != nullptr && !legal->entries.empty()
			&& legal->entries.front().targetID == w.cities)
			tied = r->stringID;
	}
	check(!tied.empty(), "a faction whose legal system is United Cities exists");
	check(cr::legalFaction(w.gd.get(), w.holy) == w.holy && cr::legalFaction(w.gd.get(), w.cities) == w.cities, "an offering faction is its own legal faction");
	check(cr::legalFaction(w.gd.get(), tied) == w.cities, "a faction with a legal system holds its bounties there");
	check(cr::legalFaction(w.gd.get(), w.nameless).empty() && cr::legalFaction(w.gd.get(), "").empty(), "no legal faction: none");
	check(runAs(tied, w.cities, 1, 500, 100.0) > 0, "an observer reacts to its legal faction's bounty");
	check(runAs(w.holy, w.cities, 1, 500, 100.0) == 0, "a bounty from another faction only: an observer with a different legal system ignores it");
	check(runAs(tied, w.holy, 1, 500, 100.0) == 0, "and the same the other way round (holder is not the observer's legal faction)");
	check(run(1, 1, 100.0) > 0, "law reacts to any bounty");
	check(run(2, 3000, 100.0) == 0 && run(2, 4500, 100.0) > 0, "military outside its own town: 4000");
	check(run(9, 900, 100.0) == 0 && run(9, 1000, 100.0) > 0, "an adventurer: 1000");
	check(run(4, 9000, 100.0) == 0 && run(4, 10000, 100.0) > 0, "anyone else: 10000");
	check(run(6, 20000, 100.0) == 0, "a slave never");
	check(run(1, 500, 380.0) > 0 && run(1, 500, 450.0) == 0, "within 400 units only");
}

void trespass(const Data& w)
{
	const std::uint64_t kHome = 777;
	const auto run = [&](bool locked, bool guard, bool friendlyIntruder) -> std::pair<std::uint64_t, std::int32_t>
	{
		Rig rig(w);
		const std::uint64_t resident = rig.add(w.holy, 4, 1, 0.0, 0.0, 0.0, false);
		rig.f(resident).building = kHome;
		rig.f(resident).homeBuilding = kHome;
		rig.f(resident).houseLocked = locked;
		if (guard)
		{
			const std::uint64_t paladin = rig.add(w.holy, 1, 1, 40.0, 0.0, 0.0, false);
			rig.f(paladin).building = kHome;
		}
		const std::uint64_t intruder = rig.add(friendlyIntruder ? w.holy : w.nameless, 9, 0, 0.0, 60.0, 3.14159265, true);
		rig.f(intruder).building = kHome;
		rig.tick(20 * 12);
		const state::CrimeRecord* r = rig.sim->crimeRecord(intruder);
		const bool crime11 = r != nullptr && r->crime == cr::kTrespassing;
		const std::int32_t bounty = rig.sim->bountyOn(intruder, w.holy);
		check(!crime11 || rig.count(cr::kEvIntruderFound) > 0, "EV_INTRUDER_FOUND accompanies crime 11");
		return {crime11 ? 1u : 0u, bounty};
	};
	const auto locked = run(true, true, false);
	check(locked.first == 1 && locked.second == 100, "house locked: crime 11 and a bounty of 100 for the faction that witnessed");
	const auto alone = run(true, false, false);
	check(alone.first == 1 && alone.second == 0, "house locked, only a civilian resident: crime 11 but no bounty (not law)");
	const auto open = run(false, true, false);
	check(open.first == 0 && open.second == 0, "house not locked (the default): the sensor stays false, no crime");
	const auto friendly = run(true, true, true);
	check(friendly.first == 0, "a friendly (same faction) character is no intruder");
	// The explicit input defaults to off, and a player-faction check: an NPC walking in is no trespasser.
	Rig rig(w);
	const std::uint64_t resident = rig.add(w.holy, 4, 1, 0.0, 0.0, 0.0, false);
	rig.f(resident).building = kHome;
	rig.f(resident).homeBuilding = kHome;
	rig.f(resident).houseLocked = true;
	const std::uint64_t npc = rig.add(w.nameless, 4, 0, 0.0, 60.0, 3.14159265, false);
	rig.f(npc).building = kHome;
	rig.tick(20 * 10);
	check(rig.sim->crimeRecord(npc) == nullptr, "only player-faction characters trespass");
}

// A town resident that spawns inside an enterable building calls it home; one outside every building, and a player character inside,
// stay unset.
void homeAtSpawn(const Data& w)
{
	Rig rig(w);
	const std::vector<BuildingSite>& sites = rig.sim->world().buildingSites();
	test::require(!sites.empty(), "the world has no building sites");
	const BuildingSite& site = sites.front();
	state::Door door;
	door.key = 1;
	door.building = site.key;
	door.inX = 1.0; // a way in: the building is enterable
	rig.sim->spawnDoor(door);
	const auto spawn = [&](double x, double y, double z, bool player) -> const state::Fighter&
	{
		state::Fighter f = w.data->makeFighter(*w.gd, w.servant, "", rig.rng);
		f.team = 0;
		f.x = x;
		f.y = y;
		f.z = z;
		f.playerControlled = player;
		return rig.f(rig.sim->spawnFighter(std::move(f)).get<state::Id>()->value);
	};
	const double midY = 0.5 * (site.minY + site.maxY);
	const state::Fighter& inside = spawn(site.cx, midY, site.cz, false);
	check(inside.building == site.key && inside.homeBuilding == site.key, "a resident spawned inside a building has it as its home");
	const double far = 2.0 * (site.halfA + site.halfB) + 500.0;
	const state::Fighter& outside = spawn(site.cx + far, midY, site.cz, false);
	check(outside.building == 0 && outside.homeBuilding == 0, "a character spawned outside every building has no home");
	const state::Fighter& player = spawn(site.cx, midY, site.cz, true);
	check(player.building == site.key && player.homeBuilding == 0, "a player character inside a building gets no home");
	// Informational: the box sizes of the world's buildings with doors (the buildings a spawn can land in).
	int bySize[4] = {0, 0, 0, 0}; // longest side: < 50, < 200, < 600, more
	for (const BuildingSite& s : sites)
	{
		const double longest = 2.0 * std::max(s.halfA, s.halfB);
		++bySize[longest < 50.0 ? 0 : longest < 200.0 ? 1 : longest < 600.0 ? 2 : 3];
	}
	std::printf("building sites: %zu; longest side <50: %d, <200: %d, <600: %d, more: %d\n", sites.size(), bySize[0], bySize[1], bySize[2], bySize[3]);
}

} // namespace

int main()
{
	try
	{
		rules();
		const Data w;
		theft(w);
		caughtStealing(w);
		betrayal(w);
		lockpick(w);
		lootBody(w);
		carryInspection(w);
		lawEnforcementCarry(w);
		plainPlayerLift(w);
		expiry(w);
		saveLoad(w);
		twoPlayers(w);
		assault(w);
		joinerAssault(w);
		lawHitCharges(w);
		wanted(w);
		cuffing(w);
		trespass(w);
		homeAtSpawn(w);
	} catch (const std::exception& e) {
		std::fprintf(stderr, "FAIL: %s\n", e.what());
		return 1;
	}
	std::puts("ksim_crime: all ok");
	return 0;
}
