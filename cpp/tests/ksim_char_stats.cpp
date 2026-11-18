// CHAR_STATS (docs/formats/SIMLINK.md "CHAR_STATS and the inspect focus"): the type 49 frame for the session's player character and
// order 87 focus, through a Session on the Hub cache. The frame is decoded strictly (no trailing bytes): skill ids are the fixed table,
// the numbers match the fighter, the player's flows with no focus, an NPC's only while focused, a change (block mode, order 10) is sent
// within a second, an unchanged one is not resent, the focus is never a sim command (logged as session control, like 50) and a bad id is refused.
// argv[1]: the Hub navmesh cache (`knav_bake`, ctest fixture knav_cache).

#include "ksim/net/simlink.hpp"
#include "ksim/net/simlink_scenario.hpp"
#include "ksim/core/simulation.hpp"
#include "ksim/combat/carry.hpp"
#include "ksim/combat/combat.hpp"

#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"

#include <cmath>
#include <cstring>
#include <iostream>
#include <functional>
#include <map>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;
namespace ks = ksim::simlink;

template <typename T>
T at(const std::vector<std::uint8_t>& b, std::size_t pos)
{
	test::require(pos + sizeof(T) <= b.size(), "frame read past the end");
	T v;
	std::memcpy(&v, b.data() + pos, sizeof(T));
	return v;
}

struct Cursor
{
	const std::vector<std::uint8_t>& b;
	std::size_t p = 0;
	template <typename T>
	T get()
	{
		const T v = at<T>(b, p);
		p += sizeof(T);
		return v;
	}
	void done(const char* what) const { test::require(p == b.size(), std::string(what) + ": trailing bytes (" + std::to_string(b.size() - p) + ")"); }
};

struct Stats
{
	std::uint64_t tick = 0;
	std::uint64_t id = 0;
	std::uint8_t state = 0;
	std::uint8_t lockLevel = 0;
	std::string goal;
	std::map<std::uint8_t, float> skills;
	float runSpeed = 0.0f;
	float koPoint = 0.0f;
	// The stats window tail (rows 21-23).
	std::map<std::uint8_t, float> progress;
	float carryKg = 0.0f;
	float carryMaxKg = 0.0f;
	float damageResist = 0.0f;
	// The AI tail (rows 16-20, spectator mode).
	std::uint16_t goalEnum = 0;
	std::uint8_t source = 0;
	std::uint16_t stepEnum = 0;
	std::uint8_t stepIndex = 0;
	std::uint8_t stepCount = 0;
	std::uint64_t targetId = 0;
	float targetX = 0.0f;
	float targetZ = 0.0f;
	// The HUD tag row tail (rows 24-32, kenshi-k16.6.215).
	std::uint16_t tagFlags = 0;
	std::uint8_t slaveState = 0;
	std::string captor;
	float slaveHoursLeft = 0.0f;
	std::uint8_t slaveLook = 0;
	std::uint8_t crime = 0;
	float crimeTimer = 0.0f;
	float escapeHoursLeft = 0.0f;
	std::string localFaction;
	struct Bounty
	{
		std::uint32_t amount = 0; // row 14
		std::uint32_t crimes = 0; // row 32
		std::uint8_t claim = 0;
		float hoursLeft = 0.0f;
	};
	std::vector<Bounty> bounties;
};

struct Rig
{
	ks::ServerContext context;
	std::unique_ptr<Simulation> sim;
	std::unique_ptr<ks::Session> session;
	ks::Duel duel;
	std::uint32_t seq = 0;
	std::vector<Stats> frames; // every CHAR_STATS seen, in order
	std::string refusal; // the last answer to an order: "" applied, else event 10's reason
	bool answered = false;

