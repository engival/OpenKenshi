// Hunger, bleeding, first aid, degeneration and the KO wake against docs/reference/MEDICAL_HUNGER_LIVE.md. The numbers
// checked are the live ones: hunger per game hour M / T, the bandage pool rate for skill 19.33 and a quality-5 kit
// (5.06 / s, live 5.06), the kit drain 1.63 / s (live 1.65), the order head, chest, arm, clotting at the data's rate.

#include "ksim/core/simulation.hpp"

#include "kworld/gamedata.hpp"

#include "test_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;

const kdata::Record* recordNamed(const kdata::GameData& gd, const char* type, const char* name)
{
	for (const kdata::Record* r : gd.byType(kworld::itemTypeByName(type)))
	{
		if (r->name == name)
			return r;
	}
	throw std::runtime_error(std::string("no ") + type + " " + name);
}

state::FighterPart* part(state::Fighter& f, const char* name)
{
	for (state::FighterPart& p : f.parts)
	{
		if (p.name == name)
			return &p;
	}
	throw std::runtime_error(std::string("no part ") + name);
}

const state::Fighter& fighterOf(const Simulation& sim, std::uint64_t id)
{
	const state::Fighter* f = sim.combat()->find(id);
	test::require(f != nullptr, "fighter missing");
	return *f;
}

void near(double got, double want, double tol, const char* what)
{
	if (std::fabs(got - want) > tol)
	{
		char b[160];
		std::snprintf(b, sizeof b, "%s: %.8g, wanted %.8g +- %g", what, got, want, tol);
		throw test::Failure(b);
	}
	std::printf("ok  %-52s %.8g\n", what, got);
}

} // namespace

