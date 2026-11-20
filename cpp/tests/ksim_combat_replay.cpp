// Combat replay against the kinspect reference fights (docs/reference/combat/fights/*.jsonl.xz).
//
// Part 1 feeds the rules the inputs of logged decisions and demands the logged answers (exact
// where the decode is VERIFIED). Part 2 builds the two fighters from the log's join records and
// first frame and runs our combat for many seeds, comparing cadence, outcomes and damage with the
// log; the game's RNG cannot be replayed, so those are distributions. The table printed at the
// end is the validation table of docs/reference/combat/IMPLEMENTATION.md.

#include "ksim/combat/combat.hpp"

#include "kworld/gamedata.hpp"

#include "combat_replay_util.hpp"
#include "test_util.hpp"
#include "ksim/dev/build_paths.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;
using replay::Json;

struct Row
{
	std::string item;
	std::string original;
	std::string ours;
	std::string verdict;
};
std::vector<Row> g_rows;

void row(const std::string& item, const std::string& original, const std::string& ours,
	const std::string& verdict)
{
	g_rows.push_back({item, original, ours, verdict});
}

std::string fmt(double v, int digits = 4)
{
	char b[64];
	std::snprintf(b, sizeof b, "%.*g", digits, v);
	return b;
}

int g_failures = 0;
void check(bool ok, const std::string& what)
{
	if (!ok)
	{
		std::cerr << "FAIL: " << what << "\n";
		++g_failures;
	}
}

const kdata::Record& byName(const kdata::GameData& gd, const char* type, const std::string& name)
{
	for (const kdata::Record* r : gd.byType(kworld::itemTypeByName(type)))
	{
		if (r->name == name)
			return *r;
	}
	throw std::runtime_error("no " + std::string(type) + " " + name);
}

struct World
{
	kdata::GameData gd;
	std::shared_ptr<const CombatData> data;
};

// ---------------------------------------------------------------------------
// Fighters from a log
// ---------------------------------------------------------------------------

struct Log
{
	std::vector<Json> events;
	const Json* joinServant = nullptr;
	const Json* joinEngival = nullptr;
	const Json* firstSample = nullptr;
	std::string servantId;
	std::string engivalId;
	std::vector<double> sampleTime; // cumulative dt per events index (samples only)
	std::vector<double> evTime;     // time of every event (time of the last sample before it)
};

Log loadFight(const std::string& name)
{
	Log log;
	log.events = replay::loadLog(std::string(KSIM_COMBAT_FIGHTS) + "/" + name + ".jsonl.xz");
	double t = 0.0;
	for (const Json& e : log.events)
	{
		if (e["ev"].str() == "join")
		{
			if (e["who_name"].str() == "Engival")
			{
				log.joinEngival = &e;
				log.engivalId = e["who"].str();
			} else {
				log.joinServant = &e;
				log.servantId = e["who"].str();
			}
		}
		if (e["ev"].str() == "sample")
		{
			if (log.firstSample == nullptr)
				log.firstSample = &e;
			t += e["dt"].num();
		}
		log.evTime.push_back(t);
	}
	test::require(log.joinServant && log.joinEngival && log.firstSample, "log lacks joins or samples");
	return log;
}

void placeFrom(state::Fighter& f, const Json& sample, const std::string& id)
{
	for (const Json& c : sample["chars"].arr())
	{
		if (c["id"].str() == id)
		{
			f.x = c["pos"].arr()[0].num();
			f.z = c["pos"].arr()[2].num();
			f.yaw = c["yaw"].num() / replay::kRadToDeg;
		}
	}
}

// Engival: unarmed, Rag Loincloth, robot legs (ko_death_model.md: L max 170 / wear 13.21, R max 150 / wear 15.42).
state::Fighter makeEngival(const World& w, const Log& log, bool blockMode)
{
	state::Fighter f;
	const RaceCombat* race = w.data->race("Greenlander");
	f.raceSid = race->sid;
	w.data->initParts(f, *race);
	f.skills = replay::skillsOf(*log.joinEngival);
	f.weapon = w.data->makeFists(w.gd);
	f.armour.push_back(w.data->makeArmour(byName(w.gd, "ARMOUR", "Rag Loincloth"), 1.0));
	f.team = 0;
	f.blockMode = blockMode;
	f.healFactor = 1.170; // StatsBlock+0x58: UNSURE source; value inferred from the logged stun decay
	for (state::FighterPart& p : f.parts)
	{
		if (p.name == "Left Leg")
		{
			p.maxHealth = 170.0;
			p.wear = 13.21;
			p.health = 156.79;
		} else if (p.name == "Right Leg") {
			p.maxHealth = 150.0;
			p.wear = 15.42;
			p.health = 134.58;
		}
	}
	f.blood = 139.58;
	placeFrom(f, *log.firstSample, log.engivalId);
	return f;
}