	Rig(const std::shared_ptr<const kdata::GameData>& gd, const std::shared_ptr<const CombatData>& data, const std::shared_ptr<MeshNavigator>& nav,
		const kworld::LoadOrder& order)
	{
		sim = std::make_unique<Simulation>(Simulation::load(KENSHI_DATA, 91));
		sim->setGameData(gd);
		context.gameData = gd;
		context.dataHash = ks::dataHash(order);
		CombatSettings settings;
		settings.substeps = 2;
		sim->enableCombat(data, settings);
		sim->enableNavigation(nav);
		duel = ks::spawnDuel(*sim, *gd, *data, context);
		session = std::make_unique<ks::Session>(*sim, context);
		std::vector<std::uint8_t> bytes;
		ks::BinaryWriter w(bytes);
		session->begin(w);
		std::vector<std::uint8_t> hello;
		ks::BinaryWriter h(hello);
		h.begin(ks::kHello);
		h.u32("magic", ks::kMagic);
		h.u16("protocol", ks::kProtocol);
		h.u8("role", 0);
		h.u64("dataHash", context.dataHash);
		h.str("name", "test");
		h.end();
		test::require(session->onFrame(ks::kHello, hello.data() + 6, hello.size() - 6, w), "hello refused");
		session->sendTick(w, true);
		parse(bytes, 0);
	}

	Stats decode(const std::vector<std::uint8_t>& body)
	{
		Cursor c{body};
		Stats s;
		s.tick = c.get<std::uint64_t>();
		s.id = c.get<std::uint64_t>();
		s.state = c.get<std::uint8_t>();
		s.lockLevel = c.get<std::uint8_t>();
		s.goal = session->strings().text(c.get<std::uint32_t>());
		c.get<float>(); // encumbrance
		c.get<std::uint8_t>(); // extraKind
		c.get<float>(); // extra
		test::require(c.get<std::uint8_t>() <= 2, "weaponClass 0..2");
		const std::uint8_t n = c.get<std::uint8_t>();
		for (std::uint8_t i = 0; i < n; ++i)
		{
			const std::uint8_t skill = c.get<std::uint8_t>();
			s.skills[skill] = c.get<float>();
			c.get<float>(); // bonus
		}
		for (int i = 0; i < 3; ++i)
			c.get<float>(); // XP rates
		s.koPoint = c.get<float>();
		s.runSpeed = c.get<float>();
		c.get<float>(); // runPenalty
		c.get<float>(); // hungerRate
		test::require(c.get<float>() >= 0.0f, "light 0..1");
		const std::uint8_t bounties = c.get<std::uint8_t>();
		for (std::uint8_t i = 0; i < bounties; ++i)
		{
			c.get<std::uint32_t>();
			s.bounties.push_back({c.get<std::uint32_t>()});
		}
		c.get<float>(); // sentenceHours
		s.goalEnum = c.get<std::uint16_t>();
		s.source = c.get<std::uint8_t>();
		s.stepEnum = c.get<std::uint16_t>();
		s.stepIndex = c.get<std::uint8_t>();
		s.stepCount = c.get<std::uint8_t>();
		s.targetId = c.get<std::uint64_t>();
		s.targetX = c.get<float>();
		s.targetZ = c.get<float>();
		const std::uint8_t progressCount = c.get<std::uint8_t>();
		for (std::uint8_t i = 0; i < progressCount; ++i)
		{
			const std::uint8_t skill = c.get<std::uint8_t>();
			s.progress[skill] = c.get<float>();
		}
		s.carryKg = c.get<float>();
		s.carryMaxKg = c.get<float>();
		s.damageResist = c.get<float>();
		s.tagFlags = c.get<std::uint16_t>();
		s.slaveState = c.get<std::uint8_t>();
		s.captor = session->strings().text(c.get<std::uint32_t>());
		s.slaveHoursLeft = c.get<float>();
		s.slaveLook = c.get<std::uint8_t>();
		s.crime = c.get<std::uint8_t>();
		s.crimeTimer = c.get<float>();
		s.escapeHoursLeft = c.get<float>();
		s.localFaction = session->strings().text(c.get<std::uint32_t>());
		test::require(c.get<std::uint8_t>() == bounties, "row 32's n equals row 14's");
		for (Stats::Bounty& bounty : s.bounties)
		{
			bounty.crimes = c.get<std::uint32_t>();
			bounty.claim = c.get<std::uint8_t>();
			bounty.hoursLeft = c.get<float>();
		}
		c.done("CHAR_STATS");
		return s;
	}