int main()
{
	try
	{
		const kdata::GameData gd = kworld::loadMergedGameData(KENSHI_DATA);
		const std::shared_ptr<const CombatData> data = CombatData::build(gd, KENSHI_DATA);
		const CombatConstants& k = data->k;
		const kdata::Record* servant = recordNamed(gd, "CHARACTER", "Escaped Servant");
		const std::string bread = recordNamed(gd, "ITEM", "Bread")->stringID;
		const std::string basicKit = recordNamed(gd, "ITEM", "Basic First Aid Kit")->stringID;
		std::string robotKit;
		for (const auto& [sid, info] : data->items)
		{
			if (info.function == kItemFunctionRobotKit)
				robotKit = sid;
		}
		test::require(!robotKit.empty(), "no robotic kit item in the data");
		// The organic kit with the most charges (the Advanced one): carried FIRST below, it must still lose to the Basic kit.
		std::string bigKit;
		for (const auto& [sid, info] : data->items)
		{
			if (info.function == kItemFunctionMedkit && sid != basicKit && (bigKit.empty() || info.charges > data->item(bigKit)->charges))
				bigKit = sid;
		}
		test::require(!bigKit.empty() && data->item(bigKit)->charges > data->item(basicKit)->charges, "no organic kit with more charges than the Basic one");

		std::printf("constants: medic speed %g, robot %g, kit drain %g/%g, degen %g/%g, starvation %g h, bed %g, enc %g, clot %g, xpMedic %g/%g\n", k.medicSpeed, k.robotMedicSpeed,
			k.medkitDrain1, k.medkitDrain99, k.degenerationMult1, k.degenerationMult99, k.starvationHours, k.bedHungerRate, k.encumbranceHungerRate, k.bleedClotRate, k.xpMedic1, k.xpMedic99);
		Simulation sim = Simulation::load(KENSHI_DATA, 5, RegionConfig{});
		CombatSettings settings;
		settings.medicalClock = 1.0; // the rate references below are per sim second at the nominal clock; the clock block further down varies it
		sim.enableCombat(data, settings);
		std::uint64_t rng = 99;
		const auto spawn = [&](double x, double hunger, bool player)
		{
			state::Fighter f = data->makeFighter(gd, servant->stringID, "", rng);
			f.x = x;
			f.z = 0.0;
			f.hunger = hunger;
			f.playerControlled = player;
			f.skills.toughness = 50.0;
			return sim.spawnFighter(std::move(f)).get<state::Id>()->value;
		};
		const auto entityOf = [&](std::uint64_t id)
		{
			flecs::entity found;
			sim.world().ecs().each([&](flecs::entity e, const state::Id& i, const state::Fighter&) { if (i.value == id) found = e; });
			return found;
		};
		const double hourTicks = static_cast<double>(kTicksPerGameHour);
		const double T = k.starvationHours * settings.hungerTime;

		// ---- hunger: standing, walking, encumbered, KO'd ----
		const std::uint64_t idle = spawn(0, 3.0, false);
		const std::uint64_t ticks = 1200; // a minute of play: 0.55 game hours
		sim.run(1);
		const std::uint64_t firstStamp = fighterOf(sim, idle).hungerStep; // the arena step the 1 s hunger slice counts from
		const double firstHunger = fighterOf(sim, idle).hunger;
		sim.run(ticks - 1);
		// Hunger runs once a second (k16.6.637): the loss is the time up to the member's last slice, which the stamp says (arena steps of 1/substeps tick).
		const double hours = static_cast<double>(fighterOf(sim, idle).hungerStep - firstStamp) * (Simulation::kTickSeconds / std::max(1, settings.substeps)) / (hourTicks * Simulation::kTickSeconds);
		near((firstHunger - fighterOf(sim, idle).hunger) / hours, 1.0 / T, 2e-5, "hunger / game hour, standing (M 1, T 96 h)");
		test::require(!fighterOf(sim, idle).unconscious, "a fed fighter is awake");

		{
			CombatRules rulesOnly(data, settings);
			state::Fighter f = data->makeFighter(gd, servant->stringID, "", rng);
			near(rulesOnly.hungerUse(f), 1.0, 1e-12, "M standing");
			f.forwardSpeed = 10.0;
			near(rulesOnly.hungerUse(f), 1.0, 1e-12, "M moving, unencumbered");
			f.encumbrance = 0.5;
			near(rulesOnly.hungerUse(f), 1.0 + 0.5 * k.encumbranceHungerRate, 1e-12, "M moving at encumbrance 0.5");
			f.forwardSpeed = 0.0;
			f.target = 7;
			near(rulesOnly.hungerUse(f), 1.0 + 0.5 * k.encumbranceHungerRate, 1e-12, "M in combat at encumbrance 0.5");
			f.target = 0;
			f.unconscious = true;
			near(rulesOnly.hungerUse(f), k.bedHungerRate, 1e-12, "M KO'd (bed rate)");
		}

		// ---- thresholds: KO below 0.1 (held until fed), death below 0 ----
		const std::uint64_t faint = spawn(400, 0.1001, false);
		const std::uint64_t dying = spawn(500, 0.0001, false);
		sim.run(200);
		test::require(fighterOf(sim, faint).unconscious && fighterOf(sim, faint).hunger < 0.1, "hunger below 0.1 knocks out");
		const double timer = fighterOf(sim, faint).koTimer;
		test::require(timer > 0.0 || fighterOf(sim, faint).hungerKo, "the hunger KO timer ran");
		test::require(fighterOf(sim, dying).dead, "hunger below 0 kills");
		std::printf("ok  hunger KO below 0.1 (timer left %.1f s), death below 0\n", timer);
		sim.run(1600); // the timer (up to ~105 s) runs out; hunger is still < 0.1: the KO holds
		test::require(fighterOf(sim, faint).unconscious, "hunger KO holds after the timer");

		// ---- eating: a hungry AI character with Bread eats it ----
		const std::uint64_t hungry = spawn(700, 2.0, false);
		{
			flecs::entity e = entityOf(hungry);
			state::Inventory inv = *e.get<state::Inventory>();
			inv.add(bread, 2);
			e.set<state::Inventory>(inv);
		}
		sim.run(80);
		test::require(fighterOf(sim, hungry).hunger > 2.9, "a hungry AI character with Bread eats up to fed");
		near(fighterOf(sim, hungry).hunger, 3.0, 0.02, "hunger after one Bread (2 bites of 0.5, UNSURE units)");
		test::require(entityOf(hungry).get<state::Inventory>()->carried(bread) == 1, "one Bread was eaten");
		const std::uint64_t peckish = spawn(800, 2.2, true); // a player's character starts at 2.1, not 2.2
		{
			flecs::entity e = entityOf(peckish);
			state::Inventory inv = *e.get<state::Inventory>();
			inv.add(bread, 1);
			e.set<state::Inventory>(inv);
		}
		sim.run(40);
		test::require(fighterOf(sim, peckish).hunger < 2.2 && entityOf(peckish).get<state::Inventory>()->carried(bread) == 1,
			"a player's character does not eat at 2.2");

		// ---- bleeding and blood (rules level) ----
		CombatRules rules(data, settings);
		{
			state::Fighter f = data->makeFighter(gd, servant->stringID, "", rng);
			f.wounds = {0.0366, 0.0100};
			const double blood0 = f.blood;
			rules.updateMedical(f, 1.0);
			near(f.bleeding, 1.2 * (0.0366 + 0.0100 - 2 * k.bleedClotRate), 1e-9, "bleed rate = 1.2 x sum of wounds");
			near(blood0 - f.blood, f.bleeding, 1e-9, "blood falls by the bleed rate per second");
			near(k.bleedClotRate, 0.00085, 1e-6, "clot rate of the merged data (live 0.00085)");
			for (int i = 0; i < 100; ++i)
				rules.updateMedical(f, 1.0);
			test::require(f.wounds.empty(), "wounds clot away (a 30-point cut in about 43 s)");
			const double low = rules.maxBlood(f) - 10.0;
			f.blood = low;
			rules.updateMedical(f, 1.0);
			near(f.blood - low, k.bloodRecoveryRate * f.bloodFactor, 1e-9, "blood regenerates 0.04 / s once nothing bleeds");
		}

		// ---- clotting is per wound (MEDICAL_HUNGER_LIVE (b2), kinspect 32): every open wound loses 0.00085 per medical second ----
		{
			const double c = k.bleedClotRate;
			const auto rateAfter = [&](state::Fighter& f, double seconds) {
				for (double t = 0.0; t < seconds - 1e-9; t += 1.0)
					rules.updateMedical(f, 1.0);
			};
			for (int state = 0; state < 3; ++state) // standing, KO'd, moving: no dependence
			{
				state::Fighter f = data->makeFighter(gd, servant->stringID, "", rng);
				f.wounds = {0.3};
				f.unconscious = state == 1;
				f.koTimer = state == 1 ? 1.0e6 : 0.0;
				f.forwardSpeed = state == 2 ? 3.0 : 0.0;
				rules.updateMedical(f, 1.0);
				const double r0 = f.bleeding;
				rateAfter(f, 10.0);
				near((r0 - f.bleeding) / 10.0, 1.2 * c, 1e-9, "one wound of 0.3: the rate falls 1.2 x 0.00085 per s (any state)");
				rateAfter(f, 0.3 / c - 11.0 + 2.0);
				test::require(f.wounds.empty(), "one wound of 0.3 is gone after 0.3 / 0.00085 s");
			}
			state::Fighter one = data->makeFighter(gd, servant->stringID, "", rng);
			one.wounds = {0.3};
			rateAfter(one, std::floor(0.3 / c) - 1.0);
			test::require(!one.wounds.empty() && one.bleeding > 0.0, "and still bleeds just before that");
			// 9 wounds: n x 1.2 x 0.00085 per s while all are open, bleeding ends with the largest.
			state::Fighter nine = data->makeFighter(gd, servant->stringID, "", rng);
			nine.wounds = {0.013, 0.02, 0.025, 0.03, 0.035, 0.04, 0.05, 0.06, 0.07};
			rules.updateMedical(nine, 1.0);
			const double r1 = nine.bleeding;
			rules.updateMedical(nine, 1.0);
			near(r1 - nine.bleeding, 9 * 1.2 * c, 1e-9, "9 wounds: the rate falls n x 1.2 x 0.00085 per s");
			for (double t = 2.0; t < 0.07 / c - 2.0; t += 1.0)
				rules.updateMedical(nine, 1.0);
			test::require(nine.bleeding > 0.0 && nine.wounds.size() == 1, "9 wounds: only the largest is left near the end");
			rateAfter(nine, 4.0);
			test::require(nine.bleeding == 0.0 && nine.wounds.empty(), "9 wounds: bleeding ends when the largest does (0.07 / 0.00085 s)");
		}

		// ---- first aid treats the newest wound only; bandaging waits for an empty wound list ((c), FUN_14064d800) ----
		{
			state::Fighter f = data->makeFighter(gd, servant->stringID, "", rng);
			f.wounds = {0.05, 0.04, 0.03};
			const double extra = (0.0005 + 0.05 * (0.03 - 0.0005)) * 2.0 * k.medicSpeed * 0.05;
			rules.clotWounds(f, 5.0, 100.0, false, 0.05);
			near(f.wounds[0], 0.05, 1e-12, "first aid leaves the oldest wound alone");
			near(f.wounds[1], 0.04, 1e-12, "first aid leaves the middle wound alone");
			near(f.wounds[2], 0.03 - extra, 1e-12, "first aid treats the newest wound: 3 x 2 x lerp(f, 0.0005, 0.03) x dt");
			for (int i = 0; i < 100000 && f.wounds.size() == 3; ++i)
				rules.clotWounds(f, 5.0, 100.0, false, 0.05);
			test::require(f.wounds.size() == 2 && f.wounds[1] == 0.04, "the newest is removed first, then the next newest");
		}

		// ---- degeneration, and no natural regeneration ----
		{
			state::Fighter f = data->makeFighter(gd, servant->stringID, "", rng);
			state::FighterPart* chest = part(f, "Chest");
			chest->health = chest->maxHealth * 0.5;
			const double before = chest->health;
			for (int i = 0; i < 600; ++i)
				rules.updateMedical(f, 1.0);
			test::require(chest->health < before - 1.0, "an untreated part over 20 % damage degenerates");
			std::printf("ok  chest 50 %% damaged loses %.2f HP in 10 min untreated\n", before - chest->health);
			state::Fighter g = data->makeFighter(gd, servant->stringID, "", rng);
			state::FighterPart* arm = part(g, "Left Arm");
			arm->health = arm->maxHealth * 0.9; // 10 % damage: under the 20 % line
			for (int i = 0; i < 600; ++i)
				rules.updateMedical(g, 1.0);
			test::require(arm->health == arm->maxHealth * 0.9, "a part under 20 % damage neither heals nor degenerates");
			// The pool converts at 0.05 x heal rate mult x H per second: 0.0125 / s on a part with no wear.
			state::Fighter h = data->makeFighter(gd, servant->stringID, "", rng);
			state::FighterPart* leg = part(h, "Left Leg");
			leg->health = leg->maxHealth * 0.5;
			leg->pool = leg->maxHealth - leg->health; // fully bandaged: no degeneration
			const double l0 = leg->health;
			rules.updateMedical(h, 1.0);
			near(leg->health - l0, 0.05 * k.healRateMult * leg->maxHealth / 100.0, 1e-3, "pool converts at 0.0125 HP / s (0.05 x 0.25 x H)");
		}

		// ---- KO wake ----
		{
			state::Fighter f = data->makeFighter(gd, servant->stringID, "", rng);
			f.unconscious = true;
			f.koTimer = 5.0;
			for (int i = 0; i < 4; ++i)
				rules.updateMedical(f, 1.0);
			test::require(f.unconscious, "still down with 1 s on the KO timer");
			rules.updateMedical(f, 1.5);
			test::require(!f.unconscious, "wakes when the KO timer runs out");
			std::printf("ok  KO wake at timer 0\n");
		}

		// ---- the medical clock (CombatSettings::medicalClock): bleed, clot and the KO timer run on it, hunger does not ----
		{
			CombatSettings fast = settings;
			fast.medicalClock = 2.0; // the duel logs' Escaped Servant: blood loss 2.00 x rate per sim s, wounds clot 2 x
			CombatRules fastRules(data, fast);
			state::Fighter f = data->makeFighter(gd, servant->stringID, "", rng);
			f.wounds = {0.0366};
			f.unconscious = true;
			f.koTimer = 10.0;
			const double blood0 = f.blood;
			const double hunger0 = f.hunger;
			fastRules.updateMedical(f, 1.0);
			near(f.bleeding, 1.2 * (0.0366 - 2 * k.bleedClotRate), 1e-9, "medical clock 2: a wound clots 2 x per sim second");
			near(blood0 - f.blood, 2 * f.bleeding, 1e-9, "medical clock 2: blood falls 2 x rate per sim second");
			near(f.koTimer, 8.0, 1e-9, "medical clock 2: the KO timer runs 2 x");
			state::Fighter slow = data->makeFighter(gd, servant->stringID, "", rng);
			slow.unconscious = true;
			slow.koTimer = 10.0;
			rules.updateMedical(slow, 1.0);
			near(hunger0 - f.hunger, hunger0 - slow.hunger, 1e-12, "medical clock leaves hunger on game time");
		}

		// ---- limb collapse: below the KO point, never a timer (kinspect 8de1eab findings/medical) ----
		{
			// An intact limb is down when its effective health (health - stun + splint + heal pool) is below the
			// character's KO point; a severed limb is always down. Live: Engival's leg at -50 walks normally, at -52
			// he is down and stays conscious; his KO point is -51.65. Nothing is down without a collapsed part, the
			// flag is recomputed every tick (it clears as soon as the part rises above the point), and a repair ends
			// a cut one (no natural regeneration: an untreated leg keeps sinking until first aid).
			state::Fighter f = data->makeFighter(gd, servant->stringID, "", rng);
			f.skills.toughness = 55.5337; // Engival's: the live KO point -51.65 (-lerp(t, 10, 85))
			state::FighterPart* gl = nullptr;
			for (state::FighterPart& p : f.parts)
			{
				if (p.collapses && (p.collapseBits & (1 | 16 | 32)) && !p.vital && !p.robotic)
				{
					gl = &p;
					break;
				}
			}
			test::require(gl != nullptr, "no collapsing leg part on the servant");
			near(rules.koPoint(f), -51.65, 0.01, "the KO point at toughness 55.5337");
			const auto cut = [&](double health, double stun)
			{
				gl->health = health;
				gl->stun = stun;
				gl->pool = 0.0;
				rules.checkCollapse(f);
				return f.down;
			};
			test::require(!cut(-20.0, 0.0), "a leg at -20 stands");
			test::require(!cut(-50.0, 0.0), "a leg at -50 stands (live: the character walks)");
			test::require(cut(-52.0, 0.0), "a leg at -52 is down (below the -51.65 point)");
			test::require(!f.unconscious, "limbs never knock a character out");
			test::require(!cut(-40.0, 0.0), "raising the health above the point clears the down flag at once");
			test::require(cut(90.0, 145.0), "a stun pushing effective health to -55 also collapses the leg");
			for (int i = 0; i < 30; ++i)
				rules.updateMedical(f, 1.0);
			test::require(f.down, "still down 30 s later");
			for (int i = 0; i < 600; ++i)
				rules.updateMedical(f, 1.0);
			test::require(!f.down, "the stun collapse ends by itself as the stun decays (0.0125 HP / s per 100 max)");
			gl->health = -60.0;
			gl->stun = 0.0;
			rules.checkCollapse(f);
			test::require(f.down, "a leg cut to -60 is down");
			for (int i = 0; i < 600; ++i)
				rules.updateMedical(f, 1.0);
			test::require(f.down && !f.unconscious && f.blood > 0.0 && f.bleeding == 0.0,
				"a crippled leg stays down (awake, not bleeding) until treated");
			bool cause = false;
			for (const state::FighterPart& p : f.parts)
				cause = cause || p.collapsed;
			test::require(cause, "down has a collapsed part as its cause");
			gl->pool = gl->maxHealth * 4.0;
			rules.updateMedical(f, 40000.0);
			test::require(!f.down, "first aid ends a crippled-leg down");
			gl->health = -10.0;
			gl->pool = 0.0;
			gl->maxHealth = state::FighterPart::kSeveredMax; // limb state 1
			rules.updateMedical(f, 1.0);
			// The sever fires the forced KO writer (task BL, 3.0 medical units): down and KO'd, and leaking.
			test::require(f.down && f.unconscious && f.bleeding > 0.0, "a severed leg is down, KO'd and leaking");
			test::require(std::fabs(f.koTimer - CombatRules::kCollapseKoTime) < 0.06, "the sever's KO is the 3.0 floor");
			std::printf("ok  down: the KO point (not 0) collapses an intact limb, a severed one always\n");
		}

		// ---- first aid: Engival's numbers ----
		{
			const std::uint64_t medic = spawn(1000, 3.0, true);
			flecs::entity e = entityOf(medic);
			state::Fighter* f = const_cast<state::Fighter*>(sim.combat()->find(medic));
			f->skills.medic = 24.16; // x0.8 on himself = 19.33
			part(*f, "Head")->health = 70.0;
			part(*f, "Chest")->health = 60.0;
			part(*f, "Left Arm")->health = 50.0;
			part(*f, "Stomach")->health = 100.0;
			state::Inventory inv = *e.get<state::Inventory>();
			inv.add(bigKit, 1);
			inv.add(basicKit, 1);
			inv.add(robotKit, 1);
			e.set<state::Inventory>(inv);
			state::InteractTask task;
			task.target = medic;
			task.action = 10;
			e.set<state::InteractTask>(task);
			sim.run(5); // the order is picked up and the kit opened
			test::require(fighterOf(sim, medic).kitSid == basicKit, "the kit with the fewest charges is opened, not the first carried");
			const auto headOf = [&](const state::Fighter& x) -> const state::FighterPart&
			{
				for (const state::FighterPart& p : x.parts)
					if (p.name == "Head") return p;
				throw std::runtime_error("no head");
			};
			const double pool0 = headOf(fighterOf(sim, medic)).pool;
			const double kit0 = fighterOf(sim, medic).kitCharges;
			if (std::getenv("KMED_TRACE") != nullptr)
				for (int i = 0; i < 24; ++i)
				{
					sim.run(1);
					std::printf("   t%d pool %.4f kit %.4f\n", i, headOf(fighterOf(sim, medic)).pool, fighterOf(sim, medic).kitCharges);
				}
			else
			sim.run(20); // 1 s of treatment
			const state::Fighter& m = fighterOf(sim, medic);
			const state::FighterPart* head = &headOf(m);
			near(head->pool - pool0, 5.06, 0.1, "bandage pool / s, skill 19.33 vs quality 5 (live 5.06)");
			near(kit0 - m.kitCharges, 1.63, 0.1, "kit drain / s at skill 19.33 (live 1.65, predicted 1.63)");
			for (const state::FighterPart& p : m.parts)
				if (p.name == "Chest") test::require(p.pool == 0.0, "the head is filled before the chest");
			sim.run(400); // 20 s more: head (30), chest (40) and the arm (50) take 120 pool at 5.06 / s = 24 s
			sim.run(200);
			const state::Fighter& after = fighterOf(sim, medic);
			double pooled = 0.0;
			for (const state::FighterPart& p : after.parts)
				pooled += p.pool;
			test::require(pooled > 100.0, "every damaged organic part got its pool");
			test::require(entityOf(medic).get<state::InteractTask>() == nullptr, "the treatment finished");
			test::require(after.skills.medic > 24.16, "First Aid gained XP");
			test::require(entityOf(medic).get<state::Inventory>()->carried(robotKit) == 1, "the unused robotic kit was not opened");
			std::printf("ok  first aid done: pool %.1f, kit left %.1f, First Aid %.4f\n", pooled, after.kitCharges, after.skills.medic);
		}
		// ---- k16.6.174 (rwh.154): several medics on one patient are one serial stream, and the aid ends on the medical state alone ----
		{
			const auto aid = [&](int medics, double& rate, double& endSpread)
			{
				const std::uint64_t patient = spawn(5000.0 + 100.0 * medics, 3.0, false);
				state::Fighter* p = const_cast<state::Fighter*>(sim.combat()->find(patient));
				p->wounds.clear();
				p->bleeding = 0.0;
				p->koTimer = 3000.0; // stays down: the aid's work is what is measured
				double missing = 0.0;
				for (state::FighterPart& part : p->parts)
				{
					part.health = part.maxHealth - (part.type == 3 ? 55.0 : part.type == 0 ? 60.0 : 20.0); // head, torso, limbs: a heavy profile
					part.pool = 0.0;
					missing += part.maxHealth - part.health;
				}
				std::vector<std::uint64_t> crew;
				for (int m = 0; m < medics; ++m)
				{
					const std::uint64_t id = spawn(5000.0 + 100.0 * medics + 2.0 * (m + 1), 3.0, false);
					flecs::entity e = entityOf(id);
					const_cast<state::Fighter*>(sim.combat()->find(id))->skills.medic = 24.16;
					state::Inventory inv = *e.get<state::Inventory>();
					inv.add(basicKit, 1);
					e.set<state::Inventory>(inv);
					state::InteractTask task;
					task.target = patient;
					task.action = 10;
					e.set<state::InteractTask>(task);
					crew.push_back(id);
				}
				sim.run(5); // the orders are picked up and the kits opened
				const auto pooled = [&]
				{
					double sum = 0.0;
					for (const state::FighterPart& part : fighterOf(sim, patient).parts)
						sum += part.pool;
					return sum;
				};
				const double p0 = pooled();
				sim.run(40); // 2 s
				rate = (pooled() - p0) / 2.0;
				std::vector<int> ended(crew.size(), -1);
				for (int t = 0; t < 4000; ++t)
				{
					sim.run(1);
					for (std::size_t m = 0; m < crew.size(); ++m)
						if (ended[m] < 0 && entityOf(crew[m]).get<state::InteractTask>() == nullptr)
							ended[m] = t;
					if (std::find(ended.begin(), ended.end(), -1) == ended.end())
						break;
				}
				test::require(std::find(ended.begin(), ended.end(), -1) == ended.end(), "every medic's aid ended");
				endSpread = *std::max_element(ended.begin(), ended.end()) - *std::min_element(ended.begin(), ended.end());
				const state::Fighter& after = fighterOf(sim, patient);
				test::require(after.wounds.empty(), "the aid ended with the wound list empty");
				double healthLost = 0.0;
				for (const state::FighterPart& part : after.parts)
				{
					test::require(std::fabs(part.pool - (part.maxHealth - part.wear - part.health)) < 1.0e-6, "every part's bandage pool is at max - health when the aid ends");
					healthLost += part.maxHealth - part.health;
				}
				test::require(std::fabs(healthLost - missing) < 2.0, "the patient is bandaged, not healed: health stays where it was");
			};
			double one = 0.0;
			double four = 0.0;
			double spread1 = 0.0;
			double spread4 = 0.0;
			aid(1, one, spread1);
			aid(4, four, spread4);
			std::printf("ok  net pool rate: 1 medic %.2f/s, 4 medics %.2f/s; end spread %g ticks\n", one, four, spread4);
			test::require(one > 4.0 && one < 9.0, "one medic: ~7 pool points/s band 4-9 (rwh.154 6.4-7.8 net; skill 24 vs quality 5 gives 5.06 live)");
			test::require(four < one * 1.15 && four > one * 0.85, "four medics bandage no faster than one (rwh.154: the group is not N times faster)");
			test::require(spread4 <= 2.0, "all medics end on the same frame (rwh.154: within 0.03 s)");
		}
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "FAIL: " << e.what() << "\n";
		return 1;
	}
}