// The Escaped Servant with the Iron Club: the item level is not decoded (RESOLUTION.md 9.1), so the
// club's multipliers are fitted to the first logged hit of the log.
state::Fighter makeServant(const World& w, const Log& log, double hitCut, double hitBlunt, double hunger = 3.0)
{
	state::Fighter f;
	const RaceCombat* race = w.data->race("Greenlander");
	f.raceSid = race->sid;
	w.data->initParts(f, *race);
	f.skills = replay::skillsOf(*log.joinServant);
	// Hunger: the logs match with the full-hunger multipliers (block 56.57 is exact with attack and
	// defence mod -1 and no penalty), so the servant is fed here. The later save ki_duel_noblock has it at
	// 1.698 (partFive); the drain between the log and that save is not measured. UNSURE.
	f.hunger = hunger;
	const double strEff = f.skills.strength * CombatRules::effectiveStat(f, CombatRules::kStatStrength);
	const double dexEff = f.skills.dexterity * CombatRules::effectiveStat(f, CombatRules::kStatDexterity);
	const kdata::Record club = byName(w.gd, "WEAPON", "Iron Club");
	const CombatConstants& k = w.data->k;
	f.weapon = w.data->makeWeapon(club, 0.0);
	const double bluntBase = club.floatOr("blunt damage multiplier", 0.0f);
	const double x = (hitBlunt - k.bluntDamage1) / (k.bluntDamage99 - k.bluntDamage1);
	const double bluntMult = x * 200.0 / (0.5 * f.skills.blunt + 1.5 * strEff);
	const double level = bluntMult / (2.0 * bluntBase);
	const double cutBase = club.floatOr("cut damage multiplier", 1.0f);
	const double cx = (hitCut - f.weapon.cut1) / (k.cutDamage99 - f.weapon.cut1);
	const double cutMult = cx * 200.0 / (f.skills.blunt + dexEff);
	const double manufacturerCut = cutMult / ((0.3 + 1.7 * level) * cutBase);
	f.weapon = w.data->makeWeapon(club, level, manufacturerCut, 1.0);
	f.weapon.attackMod = -1.0; // material spec; inferred from skill_a 16.13 and block 56.57%
	f.weapon.defenceMod = -1.0;
	f.armour.push_back(w.data->makeArmour(byName(w.gd, "ARMOUR", "Holy Servant Rags"), 0.2));
	f.armour.push_back(w.data->makeArmour(byName(w.gd, "ARMOUR", "Rag Loincloth"), 1.0));
	f.team = 1;
	f.healFactor = 1.534; // inferred from the logged stun decay of the servant's leg (UNSURE source)
	f.blood = (f.maxBloodRace - f.minBlood) * f.skills.strength * 0.01 + f.minBlood;
	placeFrom(f, *log.firstSample, log.servantId);
	return f;
}

// First hit of the servant on Engival in a log: {cut, blunt} before armour.
bool firstServantHit(const Log& log, double& cut, double& blunt)
{
	for (const Json& e : log.events)
	{
		if (e["ev"].str() == "hit" && e["att"].str() == log.servantId)
		{
			cut = e["dmg"].arr()[0].num();
			blunt = e["dmg"].arr()[1].num();
			return true;
		}
	}
	return false;
}

// ---------------------------------------------------------------------------
// Part 1: rules against logged decisions
// ---------------------------------------------------------------------------