	void parse(const std::vector<std::uint8_t>& bytes, std::uint32_t mine)
	{
		for (std::size_t p = 0; p < bytes.size();)
		{
			const std::uint32_t len = at<std::uint32_t>(bytes, p);
			const std::uint16_t type = at<std::uint16_t>(bytes, p + 4);
			test::require(p + 4 + len <= bytes.size(), "frame length past the end");
			const std::vector<std::uint8_t> body(bytes.begin() + static_cast<long>(p) + 6, bytes.begin() + static_cast<long>(p) + 4 + len);
			p += 4 + len;
			if (type == ks::kCharStats)
			{
				frames.push_back(decode(body));
			} else if (type == ks::kEvent && mine != 0) {
				const std::uint16_t kind = at<std::uint16_t>(body, 8);
				if ((kind == 9 || kind == 10) && at<std::uint32_t>(body, 10) == mine)
				{
					answered = true;
					refusal = kind == 10 ? session->strings().text(at<std::uint32_t>(body, 14)) : "";
				}
			}
		}
	}

	void run(int ticks)
	{
		for (int i = 0; i < ticks; ++i)
		{
			sim->tick();
			std::vector<std::uint8_t> bytes;
			ks::BinaryWriter w(bytes);
			session->sendTick(w, false);
			parse(bytes, seq);
		}
	}

	void order(std::uint16_t kind, const std::function<void(ks::BinaryWriter&)>& body)
	{
		std::vector<std::uint8_t> frame;
		ks::BinaryWriter w(frame);
		w.begin(ks::kOrder);
		w.u32("clientSeq", ++seq);
		w.u16("kind", kind);
		body(w);
		w.end();
		std::vector<std::uint8_t> out;
		ks::BinaryWriter reply(out);
		answered = false;
		refusal.clear();
		test::require(session->onFrame(ks::kOrder, frame.data() + 6, frame.size() - 6, reply), "an order kicked the client");
		parse(out, seq);
		run(2); // applied at the next tick boundary
		test::require(answered, "order " + std::to_string(kind) + " not answered");
	}
	void focus(std::uint64_t id) { order(87, [id](ks::BinaryWriter& w) { w.u64("id", id); }); }
	// Order 87 with no tick after it: what the reply alone carries (a paused server).
	void focusNoTick(std::uint64_t id)
	{
		std::vector<std::uint8_t> frame;
		ks::BinaryWriter w(frame);
		w.begin(ks::kOrder);
		w.u32("clientSeq", ++seq);
		w.u16("kind", 87);
		w.u64("id", id);
		w.end();
		std::vector<std::uint8_t> out;
		ks::BinaryWriter reply(out);
		answered = false;
		refusal.clear();
		test::require(session->onFrame(ks::kOrder, frame.data() + 6, frame.size() - 6, reply), "an order kicked the client");
		parse(out, seq);
		test::require(answered, "order 87 not answered");
	}

