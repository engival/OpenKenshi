// One defender against many attackers (kenshi-k16.6.27; the crowd-combat capture runA: a player and ten hungry
// bandits on open ground). Measures the numbers the capture gives, per step, on a bare combat arena (no navmesh): the defender's engaged count, the
// attack-slot holders, the Circle / Wait counts and their distance bands, the closest attacker pair, the per-step position jumps against the gait step,
// and the hits by attackers that were not in the defender's engaged list. With an argument it writes the table (tsv) there.
#include "ksim/combat/combat.hpp"

#include "kworld/gamedata.hpp"

#include "test_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <string>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;

constexpr double kDt = 0.05;

struct Band
{
	std::vector<double> v;
	void add(double d) { v.push_back(d); }
	double pct(double p) const
	{
		if (v.empty())
			return 0.0;
		std::vector<double> s = v;
		std::sort(s.begin(), s.end());
		return s[static_cast<std::size_t>(p * static_cast<double>(s.size() - 1))];
	}
};

struct Result
{
	std::size_t frames = 0;
	std::size_t engaged5 = 0;
	std::size_t slotHeld = 0;
	std::size_t slotMax = 0;
	std::size_t waitFrames = 0; // frames with exactly (attackers - 5) in Wait while 5 circle / attack
	Band circle;
	Band wait;
	std::vector<std::pair<std::size_t, std::size_t>> pairHist;
	double minPair = 1e9;
	Band pairs;
	double maxJump = 0.0; // worst step / (run speed x dt)
	std::size_t jumps2x = 0;
	std::size_t hits = 0;
	std::size_t hitsNonEngaged = 0;
	std::size_t waitCountSum = 0;
	std::size_t circleCountSum = 0;
};