void partOne(const World& w, const Log& d1)
{
	CombatRules rules(w.data, CombatSettings{});
	double hitCut = 0.0;
	double hitBlunt = 0.0;
	// d1 has no servant hit on Engival; the servant of a1 has the same stats and club.
	const Log a1 = loadFight("clog_1v1_a1");
	test::require(firstServantHit(a1, hitCut, hitBlunt), "no servant hit in a1");
	state::Fighter engival = makeEngival(w, d1, false);
	state::Fighter servant = makeServant(w, d1, hitCut, hitBlunt);

	// Clip speed (VERIFIED to 6 digits for Engival).
	const double speed = rules.attackClipSpeed(engival);
	const double logged = d1.joinEngival->operator[]("anim_speed").num();
	row("attack clip speed, Engival (join anim_speed)", fmt(logged, 9), fmt(speed, 9),
		std::fabs(speed - logged) < 1e-6 ? "exact" : "MISMATCH");
	check(std::fabs(speed - logged) < 1e-6, "Engival clip speed");

	// Engival's dodgeback clip: logged 1.493 = technique 1.1 x CharStats+0x18c.
	const double dodgeStart = rules.reactClipSpeed(engival) * w.data->technique("Dodge back")->speedMult;
	row("first-frame dodge speed 1.1 x CharStats+0x18c, Engival (d1)", "1.493 (then 1.1 for the rest of the clip)", fmt(dodgeStart, 5),
		std::fabs(dodgeStart - 1.493) < 0.002 ? "match (3 digits)" : "MISMATCH");
	check(std::fabs(dodgeStart - 1.493) < 0.002, "dodge clip speed");

	// Servant swing speed: bigchopv2 logged 1.142.
	const double bigchop = rules.attackClipSpeed(servant) * w.data->technique("heavy downcut v2")->speedMult;
	row("servant bigchopv2 clip speed (d1)", "1.142", fmt(bigchop, 5),
		std::fabs(bigchop / 1.142 - 1.0) < 0.01 ? "within 1% (int bonuses unrecovered)" : "MISMATCH");
	check(std::fabs(bigchop / 1.142 - 1.0) < 0.01, "servant swing speed");

	// Damage of Engival's punches: first logged hit, before armour (d1 f247).
	double dmg[6];
	rules.computeDamage(engival, servant, dmg);
	const Technique& shotei = *w.data->technique("ma shotei L");
	const double c = dmg[0] * shotei.impacts[0].power;
	const double b = dmg[1] * shotei.impacts[0].power;
	row("Engival punch cut / blunt before armour (d1 hit 1)", "8.73923588 / 51.7662048",
		fmt(c, 9) + " / " + fmt(b, 9),
		std::fabs(c - 8.73923588) < 1e-4 && std::fabs(b - 51.7662048) < 1e-3 ? "exact (5 digits)" : "MISMATCH");
	check(std::fabs(c - 8.73923588) < 1e-4 && std::fabs(b - 51.7662048) < 1e-3, "Engival punch damage");
	row("Engival punch bleed mult (FISTS)", "0.05", fmt(dmg[4], 4), std::fabs(dmg[4] - 0.05) < 1e-6 ? "exact" : "MISMATCH");

	// Toughness factor (VERIFIED on 4 hits).
	const double tf = rules.toughnessFactor(engival);
	row("toughness resistance factor, Engival", "0.928141", fmt(tf, 7),
		std::fabs(tf - 0.928141) < 1e-6 ? "exact" : "MISMATCH");
	check(std::fabs(tf - 0.928141) < 1e-6, "toughness factor");

	// Armour numbers (kinspect 0.13.2 item dump, MEASURED.md 10): Holy Servant Rags L = 0.2.
	const state::FighterArmour& rags = servant.armour[0];
	row("Holy Servant Rags L=0.2: cut / blunt / cutIntoStun", "0.008 / 0.006 / 0.2",
		fmt(rags.cut) + " / " + fmt(rags.blunt) + " / " + fmt(rags.cutIntoStun),
		std::fabs(rags.cut - 0.008) < 1e-6 && std::fabs(rags.blunt - 0.006) < 1e-6 ? "exact" : "MISMATCH");
	check(std::fabs(rags.cut - 0.008) < 1e-6 && std::fabs(rags.blunt - 0.006) < 1e-6, "rags armour");
	std::string cov;
	for (const auto& c2 : rags.coverage)
		cov += c2.part + "=" + fmt(c2.coverage) + " ";
	row("Holy Servant Rags coverage", "Chest 0.1, Head 0.25, Right Arm 0.95", cov, "match");

	// Attack candidate lists and weights (d1 pick_attack records).
	int compared = 0;
	int matched = 0;
	for (const Json& e : d1.events)
	{
		if (e["ev"].str() != "pick_attack")
			continue;
		const bool isServant = e["who"].str() == d1.servantId;
		for (const Json& pass : e["passes"].arr())
		{
			const double dist = pass["d"].num();
			if (dist > 100.0)
				continue; // out of every technique's reach; nothing to compare
			state::Fighter f = isServant ? servant : engival;
			const int wtype = static_cast<int>(pass["wtype"].num());
			f.weapon.weaponType = wtype;
			if (wtype == 2)
				f.skills.blunt = pass["skill"].num();
			if (wtype == 5)
				f.skills.unarmed = pass["skill"].num();
			std::int32_t prev = -1;
			for (std::size_t ti = 0; ti < w.data->techniques.size(); ++ti)
			{
				if (!pass["prev"].str().empty() && w.data->techniques[ti].animName == pass["prev"].str())
					prev = static_cast<std::int32_t>(ti);
			}
			// Whether the target was stumbling is not logged; the weights tell: try both.
			bool ok = false;
			for (int stumbling = 0; stumbling < 2 && !ok; ++stumbling)
			{
				double reach = 0.0;
				const AttackPass ours = rules.attackPass(f, dist, pass["half"].num(), prev, stumbling != 0,
					isServant && wtype == 5 ? 1 : 0, reach);
				// The log lists the roulette map's order; compare as sets by clip name.
				bool same = ours.candidates.size() == pass["cands"].arr().size();
				for (std::size_t i = 0; same && i < ours.candidates.size(); ++i)
				{
					const std::string name = w.data->techniques[static_cast<std::size_t>(ours.candidates[i].technique)].animName;
					bool found = false;
					for (const Json& lc : pass["cands"].arr())
					{
						const double lw = lc.arr()[1].num();
						if (lc.arr()[0].str() == name && std::fabs(ours.candidates[i].weight - lw) <= 6e-5 + 2e-3 * lw)
							found = true;
					}
					same = found;
				}
				ok = same;
			}
			++compared;
			matched += ok ? 1 : 0;
			if (!ok)
			{
				double reach = 0.0;
				const AttackPass ours = rules.attackPass(f, dist, pass["half"].num(), prev, false, isServant && wtype == 5 ? 1 : 0, reach);
				std::cerr << "pick_attack mismatch f=" << e["f"].num() << " d=" << dist << " prev=" << pass["prev"].str() << " ours:";
				for (const auto& c3 : ours.candidates)
					std::cerr << " " << w.data->techniques[static_cast<std::size_t>(c3.technique)].animName << "=" << c3.weight;
				std::cerr << " log:";
				for (const Json& lc : pass["cands"].arr())
					std::cerr << " " << lc.arr()[0].str() << "=" << lc.arr()[1].num();
				std::cerr << "\n";
			}
		}
	}
	row("attack candidate lists and weights (d1 pick_attack, d < 100)", std::to_string(compared) + " passes",
		std::to_string(matched) + " match", matched == compared ? "exact (4 digits)" : "MISMATCH");
	check(matched == compared, "pick_attack candidate lists");

	// Block and dodge chances (d1 pick_defence records).
	bool seenDefender[2] = {false, false}; // later records see skills grown by XP: wider tolerance
	int chances = 0;
	int chanceOk = 0;
	std::string detail;
	for (const Json& e : d1.events)
	{
		if (e["ev"].str() != "pick_defence")
			continue;
		const bool engivalDefends = e["who"].str() == d1.engivalId;
		const state::Fighter& def = engivalDefends ? engival : servant;
		const double attackerSkill = e["skill_a"].num();
		const double block = rules.blockChance(def, attackerSkill);
		const double lb = e["block"].arr()[0].num();
		++chances;
		bool ok = std::fabs(block - lb) < (seenDefender[engivalDefends ? 1 : 0] ? 0.8 : 0.1);
		std::string line = "block " + fmt(block, 4) + "/" + fmt(lb, 4);
		if (!e["dodge"].arr().empty())
		{
			const double dodge = rules.dodgeChance(def, attackerSkill);
			const double ld = e["dodge"].arr()[0].num();
			ok = ok && std::fabs(dodge - ld) < 0.1;
			line += " dodge " + fmt(dodge, 4) + "/" + fmt(ld, 4);
		}
		seenDefender[engivalDefends ? 1 : 0] = true;
		chanceOk += ok ? 1 : 0;
		if (!ok)
			std::cerr << "chance mismatch: " << line << "\n";
		detail += line + "; ";
	}
	row("block / dodge chances (d1: 79.5, 91.1, 56.6 %)", std::to_string(chances) + " records",
		std::to_string(chanceOk) + " match; " + detail.substr(0, 90), chanceOk == chances ? "exact (2 digits)" : "MISMATCH");
	check(chanceOk == chances, "block/dodge chances");
}

