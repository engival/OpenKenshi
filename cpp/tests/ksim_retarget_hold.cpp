// combat.retarget_hold_s (kenshi-n02.1, COMBAT_OPTIONS.md 6): the seconds a normal NPC keeps its target before pickTarget may re-pick.
// The original holds a target while the CCAI+0x2f0 timer is < 1.0 s (< 0.3 s with CharStats+0x12b set), default 1.0
// (findings/ai_target_hand.md, row CCAI+0x2f0 and "hold" paragraph, corrected 2026-11-13 rwh.113; the old "0 = the exe" reading was wrong).
// The +0x12b 0.3 s variant is not modelled (no character carries it), so only the 1.0 s default is asserted. A 3v3 on a bare arena (no navmesh),
// each fighter's held target dropping out of the candidate scan for one pick every 0.4 s (what a re-pick needs: the current target's +5 / +10 beat
// every other enemy otherwise): at the default (CombatSettings{}.retargetHold == 1.0) no fighter changes target less than 1.0 s after its previous
// change; at hold 0 (the setting's A/B off position, NOT the original) some do, and it re-picks clearly more often. The scenario header's
// `retarget_hold=S` field parses and a value out of range is refused.

#include "ksim/combat/combat.hpp"
#include "ksim/save/scenario_file.hpp"

#include "kworld/gamedata.hpp"

#include "test_util.hpp"
#include "ksim/dev/build_paths.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;

constexpr double kDt = 0.05;
constexpr int kSteps = 20 * 40;
constexpr int kLapseSteps = 8; // each fighter's target drops out of its scan every 0.4 s

struct Retargets
{
	int changes = 0;
	int quick = 0;  // a voluntary switch < 1.0 s after that fighter's previous change (the previous target was still a valid target)
	int forced = 0; // a change < 1.0 s after the previous one because the previous target died, was KO'd or went down, or
					// a switch onto an enemy that is attacking it (the counter flip of a defender, CADENCE_SLICE.md; it retargets outside pickTarget): the hold does not cover these
};

// Target changes (one target to another, or none to one) of six fighters over 40 s of a 3v3 in the open.
Retargets retargets(const kdata::GameData& gd, const std::shared_ptr<const CombatData>& data, double hold, std::uint64_t seed)
{
	CombatSettings settings;
	settings.retargetHold = hold;
	CombatArena arena(data, settings, seed);
	const kdata::Record* servant = nullptr;
	for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("CHARACTER")))
		if (r->name == "Escaped Servant")
			servant = r;
	test::require(servant != nullptr, "no Escaped Servant");
	std::uint64_t rng = seed;
	std::vector<std::uint64_t> ids;
	for (int i = 0; i < 6; ++i)
	{
		state::Fighter f = data->makeFighter(gd, servant->stringID, "", rng);
		const int team = i % 2;
		f.x = team == 0 ? -40.0 : 30.0;
		f.z = team == 0 ? 30.0 * (i / 2) - 30.0 : 3.0 * (i / 2);
		f.yaw = team == 0 ? 1.5707963267948966 : -1.5707963267948966;
		f.team = team;
		arena.add(i + 1, std::move(f));
		ids.push_back(i + 1);
	}
	std::vector<std::uint64_t> last(ids.size(), 0);
	std::vector<int> lastChange(ids.size(), -1000000);
	Retargets out;
	for (int step = 0; step < kSteps; ++step)
	{
		// What makes a held target drop out of the candidate scan in a real fight (a wall between, a turned head) is stood in for by a one-pick
		// block on it (aggroBlock, the "gave up this one" flag): the +5 / +10 of the current target outscore any other enemy while it is a
		// candidate, so without a lapse nobody re-picks.
		if (step % kLapseSteps == 0)
			for (std::size_t i = 0; i < ids.size(); ++i)
			{
				state::Fighter* f = arena.find(ids[i]);
				if (f->target != 0)
				{
					f->aggroBlockId = f->target;
					f->aggroBlock = 2.0 * kDt;
				}
			}
		// A held target that is dead or down is dropped at once (targetOf / targetable), so its re-pick is forced, not a hold break.
		std::vector<bool> gone(ids.size(), false);
		const auto lost = [&](std::uint64_t t)
		{
			const state::Fighter* tf = t != 0 ? arena.find(t) : nullptr;
			return tf == nullptr || tf->dead || tf->unconscious || tf->down || tf->carriedBy != 0 || tf->cageBuilding != 0;
		};
		for (std::size_t i = 0; i < ids.size(); ++i)
			gone[i] = lost(last[i]);
		arena.step(kDt);
		for (std::size_t i = 0; i < ids.size(); ++i)
		{
			const state::Fighter* f = arena.find(ids[i]);
			if (f->target != 0 && f->target != last[i])
			{
				++out.changes;
				if (last[i] != 0 && (step - lastChange[i]) * kDt < 1.0 - 1e-9)
				{
					if (gone[i] || lost(last[i]) || arena.find(f->target)->target == ids[i])
						++out.forced;
					else
						++out.quick;
				}
				lastChange[i] = step;
			}
			last[i] = f->target;
		}
	}
	return out;
}