Result measure(const kdata::GameData& gd, const std::shared_ptr<const CombatData>& data, std::uint64_t seed, int bandits, int steps, double ringRadius, bool trace = false)
{
	const kdata::Record* bandit = nullptr;
	const kdata::Record* servant = nullptr;
	for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("CHARACTER")))
	{
		if (r->name == "Hungry bandit")
			bandit = r;
		if (r->name == "Escaped Servant")
			servant = r;
	}
	test::require(servant != nullptr, "no Escaped Servant");
	if (bandit == nullptr)
		bandit = servant;
	CombatArena arena(data, CombatSettings{}, seed);
	std::uint64_t rng = 7;
	auto spawn = [&](const kdata::Record* r, std::uint64_t id, double x, double z, int team, bool player)
	{
		state::Fighter f = data->makeFighter(gd, r->stringID, "", rng);
		f.ranged = state::FighterRanged{};
		f.x = x;
		f.z = z;
		f.team = team;
		f.playerControlled = player;
		arena.add(id, std::move(f));
	};
	spawn(servant, 1, 0.0, 0.0, 0, true);
	std::vector<std::uint64_t> ids;
	for (int i = 0; i < bandits; ++i)
	{
		const double a = 6.283185307179586 * i / bandits;
		ids.push_back(static_cast<std::uint64_t>(i + 2));
		// The capture teleported the player next to one bandit and ordered the attack on it: bandit 0 starts beside the defender.
		const double r = i == 0 ? 20.0 : ringRadius;
		spawn(bandit, ids.back(), r * std::cos(a), r * std::sin(a), 1, false);
	}
	Result out;
	std::vector<std::uint64_t> before;
	std::map<std::uint64_t, std::vector<std::string>> hist;
	int stepNo = 0;
	arena.setObserver([&](const CombatEvent& e)
	{
		if (e.kind != CombatEvent::Kind::Hit || e.who != 1)
			return;
		++out.hits;
		const auto& eng = arena.find(1)->engaged;
		if (std::find(eng.begin(), eng.end(), e.other) == eng.end() && std::find(before.begin(), before.end(), e.other) == before.end())
		{
			++out.hitsNonEngaged;
			if (trace)
			{
				const state::Fighter& a = *arena.find(e.other);
				const auto& h = hist[e.other];
				std::string prev;
				for (std::size_t i = h.size() > 60 ? h.size() - 60 : 0; i < h.size(); ++i)
				{
					const std::string key = h[i].substr(h[i].find(' '));
					if (key != prev)
						std::printf("      %s\n", h[i].c_str());
					prev = key;
				}
				std::printf("  non-engaged hit step %d by %llu state %d target %llu tech %d engagedNow %zu wasEngagedLastStep %d\n", stepNo, static_cast<unsigned long long>(e.other),
					static_cast<int>(a.state), static_cast<unsigned long long>(a.target), a.technique, arena.find(1)->engaged.size(), 0);
			}
		}
	});
	std::map<std::uint64_t, std::pair<double, double>> last;
	for (int s = 0; s < steps; ++s)
	{
		stepNo = s;
		before = arena.find(1)->engaged;
		arena.step(kDt);
		const state::Fighter& d = *arena.find(1);
		if (d.unconscious || d.dead)
			break;
		++out.frames;
		out.engaged5 += d.engaged.size() == 5 ? 1 : 0;
		std::size_t held = 0;
		for (const state::FighterSlot& sl : d.slots)
			held += sl.holder != 0 ? 1 : 0;
		out.slotHeld += held > 0 ? 1 : 0;
		out.slotMax = std::max(out.slotMax, held);
		std::size_t nc = 0;
		std::size_t nw = 0;
		for (std::uint64_t id : ids)
		{
			const state::Fighter& f = *arena.find(id);
			const double dist = std::hypot(f.x - d.x, f.z - d.z);
			if (f.state == state::CombatState::Circle)
			{
				out.circle.add(dist);
				++nc;
			} else if (f.state == state::CombatState::Wait) {
				out.wait.add(dist);
				++nw;
			}
			auto it = last.find(id);
			if (it != last.end())
			{
				const double step = std::hypot(f.x - it->second.first, f.z - it->second.second);
				const double ratio = step / std::max(1e-9, arena.runSpeed(f) * kDt);
				out.maxJump = std::max(out.maxJump, ratio);
				out.jumps2x += ratio > 2.0 ? 1 : 0;
			}
			last[id] = {f.x, f.z};
			for (std::uint64_t other : ids)
			{
				if (other <= id)
					continue;
				const state::Fighter& g = *arena.find(other);
				// A KO'd or dead body is a ragdoll: the uprights never read it (task AW, separate()), so a lunge may cross it. The capture's 6.98 u is between standing attackers.
				if (f.unconscious || f.dead || g.unconscious || g.dead)
					continue;
				const double p = std::hypot(f.x - g.x, f.z - g.z);
				if (p < out.minPair)
				{
					out.minPair = p;
					if (trace && p < 6.5)
						std::printf("  pair %llu/%llu %.2f at step %d states %d/%d speeds %.1f/%.1f\n", static_cast<unsigned long long>(id), static_cast<unsigned long long>(other), p, s,
							static_cast<int>(f.state), static_cast<int>(g.state), f.forwardSpeed, g.forwardSpeed);
				}
				out.pairs.add(p);
			}
		}
		if (trace)
			for (std::uint64_t id : ids)
			{
				const state::Fighter& b = *arena.find(id);
				const bool in = std::find(d.engaged.begin(), d.engaged.end(), id) != d.engaged.end();
				hist[id].push_back("s" + std::to_string(s) + " st" + std::to_string(static_cast<int>(b.state)) + " pend" + std::to_string(static_cast<int>(b.pending)) + " tech" + std::to_string(b.technique) + " tgt" + std::to_string(b.target) + (in ? " IN" : " out"));
			}
		out.waitCountSum += nw;
		out.circleCountSum += nc;
		out.waitFrames += nw == static_cast<std::size_t>(std::max(0, bandits - 5)) ? 1 : 0;
	}
	return out;
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		const kdata::GameData gd = kworld::loadMergedGameData(KENSHI_DATA);
		const std::shared_ptr<const CombatData> data = CombatData::build(gd, KENSHI_DATA);
		std::ofstream tsv;
		if (argc > 1)
			tsv.open(argv[1]);
		tsv << "seed\tframes\tengaged5\tslot_held_frames\tslot_max\twait_exact_frames\tcircle_p10\tcircle_p50\tcircle_p90\twait_p10\twait_p50\twait_p90\tmin_pair\tpair_p1\tpair_p5\tmax_jump_ratio\tjumps_gt2x\thits\thits_non_engaged\n";
		std::vector<Result> results;
		for (std::uint64_t seed = 1; seed <= 3; ++seed)
		{
			const Result r = results.emplace_back(measure(gd, data, seed, 10, 6000, 150.0, argc > 2));
			std::printf("seed %llu: frames %zu engaged==5 %zu slotHeld %zu (max %zu) wait==5 %zu | circle %.0f/%.0f/%.0f wait %.0f/%.0f/%.0f | minPair %.2f p1 %.2f p5 %.2f | maxJump %.2fx (%zu >2x) | hits %zu nonEngaged %zu\n",
				static_cast<unsigned long long>(seed), r.frames, r.engaged5, r.slotHeld, r.slotMax, r.waitFrames, r.circle.pct(0.1), r.circle.pct(0.5),
				r.circle.pct(0.9), r.wait.pct(0.1), r.wait.pct(0.5), r.wait.pct(0.9), r.minPair, r.pairs.pct(0.01), r.pairs.pct(0.05), r.maxJump,
				r.jumps2x, r.hits, r.hitsNonEngaged);
			tsv << seed << '\t' << r.frames << '\t' << r.engaged5 << '\t' << r.slotHeld << '\t' << r.slotMax << '\t' << r.waitFrames << '\t'
				<< r.circle.pct(0.1) << '\t' << r.circle.pct(0.5) << '\t' << r.circle.pct(0.9) << '\t' << r.wait.pct(0.1) << '\t' << r.wait.pct(0.5)
				<< '\t' << r.wait.pct(0.9) << '\t' << r.minPair << '\t' << r.pairs.pct(0.01) << '\t' << r.pairs.pct(0.05) << '\t' << r.maxJump << '\t'
				<< r.jumps2x << '\t' << r.hits << '\t' << r.hitsNonEngaged << '\n';
		}
		for (const Result& r : results)
		{
			test::require(r.frames > 100, "the defender lasted " + std::to_string(r.frames) + " steps");
			test::require(r.engaged5 * 100 >= r.frames * 97, "the defender's engaged list sits at its cap of 5 (capture: 6100 of 6111 frames)");
			test::require(r.slotMax == 1, "one attack slot is held at once");
			test::require(r.slotHeld * 100 >= r.frames * 80, "the slot is held almost all the time");
			test::require(r.minPair >= 4.0, "two attackers stack: closest pair " + std::to_string(r.minPair) + " u (capture 6.98; a lunge's step dips below 8)");
			test::require(r.pairs.pct(0.01) >= 8.0, "p1 of the attacker pair distances is under the footprint sum");
			test::require(r.jumps2x == 0, "a step above 2x the gait step: " + std::to_string(r.maxJump) + "x");
			test::require(r.circle.pct(0.5) >= 20.0 && r.circle.pct(0.5) <= 40.0, "the Circle median is outside the capture's 20-35 u band");
			test::require(r.wait.pct(0.5) >= 35.0 && r.wait.pct(0.5) <= 65.0, "the Wait median is outside the capture's 40-65 u band");
		}
		std::size_t nonEngaged = 0;
		for (const Result& r : results)
			nonEngaged += r.hitsNonEngaged;
		test::require(nonEngaged == 0, std::to_string(nonEngaged) + " hits by attackers outside the engaged list (capture: 0 of 22)");
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "FAIL: " << e.what() << "\n";
		return 1;
	}
}