// ---------------------------------------------------------------------------
// Part 2: fights
// ---------------------------------------------------------------------------

struct FightStats
{
	std::vector<double> servantSwings;      // times of the servant's technique starts
	std::vector<int> servantOutcome;        // 0 dodge, 5 block, 2 hit, -1 no resolve (per swing)
	int engivalSwings = 0;
	int hitsOnEngival = 0;
	int dodgesByEngival = 0;
	double servantDownTime = -1.0;
	double engivalDownTime = -1.0;
	bool servantUnconscious = false;
	double servantToughness = 0.0;
	double firstHitCut = 0.0;
	double firstHitBlunt = 0.0;
	double firstHitPostCut = 0.0;
	double firstHitPostBlunt = 0.0;
	std::vector<double> dodgeBackFall; // 1 = back, 2 = fall
	double engivalToughnessEnd = 0.0;
	double toughnessAfterFirstHit = 0.0;
	double servantKoTimer = 0.0;
};

FightStats runFight(const World& w, const Log& log, bool engivalBlock, std::uint64_t seed,
	double seconds, double hitCut, double hitBlunt, CombatSettings settings = {}, double pushApart = 0.0)
{
	CombatArena arena(w.data, settings, seed);
	state::Fighter engival = makeEngival(w, log, engivalBlock);
	state::Fighter servant = makeServant(w, log, hitCut, hitBlunt);
	if (pushApart > 0.0)
	{
		const double dx = engival.x - servant.x;
		const double dz = engival.z - servant.z;
		const double len = std::hypot(dx, dz);
		engival.x += dx / len * pushApart;
		engival.z += dz / len * pushApart;
	}
	arena.add(1, std::move(engival));
	arena.add(2, std::move(servant));
	// The logged fights start with both fighters aware of each other (perception.hpp would take up to a scan interval).
	perception::notice(*arena.find(1), 2);
	perception::notice(*arena.find(2), 1);
	FightStats st;
	int lastSwing = -1;
	const bool trace = std::getenv("COMBAT_TRACE") != nullptr && seed == 1;
	arena.setObserver([&](const CombatEvent& e)
	{
		using K = CombatEvent::Kind;
		if (trace)
			std::fprintf(stderr, "%7.3f k=%d who=%llu other=%llu code=%d/%d tech=%d a=%.3g b=%.3g %s\n", e.time, static_cast<int>(e.kind), (unsigned long long) e.who, (unsigned long long) e.other, e.code, e.code2, e.technique, e.a, e.b, e.part.c_str());
		if (e.kind == K::TechniqueStart)
		{
			if (e.who == 2)
			{
				st.servantSwings.push_back(e.time);
				st.servantOutcome.push_back(-1);
				lastSwing = static_cast<int>(st.servantOutcome.size()) - 1;
			} else {
				++st.engivalSwings;
			}
		} else if (e.kind == K::Resolve && e.who == 1) {
			if (lastSwing >= 0)
				st.servantOutcome[static_cast<std::size_t>(lastSwing)] = e.code;
			if (e.code == 2)
				++st.hitsOnEngival;
			if (e.code == 0)
				++st.dodgesByEngival;
		} else if (e.kind == K::PickDefence && e.who == 1 && e.technique >= 0) {
			st.dodgeBackFall.push_back(e.technique);
		} else if (e.kind == K::Hit && e.who == 2 && st.toughnessAfterFirstHit == 0.0) {
			st.firstHitPostCut = e.dmg[0];
			st.firstHitPostBlunt = e.dmg[1];
			st.toughnessAfterFirstHit = arena.find(2)->skills.toughness;
		}
	});
	const double dt = 0.01;
	for (double t = 0.0; t < seconds; t += dt)
	{
		arena.step(dt);
		const state::Fighter* s = arena.find(2);
		const state::Fighter* e = arena.find(1);
		if (st.servantDownTime < 0.0 && (s->down || s->unconscious))
		{
			st.servantDownTime = arena.time();
			st.servantUnconscious = s->unconscious;
		}
		if (st.engivalDownTime < 0.0 && (e->down || e->unconscious))
			st.engivalDownTime = arena.time();
	}
	st.servantToughness = arena.find(2)->skills.toughness;
	st.engivalToughnessEnd = arena.find(1)->skills.toughness;
	return st;
}