	int count(std::uint64_t id) const
	{
		int n = 0;
		for (const Stats& s : frames)
			n += s.id == id;
		return n;
	}
	const Stats& last(std::uint64_t id) const
	{
		for (auto it = frames.rbegin(); it != frames.rend(); ++it)
		{
			if (it->id == id)
				return *it;
		}
		throw test::Failure("no CHAR_STATS for " + std::to_string(id));
	}
};

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc == 2, "usage: ksim_char_stats HUB.knav");
		const auto gd = std::make_shared<const kdata::GameData>(kworld::loadMergedGameData(KENSHI_DATA));
		const std::shared_ptr<const CombatData> data = CombatData::build(*gd, KENSHI_DATA);
		const std::shared_ptr<MeshNavigator> nav = MeshNavigator::load(argv[1]);
		Rig rig(gd, data, nav, kworld::vanillaLoadOrder(KENSHI_DATA));
		const std::uint64_t player = rig.duel.player;
		const std::uint64_t enemy = rig.duel.enemy;

		// ---- the player's frame on connect, no focus needed; the NPC's not ----
		test::require(rig.count(player) == 1 && rig.count(enemy) == 0, "connect: one CHAR_STATS for the player, none for the NPC");
		{
			const Stats& s = rig.last(player);
			const state::Fighter& f = *rig.sim->combat()->find(player);
			test::require(s.skills.size() == 33, "33 skills (ids 0..32)");
			for (std::uint8_t i = 0; i < 33; ++i)
				test::require(s.skills.count(i) == 1, "skill id " + std::to_string(i));
			test::require(s.skills.at(0) == static_cast<float>(f.skills.strength) && s.skills.at(16) == static_cast<float>(f.skills.stealth)
					&& s.skills.at(12) == static_cast<float>(f.skills.unarmed),
				"ids 0 strength, 12 martial arts, 16 stealth carry the fighter's values");
			test::require(s.state == 0 && s.goal.empty(), "the player: Normal, Aimless");
			test::require(s.goalEnum == 0xFFFF && s.source == 0, "the idle character's tail: no goal enum, source 0");
			test::require(s.stepEnum == 0xFFFF && s.stepIndex == 0 && s.stepCount == 0, "the idle character has no plan step");
			test::require(s.targetId == 0 && std::isnan(s.targetX) && std::isnan(s.targetZ), "the idle character has no target point");
			test::require(s.runSpeed > 11.0f && s.koPoint < 0.0f, "a run speed above the floor, a negative KO point");
			std::cout << "player: run " << s.runSpeed << " u/s, KO point " << s.koPoint << "\n";
		}

		// ---- unchanged: not resent ----
		rig.run(60);
		test::require(rig.count(player) <= 4, "at most once per second per character (" + std::to_string(rig.count(player)) + " in 3 s)");

		// ---- a change goes out within a second: block mode (order 10 which 0) -> Defensive ----
		rig.order(10, [player](ks::BinaryWriter& w) { w.u16("n", 1); w.u64("id", player); w.u8("which", 0); w.u8("value", 1); });
		test::require(rig.refusal.empty(), "block mode toggle applied");
		rig.run(21);
		test::require(rig.last(player).state == 1, "Defensive after block mode");


		// ---- the focus ----
		const std::size_t logged = rig.context.orderLog.size();
		test::require(rig.count(enemy) == 0, "no NPC frame before a focus");
		rig.focus(enemy);
		test::require(rig.refusal.empty(), "focus on the NPC applied");
		rig.run(2);
		test::require(rig.count(enemy) == 1, "the focused NPC gets a frame at once");
		test::require(rig.last(enemy).skills.size() == 33, "the NPC's skills");
		for (std::size_t i = logged; i < rig.context.orderLog.size(); ++i)
			test::require(!rig.context.orderLog[i].sim, "order 87 is session control: logged, never replayed (as 50)");
		rig.focus(0);
		test::require(rig.refusal.empty(), "focus 0 applied");
		const int before = rig.count(enemy);
		rig.run(40);
		test::require(rig.count(enemy) == before, "no NPC frame after focus 0");
		test::require(rig.count(player) >= 1, "the player's frame still flows");
		rig.focus(0xFFFFFFFFull);
		test::require(rig.refusal == "bad_target", "focus on no character: bad_target");

		// ---- a paused server (kenshi-k16.5.5): the focus's frame is in the order's own reply, stamped with the paused tick ----
		{
			rig.context.paused = true;
			const int heard = rig.count(enemy);
			const std::uint64_t pausedTick = rig.sim->clockTick();
			rig.focusNoTick(enemy);
			test::require(rig.refusal.empty(), "paused: focus on the NPC applied");
			test::require(rig.count(enemy) == heard + 1, "paused: the new focus's CHAR_STATS arrives with the ack, before any tick");
			test::require(rig.last(enemy).tick == pausedTick && rig.sim->clockTick() == pausedTick, "paused: the frame carries the paused tick");
			rig.focusNoTick(0);
			rig.context.paused = false;
		}

		// ---- the AI tail (rows 16-20, spectator mode; SIMLINK.md "CHAR_STATS") ----
		// The idle case is the player at connect (already decoded above): no goal, so 0xFFFF / 0 / no step, no target. This world is
		// the duel's (two fighters, no town detail), so the other two states are set on the servant's routine component: it is in no
		// world or town squad, so nothing re-scores or replaces the routine while the frames go out (the plan is the saved one, v82).
		Simulation& sim = *rig.sim;
		const auto entityOf = [&](std::uint64_t id) {
			flecs::entity found;
			sim.world().ecs().each([&](flecs::entity e, const state::Id& i, const state::Fighter&) { found = i.value == id ? e : found; });
			return found;
		};
		// A calm NPC: the duel's servant is fighting (the row reads the fight first), so the fixture puts it on the player's team and
		// clears the fight. Nothing re-targets it, so it has no goal at all until a routine is set on it.
		{
			state::Fighter* other = const_cast<state::Fighter*>(sim.combat()->find(enemy));
			other->team = sim.combat()->find(player)->team;
			other->target = 0;
			other->technique = -1;
			other->state = state::CombatState::Hub;
			other->combatGoal = 0;
		}
		rig.run(2);
		rig.focus(enemy);
		test::require(rig.refusal.empty(), "focus on the servant applied");
		rig.run(21);
		{
			const Stats& s = rig.last(enemy);
			test::require(s.goal.empty() && s.goalEnum == 0xFFFF && s.source == 0, "an idle NPC: Aimless, goalEnum 0xFFFF, source 0");
			test::require(s.stepEnum == 0xFFFF && s.stepIndex == 0 && s.stepCount == 0, "an idle NPC has no plan step");
			test::require(s.targetId == 0 && std::isnan(s.targetX) && std::isnan(s.targetZ), "an idle NPC has no target");
		}
		// A medic mid-plan: the routine slot's 58 JOB_MEDIC with the [25, 58] chain on the player.
		const state::Fighter& patient = *sim.combat()->find(player);
		{
			state::Routine routine;
			routine.goal = 58; // JOB_MEDIC
			routine.since = sim.clockTick();
			routine.chainTarget = player;
			routine.plan = { 25, 58 }; // FIRST_AID_ORDER -> JOB_MEDIC
			routine.step = 0;
			entityOf(enemy).set<state::Routine>(routine);
			rig.focus(enemy);
			test::require(rig.refusal.empty(), "focus on the servant applied");
			rig.run(21);
			const Stats& s = rig.last(enemy);
			test::require(s.goal == "Medic" && s.goalEnum == 58, "the medic: row 4 and row 16 agree (58 Medic)");
			test::require(s.source == 3, "the medic's goal came from the routine slot (3)");
			test::require(s.stepEnum == 25 && s.stepIndex == 0 && s.stepCount == 2, "the plan's first step: 25 of [25, 58]");
			test::require(s.targetId == player && std::fabs(s.targetX - static_cast<float>(patient.x)) < 0.01f
					&& std::fabs(s.targetZ - static_cast<float>(patient.z)) < 0.01f,
				"the target is the patient, with its point");
			// The second step.
			state::Routine next = *entityOf(enemy).get<state::Routine>();
			next.step = 1;
			entityOf(enemy).set<state::Routine>(next);
			rig.run(21);
			test::require(rig.last(enemy).stepEnum == 58 && rig.last(enemy).stepIndex == 1, "the plan's second step: 58");
		}
		// A guard on 38 Static guard, from the routine slot too.
		{
			state::Routine routine;
			routine.goal = 38; // STATIC_GUARD
			routine.since = sim.clockTick();
			entityOf(enemy).set<state::Routine>(routine);
			rig.run(21);
			const Stats& s = rig.last(enemy);
			test::require(s.goal == "Static guard" && s.goalEnum == 38 && s.source == 3, "the guard: 38 Static guard from the routine slot");
			test::require(s.stepEnum == 0xFFFF && s.stepCount == 0, "the guard's goal has no stored plan");
			entityOf(enemy).remove<state::Routine>();
		}

		// ---- the HUD tag row tail (rows 24-32, kenshi-k16.6.215) on the servant, one state at a time; each change goes out within a second ----
		{
			state::Fighter& npc = *const_cast<state::Fighter*>(sim.combat()->find(enemy));
			const auto settle = [&] { rig.run(21); return rig.last(enemy); };
			{
				const Stats s = settle();
				test::require(s.tagFlags == 0 && s.slaveState == 0 && s.captor.empty() && s.slaveHoursLeft == -1.0f && s.slaveLook == 255
						&& s.escapeHoursLeft == -1.0f && s.bounties.empty(),
					"a plain character: no flags, free, no captor, hours -1, look 255, no bounties (the duel's assault crime is real; flags " + std::to_string(s.tagFlags) + " state " + std::to_string(s.slaveState) + " hours " + std::to_string(s.slaveHoursLeft) + " crime " + std::to_string(s.crime) + " bounties " + std::to_string(s.bounties.size()) + ")");
			}
			// a leader: the head of a player squad's member list (the duel's player); the servant is in no list
			test::require((rig.last(player).tagFlags & 1) != 0 && (rig.last(enemy).tagFlags & 1) == 0, "b0 squad leader: the player's squad head, not the servant");
			// a captive in transit, and an obedient slave in state 1 with a captor
			npc.captive = true;
			test::require((settle().tagFlags & 4) != 0, "captive: b2");
			npc.captive = false;
			npc.obedient = true;
			npc.captorSid = "slavers";
			npc.slaveState = 1;
			{
				const Stats s = settle();
				test::require((s.tagFlags & 64) != 0 && (s.tagFlags & 4) == 0 && s.slaveState == 1 && s.captor == "slavers" && s.slaveHoursLeft == -1.0f,
					"a slave (state 1): b6 obedient, the captor, no hours left");
				test::require((s.tagFlags & (2 | 8 | 32 | 16)) == 0 && s.slaveLook == 255 && s.escapeHoursLeft == -1.0f,
					"the unmodelled bits (b1, b3, b4, b5) read 0, look 255, escape hours -1");
			}
			npc.obedient = false;
			npc.slaveState = 0; // the slave step drops its track of state 1 (a captor-less state 1 re-stamps as an escape otherwise)
			rig.run(2);
			// an escaped slave (state 2, 12 h window) and an ex-slave (state 3, 96 h)
			const double nowHours = gameHoursIn(sim.clockTick());
			npc.slaveState = 2;
			npc.slaveStamp = nowHours; // the clock is young: a stamp before it would be negative, which reads as unstamped
			{
				const Stats s = settle();
				test::require(std::fabs(s.slaveHoursLeft - 12.0f) < 0.1f, "state 2: 12 h from the stamp (state " + std::to_string(s.slaveState) + ", hours " + std::to_string(s.slaveHoursLeft) + ")");
			}
			npc.slaveState = 3;
			npc.slaveStamp = gameHoursIn(sim.clockTick());
			{
				const Stats s = settle();
				test::require(s.slaveState == 3 && std::fabs(s.slaveHoursLeft - 96.0f) < 0.1f, "state 3: 96 h from the stamp");
			}
			npc.slaveState = 0;
			npc.slaveStamp = -1.0;
			npc.captorSid.clear();
			// a crime in progress: N = (int)timer, 3 s or less = caught-in-the-act, over = witnessed
			state::CrimeRecord record;
			record.crime = 5; // assault
			record.timer = 12.5;
			entityOf(enemy).set<state::CrimeRecord>(record);
			rig.run(21);
			test::require(rig.last(enemy).crime == 5 && rig.last(enemy).crimeTimer > 0.0f && rig.last(enemy).crimeTimer <= 12.5f, "a pending assault: crime 5 with its timer");
			// bounties: one with an expiry, a claimed one that never expires
			record = *entityOf(enemy).get<state::CrimeRecord>();
			record.crime = 0;
			record.timer = 0.0;
			record.bounties.clear();
			record.bounties.push_back({"dust bandits", 500, (1u << 5) | (1u << 3), false, gameHoursIn(sim.clockTick()) - 5.0}); // 20 h to live, 5 gone
			record.bounties.push_back({"holy nation", 12000, 1u << 4, true, gameHoursIn(sim.clockTick())});
			entityOf(enemy).set<state::CrimeRecord>(record);
			rig.run(21);
			{
				const Stats& s = rig.last(enemy);
				test::require(s.crime == 0 && s.bounties.size() == 2, "two bounties, no pending crime");
				test::require(s.bounties[0].crimes == ((1u << 5) | (1u << 3)) && s.bounties[0].claim == 0 && std::fabs(s.bounties[0].hoursLeft - 15.0f) < 0.1f,
					"a bounty of 500: its crime bits, unclaimed, 15 h left of 20");
				test::require(s.bounties[1].crimes == (1u << 4) && s.bounties[1].claim == 1 && s.bounties[1].hoursLeft == -1.0f,
					"a claimed bounty of 12000 stays in row 14: claim 1, never expires");
			}
			entityOf(enemy).remove<state::CrimeRecord>();
			// unchanged: nothing new is sent for a character whose tail did not change
			rig.run(21);
			const int sent = rig.count(enemy);
			rig.run(60);
			test::require(rig.count(enemy) == sent, "an unchanged character sends nothing new");
		}

		// ---- a caged character reads Locked / Unlocked (state 5 / 6, kenshi-k16.6.36), before Unconscious or Defensive; the lock level follows ----
		// The Hub has no cage: the cage is a building key the sim knows no door for, so the lock is the default (a cage with a prisoner is locked).
		const_cast<state::Fighter*>(rig.sim->combat()->find(player))->cageBuilding = 12345;
		rig.run(21);
		test::require(rig.last(player).state == 5, "a caged character in a locked cage reads state 5 on the wire (" + std::to_string(rig.last(player).state) + ")");
		test::require(rig.last(player).lockLevel == static_cast<std::uint8_t>(std::clamp(rig.sim->cageLockLevel(12345), 0.0, 255.0)), "state 5 carries the cage's lock level");
		const_cast<state::Fighter*>(rig.sim->combat()->find(player))->cageBuilding = 0;
		rig.run(21);
		test::require(rig.last(player).state != 5 && rig.last(player).state != 6 && rig.last(player).lockLevel == 0, "out of the cage: no cage state, lock level 0");
		// ---- the stats window tail (rows 21-23 and skill ids 19-32, kenshi-k16.6.54) ----
		{
			rig.run(21);
			const state::Fighter& f = *rig.sim->combat()->find(player);
			const char* keys[] = {"perception", "swimming", "robotics", "weapon smith", "armour smith", "bow smith", "farming", "cooking", "ff"};
			// The duel's characters have no template link: give the player a known one, a CHARACTER whose STATS carry a perception above 1.
			const kdata::Record* character = nullptr;
			for (const kdata::Record* record : gd->byType(kworld::itemTypeByName("CHARACTER")))
			{
				const kdata::RefCategory* ref = record->findRefs("stats");
				const kdata::Record* linked = ref != nullptr && !ref->entries.empty() ? gd->find(ref->entries.front().targetID) : nullptr;
				if (character == nullptr && linked != nullptr && linked->floatOr("perception", 1.0f) > 1.0f && linked->floatOr("ff", 1.0f) >= 1.0f)
					character = record;
			}
			test::require(character != nullptr, "the data has a CHARACTER whose STATS perception is above 1");
			rig.sim->world().ecs().each([&](flecs::entity e, const state::Id& id)
			{
				if (id.value == player)
					e.set<state::CharacterInstance>({character->stringID});
			});
			rig.run(25);
			const Stats& s = rig.last(player);
			// 19-23 are the fighter's; 24-32 the template's STATS record (keys by the data: `ff` is precision shooting, `bow smith` the crossbow smith).
			test::require(s.skills.at(19) == static_cast<float>(f.skills.engineer) && s.skills.at(20) == static_cast<float>(f.skills.crossbows)
					&& s.skills.at(21) == static_cast<float>(f.skills.turrets) && s.skills.at(22) == static_cast<float>(f.skills.thievery)
					&& s.skills.at(23) == static_cast<float>(f.skills.assassination),
				"ids 19-23 carry the fighter's engineer, crossbows, turrets, thievery, assassination");
			const kdata::Record* stats = gd->find(character->findRefs("stats")->entries.front().targetID);
			test::require(stats != nullptr, "the STATS record resolves");
			bool nonDefault = false;
			for (std::size_t i = 0; i < std::size(keys); ++i)
			{
				const float want = stats->floatOr(keys[i], 1.0f);
				nonDefault = nonDefault || want != 1.0f;
				test::require(s.skills.at(static_cast<std::uint8_t>(24 + i)) == want, std::string("id ") + std::to_string(24 + i) + " is the template's " + keys[i]);
			}
			std::cout << "template STATS: perception " << s.skills.at(24) << ", swimming " << s.skills.at(25) << ", ff " << s.skills.at(32) << (nonDefault ? "" : " (all 1)") << "\n";
			// Row 21: the same ids as row 8, the fractional part of each value.
			test::require(s.progress.size() == s.skills.size(), "row 21 n equals row 8 n");
			for (const auto& [skill, value] : s.skills)
			{
				test::require(s.progress.count(skill) == 1, "row 21 has skill " + std::to_string(skill));
				test::require(std::fabs(s.progress.at(skill) - (value - std::floor(value))) < 1e-6f && s.progress.at(skill) >= 0.0f && s.progress.at(skill) < 1.0f,
					"row 21 progress is the fraction of skill " + std::to_string(skill));
			}
			// Row 22: one known item in the pack moves carryKg by its weight; the capacity is carry.hpp's.
			const kdata::Record* item = nullptr;
			for (const kdata::Record* record : gd->byType(kworld::itemTypeByName("ITEM")))
			{
				if (item == nullptr && record->floatOr("weight kg", 0.0f) >= 1.0f)
					item = record;
			}
			test::require(item != nullptr, "the data has an item with a weight");
			const float before = s.carryKg;
			rig.sim->world().ecs().each([&](flecs::entity e, const state::Id& id, const state::Inventory& pack)
			{
				if (id.value != player)
					return;
				state::Inventory next = pack;
				next.add(item->stringID, 3);
				++next.version;
				e.set<state::Inventory>(std::move(next));
			});
			rig.run(25);
			const Stats& loaded = rig.last(player);
			test::require(std::fabs((loaded.carryKg - before) - 3.0f * item->floatOr("weight kg", 0.0f)) < 1e-3f, "three of " + item->name + " add three times its weight to carryKg");
			const CombatConstants& k = data->k;
			const double capacity = carryCapacity(f.skills.strength * CombatRules::effectiveStat(f, CombatRules::kStatStrength), k.carryWeightMult, k.encumbranceBase);
			test::require(std::fabs(loaded.carryMaxKg - static_cast<float>(capacity)) < 1e-3f && loaded.carryMaxKg > 0.0f, "carryMaxKg is carry.hpp's capacity (" + std::to_string(loaded.carryMaxKg) + ")");
			// Row 23: Engival's toughness 55.527668 (RESOLUTION.md 5.3) = lerp(min, max, 0.55527668); rebirth -0.65..0.65 gives 0.0719.
			const_cast<state::Fighter*>(rig.sim->combat()->find(player))->skills.toughness = 55.527668;
			rig.run(25);
			const double want = k.resistanceMin + (k.resistanceMax - k.resistanceMin) * 0.55527668;
			test::require(std::fabs(rig.last(player).damageResist - static_cast<float>(want)) < 1e-5f, "damageResist is lerp(min, max, toughness / 100)");
			if (k.resistanceMin == -0.65 && k.resistanceMax == 0.65)
				test::require(std::fabs(rig.last(player).damageResist - 0.0719f) < 1e-4f, "rebirth constants: toughness 55.527668 resists 0.0719");
			std::cout << "tail: carry " << loaded.carryKg << " / " << loaded.carryMaxKg << " kg, resist " << rig.last(player).damageResist << " (min " << k.resistanceMin << ", max " << k.resistanceMax << ")\n";
		}
		std::cout << "tail: idle 0xFFFF, medic 58 plan 25/58, guard 38\n";
		std::cout << "ksim_char_stats ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "error: " << e.what() << "\n";
		return 1;
	}
}