// The repository's beak_open_ground scenario with `extra` appended to its header line.
void writeHeader(const std::string& path, const std::string& extra)
{
	std::ifstream in(std::string(KSIM_SCENARIOS_DIR) + "/beak_open_ground.kscen");
	std::ofstream out(path);
	std::string line;
	while (std::getline(in, line))
		out << line << (line.rfind("scenario ", 0) == 0 ? extra : "") << "\n";
}

} // namespace

int main()
{
	try
	{
		const kdata::GameData gd = kworld::loadMergedGameData(KENSHI_DATA);
		const std::shared_ptr<const CombatData> data = CombatData::build(gd, KENSHI_DATA);
		test::require(CombatSettings{}.retargetHold == 1.0, "the default retarget hold is not the original's 1.0 s");
		Retargets zero;
		Retargets one;
		for (std::uint64_t seed = 11; seed < 14; ++seed)
		{
			const Retargets z = retargets(gd, data, 0.0, seed);
			const Retargets o = retargets(gd, data, CombatSettings{}.retargetHold, seed);
			zero.changes += z.changes;
			zero.quick += z.quick;
			zero.forced += z.forced;
			one.changes += o.changes;
			one.quick += o.quick;
			one.forced += o.forced;
		}
		std::cout << "ksim_retarget_hold: target changes in 3 x 40 s of a 3v3: hold 0 -> " << zero.changes << " (" << zero.quick
				  << " voluntary within 1 s, " << zero.forced << " forced), default hold 1.0 -> " << one.changes << " (" << one.quick
				  << " voluntary within 1 s, " << one.forced << " forced)\n";
		test::require(zero.changes > 0 && one.changes > 0, "a fight with no target at all");
		test::require(one.quick == 0, "the default hold re-picked a still-valid target " + std::to_string(one.quick) + " times inside 1.0 s of the previous pick (the original holds < 1.0 s)");
		test::require(zero.quick > 0, "hold 0 never re-picked inside 1.0 s: the lapse does not exercise the hold");
		test::require(2 * zero.changes >= 3 * one.changes, "hold 0 re-picks " + std::to_string(zero.changes) + " times against " + std::to_string(one.changes) + " at 1.0: not clearly more (1.5x)");

		const std::string path = (std::filesystem::temp_directory_path() / "ksim_retarget_hold.kscen").string();
		writeHeader(path, "");
		test::require(!scenario::parseFile(path).retargetHold.has_value(), "no retarget_hold field reads as set");
		writeHeader(path, " retarget_hold=1.5");
		const scenario::File pinned = scenario::parseFile(path);
		test::require(pinned.retargetHold.has_value() && *pinned.retargetHold == 1.5, "retarget_hold=1.5 did not parse");
		writeHeader(path, " retarget_hold=-1");
		bool refused = false;
		try
		{
			scenario::parseFile(path);
		} catch (const std::runtime_error&) {
			refused = true;
		}
		test::require(refused, "retarget_hold=-1 was accepted");
		// The turret_manning header key (kenshi-k16.6.24): 0 / 1, absent = unset, anything else refused.
		writeHeader(path, "");
		test::require(!scenario::parseFile(path).turretManning.has_value(), "no turret_manning field reads as set");
		writeHeader(path, " turret_manning=1");
		const scenario::File manned = scenario::parseFile(path);
		test::require(manned.turretManning.has_value() && *manned.turretManning, "turret_manning=1 did not parse");
		writeHeader(path, " turret_manning=0");
		const scenario::File unmanned = scenario::parseFile(path);
		test::require(unmanned.turretManning.has_value() && !*unmanned.turretManning, "turret_manning=0 did not parse");
		writeHeader(path, " turret_manning=2");
		refused = false;
		try
		{
			scenario::parseFile(path);
		} catch (const std::runtime_error&) {
			refused = true;
		}
		test::require(refused, "turret_manning=2 was accepted");
		std::filesystem::remove(path);
		return 0;
	}
	catch (const std::exception& e)
	{
		std::cerr << "ksim_retarget_hold: " << e.what() << "\n";
		return 1;
	}
}