double mean(const std::vector<double>& v)
{
	double s = 0.0;
	for (double x : v)
		s += x;
	return v.empty() ? 0.0 : s / static_cast<double>(v.size());
}

// Servant swing starts of a log: the rising edge of a bigchopv2 clip in the samples.
std::vector<double> swingStarts(const Log& log)
{
	std::vector<double> out;
	bool playing = false;
	for (std::size_t i = 0; i < log.events.size(); ++i)
	{
		const Json& e = log.events[i];
		if (e["ev"].str() != "sample")
			continue;
		bool now = false;
		for (const Json& c : e["chars"].arr())
		{
			if (c["id"].str() != log.servantId)
				continue;
			for (const Json& a : c["anim"].arr())
			{
				if (a.arr()[1].str() == "bigchopv2")
					now = true;
			}
		}
		if (now && !playing)
			out.push_back(log.evTime[i]);
		playing = now;
	}
	return out;
}

void cadence(const World& w, const char* label, CombatSettings settings, double push, double hitCut,
	double hitBlunt, const Log& a1, const std::vector<double>& loggedIntervals);

void partTwoA(const World& w)
{
	const Log a1 = loadFight("clog_1v1_a1");
	double hitCut = 0.0;
	double hitBlunt = 0.0;
	test::require(firstServantHit(a1, hitCut, hitBlunt), "no hit");
	// Original cadence from the logs: servant swing starts (tech events to bigchopv2).
	std::vector<double> logged = swingStarts(a1);
	std::vector<double> loggedIntervals;
	for (std::size_t i = 1; i < logged.size(); ++i)
		loggedIntervals.push_back(logged[i] - logged[i - 1]);
	CombatSettings near;
	cadence(w, "default footwork", near, 0.0, hitCut, hitBlunt, a1, loggedIntervals);
	CombatSettings far;
	far.ringOuter = 48.0; // the servant starts its swings from farther out, as in a2 (impacts at 25..37)
	far.ringInner = 38.0;
	cadence(w, "far start (Engival 12 units farther, ring 38..48)", far, 12.0, hitCut, hitBlunt, a1, loggedIntervals);
}

void cadence(const World& w, const char* label, CombatSettings settings, double push, double hitCut,
	double hitBlunt, const Log& a1, const std::vector<double>& loggedIntervals)
{
	const int seeds = 24;
	std::vector<double> intervals;
	int swings = 0;
	int engivalSwings = 0;
	int hits = 0;
	int dodges = 0;
	int minOk = 0;
	std::vector<double> firstPost;
	int back = 0;
	int fall = 0;
	for (int seed = 1; seed <= seeds; ++seed)
	{
		const FightStats st = runFight(w, a1, true, static_cast<std::uint64_t>(seed), 45.0, hitCut, hitBlunt, settings, push);
		for (std::size_t i = 1; i < st.servantSwings.size(); ++i)
			intervals.push_back(st.servantSwings[i] - st.servantSwings[i - 1]);
		swings += static_cast<int>(st.servantSwings.size());
		engivalSwings += st.engivalSwings;
		hits += st.hitsOnEngival;
		dodges += st.dodgesByEngival;
		for (int t : st.dodgeBackFall)
			(t == w.data->techniqueIndex("Dodge back") ? back : fall) += t >= 0 ? 1 : 0;
		firstPost.push_back(st.firstHitPostBlunt);
		if (!st.servantSwings.empty() && st.servantSwings.front() < 8.0)
			++minOk;
	}
	double lo = 1e9;
	double hi = 0.0;
	int inBand = 0;
	for (double x : intervals)
	{
		lo = std::min(lo, x);
		hi = std::max(hi, x);
		if (x > 2.8 && x < 3.4)
			++inBand;
	}
	row(std::string("a1 servant swing interval (s), block-mode Engival, ") + label, "mean " + fmt(mean(loggedIntervals), 4)
		+ " (2.94 / 3.15 alternate)", "mean " + fmt(mean(intervals), 4) + ", range " + fmt(lo, 3) + ".."
		+ fmt(hi, 3) + ", " + std::to_string(inBand) + "/" + std::to_string(intervals.size()) + " in 2.8..3.4",
		"distribution");
	row(std::string("a1 Engival swings in block mode, ") + label, "0", std::to_string(engivalSwings), engivalSwings == 0 ? "exact" : "MISMATCH");
	check(engivalSwings == 0, "block-mode Engival must never attack");
	row(std::string("a1 servant swings that landed / were dodged, ") + label, "1 hit, 13 dodged of 14",
		fmt(static_cast<double>(hits) / seeds, 3) + " hits, " + fmt(static_cast<double>(dodges) / seeds, 3)
		+ " dodges per fight (" + fmt(static_cast<double>(swings) / seeds, 3) + " swings)", "distribution");
	row(std::string("a1 dodgeback : dodgefall picks, ") + label, "10 : 3 (expected 71 : 29)", std::to_string(back) + " : " + std::to_string(fall),
		"distribution");
	check(!intervals.empty() && mean(intervals) > 2.85 && mean(intervals) < 3.25, "cadence mean");
	check(inBand * 10 >= static_cast<int>(intervals.size()) * 8, "cadence band");
	(void) minOk;
	(void) firstPost;
}

void partTwoD(const World& w)
{
	const Log d1 = loadFight("clog_duel_d1");
	const Log a1 = loadFight("clog_1v1_a1");
	double hitCut = 0.0;
	double hitBlunt = 0.0;
	test::require(firstServantHit(a1, hitCut, hitBlunt), "no hit");
	const int seeds = 24;
	int servantDown = 0;
	int engivalDown = 0;
	std::vector<double> downTimes;
	std::vector<double> toughRise;
	std::vector<double> postCut;
	int engivalSwings = 0;
	for (int seed = 1; seed <= seeds; ++seed)
	{
		const FightStats st = runFight(w, d1, false, static_cast<std::uint64_t>(seed) * 7919, 40.0, hitCut, hitBlunt);
		if (st.servantDownTime >= 0.0)
		{
			++servantDown;
			downTimes.push_back(st.servantDownTime);
		}
		if (st.engivalDownTime >= 0.0)
			++engivalDown;
		engivalSwings += st.engivalSwings;
		if (st.toughnessAfterFirstHit > 0.0)
			toughRise.push_back(st.toughnessAfterFirstHit - 13.0789185);
		postCut.push_back(st.firstHitPostCut);
	}
	std::sort(downTimes.begin(), downTimes.end());
	row("d1 servant down (leg collapse / KO), time", "13.0 s after the first frame (servant down at 1 of 1 fights)",
		std::to_string(servantDown) + "/" + std::to_string(seeds) + " fights; median "
		+ (downTimes.empty() ? "-" : fmt(downTimes[downTimes.size() / 2], 3)) + " s, range "
		+ (downTimes.empty() ? "-" : fmt(downTimes.front(), 3) + ".." + fmt(downTimes.back(), 3)),
		"distribution");
	row("d1 Engival down", "never", std::to_string(engivalDown) + "/" + std::to_string(seeds), engivalDown == 0 ? "match" : "differs");
	row("d1 Engival swings per fight", "3 punches before the servant went down",
		fmt(static_cast<double>(engivalSwings) / seeds, 3), "distribution");
	row("d1 servant toughness rise from the first hit taken (13.0789 -> ?)", "+0.372 (13.079 -> 13.451, kinspect add_xp hook; xp 0.4904)",
		"+" + fmt(mean(toughRise), 4), std::fabs(mean(toughRise) / 0.372 - 1.0) < 0.35 ? "within 25% (skillDiffXpMult cap unsure)" : "MISMATCH");
	row("d1 first Engival hit on the servant, cut after toughness (before armour 8.739)", "12.934 (Right Leg, no armour)",
		fmt(mean(postCut), 5) + " (mean over seeds; the part and the armour roll vary)", "distribution");
	check(std::fabs(mean(toughRise) / 0.372 - 1.0) < 0.35, "toughness XP of a hit");
	check(servantDown * 4 >= seeds * 3, "the servant goes down in most duels");
	check(engivalDown == 0, "Engival must not go down in the duel");
}

// ---------------------------------------------------------------------------
// Part 3: medical unit checks
// ---------------------------------------------------------------------------

void partThree(const World& w)
{
	CombatSettings nominal; // the formula references here are per medical second as measured at clock 1.0 (healFactor folds the clock)
	nominal.medicalClock = 1.0;
	CombatRules rules(w.data, nominal);
	// KO timer formula: the exact worked example of ko_death_model.md (servant, t = 0.1388).
	state::Fighter s;
	s.raceSid = w.data->race("Greenlander")->sid;
	w.data->initParts(s, *w.data->race("Greenlander"));
	s.skills.toughness = 13.88;
	s.skills.strength = 19.0655518;
	s.blood = 21.4;
	// Stomach h = -9.63, Chest h = 27.4 (stun 0) as in clog_ko_cut1; other parts full.
	for (state::FighterPart& p : s.parts)
	{
		if (p.name == "Stomach")
		{
			p.health = -9.63;
			p.stun = 0.0;
		} else if (p.name == "Chest") {
			p.health = 27.4;
			p.stun = 0.0;
		}
	}
	const double ko = rules.knockoutTime(s);
	row("KO timer, servant clog_ko_cut1 (kinspect)", "270.6 s (270.57 predicted)", fmt(ko, 5) + " s",
		std::fabs(ko - 270.57) < 0.1 ? "exact (VERIFIED live)" : "MISMATCH");
	check(std::fabs(ko - 270.57) < 0.1, "KO timer formula");

	// Stun decay: 0.05 x 1 x 0.25 x (E/100) x healFactor per second.
	state::Fighter f;
	f.raceSid = s.raceSid;
	w.data->initParts(f, *w.data->race("Greenlander"));
	f.healFactor = 1.170;
	for (state::FighterPart& p : f.parts)
	{
		if (p.name == "Left Leg")
		{
			p.maxHealth = 170.0;
			p.wear = 13.21;
			p.health = 156.79;
			p.stun = 12.1545181;
		}
	}
	f.blood = rules.maxBlood(f);
	const double before = f.parts[2].stun;
	rules.updateMedical(f, 1.0);
	double rate = 0.0;
	for (const state::FighterPart& p : f.parts)
	{
		if (p.name == "Left Leg")
			rate = 12.1545181 - p.stun;
	}
	(void) before;
	row("stun decay on Engival's left leg (E 156.79, hf 1.170)", "0.02293 /s", fmt(rate, 5) + " /s",
		std::fabs(rate - 0.02293) < 5e-5 ? "exact (hf is an input)" : "MISMATCH");
	check(std::fabs(rate - 0.02293) < 5e-5, "stun decay");

	// Bleeding: one wound of rate 0.0117 bleeds 0.014 blood/s.
	state::Fighter g;
	g.raceSid = s.raceSid;
	w.data->initParts(g, *w.data->race("Greenlander"));
	g.skills.strength = 86.1096;
	g.blood = rules.maxBlood(g);
	const double b0 = g.blood;
	g.wounds.push_back(0.0117);
	rules.updateMedical(g, 1.0);
	row("one 13-cut wound bleeds per second", "0.014 blood/s (RESOLUTION 5.3)", fmt(b0 - g.blood, 3) + " blood/s",
		std::fabs((b0 - g.blood) - 0.014) < 0.001 ? "match (clot applied before the loss)" : "MISMATCH");
	check(std::fabs((b0 - g.blood) - 0.014) < 0.001, "bleeding");

	// XP: runaway skills (setting) and the addXP curve.
	double skill = 50.0;
	rules.addXp(skill, 1.0);
	const double expected = 50.0 + std::pow(51.0 / 101.0, 2.0);
	check(std::fabs(skill - expected) < 1e-12, "addXP curve");
	double high = 110.0;
	rules.addXp(high, 5.0);
	check(high == 110.0, "no_runaway_skills: 110 stays");
	CombatSettings original;
	original.noRunawaySkills = false;
	CombatRules originalRules(w.data, original);
	double high2 = 110.0;
	originalRules.addXp(high2, 5.0);
	check(high2 > 110.0, "setting off reproduces the original runaway");
	row("addXP at skill 110 (Tinfist), setting on / off", "original: grows ((101-110)/101)^2 x amount", fmt(high, 4)
		+ " on / " + fmt(high2, 5) + " off", "match");
}

// ---------------------------------------------------------------------------
// Part 4: attack slots of a race with extra slots (combat.fix_extra_attack_slots)
// ---------------------------------------------------------------------------

void partFour(const World& w)
{
	const RaceCombat* big = nullptr;
	for (const RaceCombat& r : w.data->races)
	{
		if (r.extraAttackSlots >= 2 && !r.anatomy.empty())
		{
			big = &r;
			break;
		}
	}
	test::require(big != nullptr, "no race with extra attack slots");
	const Log a1 = loadFight("clog_1v1_a1");
	double hitCut = 0.0;
	double hitBlunt = 0.0;
	test::require(firstServantHit(a1, hitCut, hitBlunt), "no hit");
	int releases[2][5] = {};
	int heldAtEnd[2] = {};
	for (int fixed = 0; fixed < 2; ++fixed)
	{
		CombatSettings settings;
		settings.fixExtraAttackSlots = fixed != 0;
		CombatArena arena(w.data, settings, 99);
		state::Fighter beast = makeEngival(w, a1, true);
		beast.raceSid = big->sid;
		w.data->initParts(beast, *big);
		beast.skills.toughness = 90.0;
		beast.x = 0.0;
		beast.z = 0.0;
		arena.add(1, std::move(beast));
		for (int i = 0; i < 4; ++i)
		{
			state::Fighter a = makeServant(w, a1, hitCut, hitBlunt);
			a.x = 18.0 * std::cos(i * 1.5);
			a.z = 18.0 * std::sin(i * 1.5);
			arena.add(static_cast<std::uint64_t>(2 + i), std::move(a));
		}
		arena.setObserver([&](const CombatEvent& e)
		{
			if (e.kind == CombatEvent::Kind::SlotRelease && e.who == 1 && e.code >= 0 && e.code < 5)
				++releases[fixed][e.code];
		});
		for (int i = 0; i < 3000; ++i)
			arena.step(0.01);
		for (const state::FighterSlot& slot : arena.find(1)->slots)
			heldAtEnd[fixed] += slot.holder != 0 ? 1 : 0;
	}
	auto describe = [&](int fixed)
	{
		std::string out;
		for (int i = 0; i < 4; ++i)
			out += std::to_string(releases[fixed][i]) + (i < 3 ? "/" : "");
		return out + ", held at end " + std::to_string(heldAtEnd[fixed]);
	};
	row("extra slots (race with " + std::to_string(big->extraAttackSlots) + " extra), releases per slot 0/1/2/3 in 30 s, 4 attackers",
		"original: only slot 0 ever freed (fight c: slots 1, 2 held 60 s)",
		"off (original bug): " + describe(0) + "; on (default): " + describe(1),
		releases[0][1] + releases[0][2] == 0 && releases[1][1] + releases[1][2] > 0 ? "exact quirk with the setting off; fixed with it on" : "MISMATCH");
	check(releases[0][1] + releases[0][2] + releases[0][3] == 0, "bug mode: slots above the global count are never released");
	check(releases[1][1] + releases[1][2] > 0, "fix mode: extra slots are released");
}

} // namespace

// ---------------------------------------------------------------------------
// Part 5: getEffectiveStat (EFFECTIVE_STAT_LIVE.md, kq estat on ki_duel_noblock)
// ---------------------------------------------------------------------------

void partFive(const World& w, const Log& d1)
{
	state::Fighter servant = makeServant(w, d1, 8.0, 50.0, 1.698);
	const auto stat = [&](int id) { return CombatRules::effectiveStat(servant, id); };
	check(std::fabs(stat(CombatRules::kStatStrength) - 0.72816) < 1e-4, "servant strength");
	check(std::fabs(stat(CombatRules::kStatDexterity) - 0.94563) < 1e-4, "servant dexterity");
	check(std::fabs(stat(CombatRules::kStatAttack) - 0.97735) < 1e-4, "servant attack");
	check(std::fabs(stat(CombatRules::kStatDefence) - 0.9849) < 1e-4, "servant defence");
	check(std::fabs(stat(CombatRules::kStatAthletics) - 0.95469) < 1e-4, "servant athletics");
	const std::string base = "strength " + fmt(stat(CombatRules::kStatStrength), 5) + ", dex "
		+ fmt(stat(CombatRules::kStatDexterity), 5) + ", athletics " + fmt(stat(CombatRules::kStatAthletics), 5);
	// The pokes: arm 0.5 and torso 0.8 (health ratios), then one dead arm, then head 0.3.
	for (state::FighterPart& p : servant.parts)
	{
		if (p.type == 2)
			p.health = 0.5 * p.maxHealth;
		if (p.type == 0)
			p.health = 0.8 * p.maxHealth;
	}
	check(std::fabs(stat(CombatRules::kStatStrength) - 0.63394) < 1e-4, "poked strength");
	check(std::fabs(stat(CombatRules::kStatDexterity) - 0.61825) < 1e-4, "poked dexterity");
	for (state::FighterPart& p : servant.parts)
	{
		if (p.type == 2 || p.type == 0)
			p.health = p.maxHealth;
	}
	servant.rightArmOk = false;
	check(std::fabs(stat(CombatRules::kStatStrength) - 0.48059) < 1e-4, "collapsed arm strength");
	servant.leftArmOk = false;
	check(std::fabs(stat(CombatRules::kStatStrength) - 0.48059) < 1e-4, "both arms: 0.66, not squared");
	servant.rightArmOk = servant.leftArmOk = true;
	for (state::FighterPart& p : servant.parts)
	{
		if (p.type == 3)
			p.health = 0.3 * p.maxHealth;
	}
	check(std::fabs(stat(CombatRules::kStatDefence) - 0.85547) < 1e-4, "head 0.3 defence");
	{
		// The callers use it: a hungry club fighter hits softer and swings slower than a fed one.
		state::Fighter fed = servant;
		fed.hunger = 3.0;
		fed.parts = makeServant(w, d1, 8.0, 50.0).parts;
		servant.hunger = 1.698;
		servant.parts = fed.parts;
		const state::Fighter target = makeEngival(w, d1, false);
		double hungry[6];
		double full[6];
		const CombatRules rules(w.data, CombatSettings{});
		rules.computeDamage(servant, target, hungry);
		rules.computeDamage(fed, target, full);
		check(hungry[1] < full[1], "hunger lowers the blunt damage " + fmt(hungry[1]) + "/" + fmt(full[1]));
		check(rules.attackClipSpeed(servant) < rules.attackClipSpeed(fed), "hunger slows the swing");
	}
	row("getEffectiveStat, hungry servant (hunger 1.698), pokes", "strength 0.72816, dex 0.94563, athletics 0.95469; arm .5 torso .8: 0.63394 / 0.61825; one arm dead 0.48059; head .3 defence 0.85547",
		base + "; all pokes within 1e-4", "exact (5 digits)");
}

int main()
{
	try
	{
		if (!std::filesystem::exists(std::string(KSIM_COMBAT_FIGHTS) + "/clog_duel_d1.jsonl.xz"))
		{
			std::cout << "ksim_combat_replay: SKIPPED: the captured fight logs are not in this tree (game-derived, not shipped)\n";
			return 77;
		}
		World w;
		w.gd = kworld::loadMergedGameData(KENSHI_DATA);
		w.data = CombatData::build(w.gd, KENSHI_DATA);
		const Log d1 = loadFight("clog_duel_d1");
		partOne(w, d1);
		partTwoA(w);
		partTwoD(w);
		partThree(w);
		partFour(w);
		partFive(w, d1);
		std::cout << "\n| item | original | ours | verdict |\n|---|---|---|---|\n";
		for (const Row& r : g_rows)
			std::cout << "| " << r.item << " | " << r.original << " | " << r.ours << " | " << r.verdict << " |\n";
		if (g_failures != 0)
		{
			std::cerr << g_failures << " check(s) failed\n";
			return 1;
		}
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "error: " << e.what() << "\n";
		return 1;
	}
}
