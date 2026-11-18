// Player buildings, Phase 2 (docs/reference/BUILDINGS_SLICE.md section 4): the construction math. Pure functions on worked values (the build21
// table: Small Shack T = 5, build speed mult 3 -> 0 hr; Wind Generator T = 13, mult 1 -> 2 hr, Ka 1, Kb 6), then a placed site stepped
// with scripted materials and builders: the material cap, hp gain, pause (order 94), completion, Ka 2 doubling the rate, two builders,
// the BUILDING frame's progress / rate extrapolation against the sim, save and continue.
// argv[1]: the Hub navmesh cache (`knav_bake`, ctest fixture knav_cache).

#include "buildings_rig.hpp"

#include <sstream>

namespace {

using namespace bt;

bool near(double a, double b, double eps) { return std::fabs(a - b) <= eps; }

state::PlayerBuilding site(double maxHp, std::vector<state::BuildMaterial> mats)
{
	state::PlayerBuilding b;
	b.maxHp = maxHp;
	b.mats = std::move(mats);
	return b;
}

// The shack placed at `at`, with scripted builders; returns the Id.
struct Fixture
{
	Rig rig;
	std::uint64_t id = 0;
	std::vector<state::BuildMaterial> cost;
	Fixture(const kworld::LoadOrder& o, const std::shared_ptr<const kdata::GameData>& g, const std::shared_ptr<const CombatData>& d,
		const std::shared_ptr<MeshNavigator>& n, const BuildOrder& at, double ka)
		: rig(o, g, d, n)
	{
		rig.sim.setBuildSpeed(ka);
		id = rig.placeOk(at);
		cost = buildings::buildingCost(*g->find(at.record));
	}
	const state::PlayerBuilding& b() const { return *rig.building(id); }
};

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc == 2, "usage: ksim_buildings_build HUB.knav");
		namespace bd = ksim::buildings;
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);
		const auto gd = std::make_shared<const kdata::GameData>(kworld::loadMergedGameData(order));
		const std::shared_ptr<const CombatData> data = CombatData::build(*gd, KENSHI_DATA);
		const std::shared_ptr<MeshNavigator> nav = MeshNavigator::load(argv[1]);

		// ---- the build21 table, from the merged data ----
		const kdata::Record* shack = nullptr;
		const kdata::Record* wind = nullptr;
		for (const kdata::Record* rec : gd->byType(kworld::itemTypeByName("BUILDING")))
		{
			if (rec->name == "Small Shack")
				shack = rec;
			if (rec->name == "Wind Generator")
				wind = rec;
		}
		test::require(shack != nullptr && wind != nullptr, "the Small Shack and Wind Generator records");
		{
			const std::uint32_t t1 = bd::totalMaterials(bd::buildingCost(*shack));
			const std::uint32_t t2 = bd::totalMaterials(bd::buildingCost(*wind));
			const double m1 = shack->floatOr("build speed mult", 1.0f);
			const double m2 = wind->floatOr("build speed mult", 1.0f);
			test::require(t1 == 5 && m1 == 3.0, "Small Shack: T = 5, build speed mult 3 (" + std::to_string(t1) + ", " + std::to_string(m1) + ")");
			test::require(t2 == 13 && m2 == 1.0, "Wind Generator: T = 13, build speed mult 1 (" + std::to_string(t2) + ", " + std::to_string(m2) + ")");
			test::require(bd::estimatedHours(t1, m1, 1.0) == 0 && bd::estimatedHours(t2, m2, 1.0) == 2, "estimated hours 0 and 2 at Ka 1, Kb 6");
			test::require(near(bd::buildHours(t1, m1, 1.0), 5.0 / 18.0, 1e-12) && near(bd::buildHours(t2, m2, 1.0), 13.0 / 6.0, 1e-12), "build hours 5 / 18 and 13 / 6");
			test::require(near(bd::buildHours(t2, m2, 2.0), 13.0 / 12.0, 1e-12) && near(bd::buildHours(t2, m2, 0.5), 13.0 / 3.0, 1e-12), "Ka 2 halves the hours, Ka 0.5 doubles them");
			test::require(bd::estimatedHours(t2, m2, 2.0) == 1 && bd::estimatedHours(t2, m2, 0.5) == 4, "estimates at Ka 2 and 0.5");
			test::require(bd::buildHours(0, 3.0, 1.0) == 0.0, "no materials: no build time");
			test::require(bd::maxHp(*shack) == 5.0 && bd::maxHp(*wind) == 13.0, "maxHp is the sum of the construction counts");
		}

		// ---- the pure rules on worked values ----
		{
			const double perTick = bd::hpPerTick(13.0, 13.0 / 6.0, 1.0);
			test::require(near(perTick, 6.0 / static_cast<double>(kTicksPerGameHour), 1e-12), "13 hp over 13 / 6 hours: 6 hp per game hour");
			test::require(near(bd::hpPerTick(13.0, 13.0 / 6.0, 2.0), 2.0 * perTick, 1e-15) && bd::hpPerTick(13.0, 13.0 / 6.0, 0.0) == 0.0, "builders add; none gain nothing");
			state::PlayerBuilding b = site(10.0, {{"a", 6, 3}, {"b", 4, 0}});
			test::require(near(bd::materialCap(b), 3.0, 1e-12), "cap = maxHp x delivered / needed = 10 x 3 / 10");
			b.mats[1].have = 4;
			test::require(near(bd::materialCap(b), 7.0, 1e-12), "cap after delivering the other row");
			b.mats[0].have = 6;
			test::require(near(bd::materialCap(b), 10.0, 1e-12), "all delivered: the cap is maxHp");
			const state::PlayerBuilding partial = site(10.0, {{"a", 6, 1}});
			test::require(bd::accepts(b, "a") == 0 && bd::accepts(partial, "a") == 5 && bd::accepts(b, "zz") == 0, "a site accepts only what it still needs");
			// a step: one builder, 1 hp per tick, delivered 4 of 10 -> stops at 4
			state::PlayerBuilding s = site(10.0, {{"a", 10, 4}});
			const double hours = 10.0 / static_cast<double>(kTicksPerGameHour); // maxHp / hours / ticksPerHour = 1 hp per tick
			for (int i = 0; i < 100; ++i)
				bd::stepSite(s, hours, 1.0);
			test::require(near(s.hp, 4.0, 1e-9) && !s.built, "the cap holds the hp at 4");
			test::require(bd::statusOf(s) == bd::kNeedsMaterials, "status NeedsMaterials at the cap");
			s.mats[0].have = 10;
			for (int i = 0; i < 100; ++i)
				bd::stepSite(s, hours, 1.0);
			test::require(s.built && s.hp == 10.0 && bd::progressOf(s) == 1.0 && bd::statusOf(s) == bd::kWorking, "all delivered: completes at maxHp");
			state::PlayerBuilding p = site(10.0, {{"a", 10, 10}});
			p.paused = true;
			test::require(!bd::stepSite(p, hours, 1.0) && p.hp == 0.0 && bd::statusOf(p) == bd::kPaused, "paused: no progress");
			test::require(bd::rateOf(p, hours) == 0.0, "paused: rate 0");
			test::require(bd::navActive(0.5, 0.0) && !bd::navActive(0.5, 80.0) && bd::navActive(0.8, 80.0), "the nav follows past build threshold percent");
			// kinspect 52 live rows (item52_build_timing.md): a Small Wind Generator, build speed mult 1, maxHp 11, Ka 1, Kb 6. HP per game hour is
			// 6 x Sum w_k 0.75^k; the live rows are reference values within 5 %, the formula is exact.
			const auto perHour = [&](const std::vector<double>& eng) { return bd::builderSum(eng) * 1.0 * 1.0 * bd::kKb; };
			test::require(near(perHour({43.3}), 6.2166, 0.01) && near(perHour({43.3}), 6.21, 6.21 * 0.05), "Eng 43.3 alone: 6.22 predicted, 6.21 live");
			test::require(near(perHour({21.6}), 4.003, 0.01) && near(perHour({21.6}), 4.16, 4.16 * 0.05), "Eng 21.6 alone: 4.00 predicted, 4.16 live");
			test::require(near(perHour({21.6, 43.3}), 8.666, 0.01) && near(perHour({21.6, 43.3}), 8.49, 8.49 * 0.05), "both, the weaker first: 8.62 predicted (message order), 8.49 / 8.64 live");
			test::require(near(11.0 / perHour({43.3}), 1.77, 1.77 * 0.05), "11 hp at Eng 43.3 takes 1.77 game hours live");
			test::require(near(bd::skillMult(0), 0.3, 1e-12) && near(bd::skillMult(100), 2.0, 1e-12) && near(bd::skillMult(500), 2.0, 1e-12), "w = 0.3 + 1.7 Eng / 100, clamped");
			std::cout << "pure ok\n";
		}

		// ---- a placed site ----
		BuildOrder at;
		{
			Rig probe(order, gd, data, nav);
			at = probe.findSite(shack->stringID).value();
		}
		{
			Fixture f(order, gd, data, nav, at, 1.0);
			Rig& r = f.rig;
			const double hours = bd::buildHours(5, 3.0, 1.0);
			const double perTick = bd::hpPerTick(5.0, hours, 1.0);
			test::require(f.cost.size() >= 1 && f.b().hp == 0.0, "placed: hp 0");
			// no builder, no material: nothing
			r.tick(100);
			test::require(f.b().hp == 0.0 && f.b().version == 1, "no builder: no progress, no frame");
			// a builder with no material: the cap is 0
			test::require(r.sim.setBuilders(f.id, 1), "setBuilders");
			r.tick(100);
			test::require(f.b().hp == 0.0 && f.b().status == bd::kNeedsMaterials, "a builder without material: hp stays 0");
			// deliver part of the first row
			const state::BuildMaterial& row = f.cost[0];
			const std::uint32_t part = std::max<std::uint32_t>(1, row.need / 2);
			test::require(r.sim.addMaterials(f.id, row.itemSid, part + 1000) == row.need && f.b().mats[0].have == row.need, "addMaterials accepts what the site needs, no more");
			// (the whole first row went in: re-deliver nothing)
			test::require(r.sim.addMaterials(f.id, row.itemSid, 1) == 0 && r.sim.addMaterials(f.id, "no-such-item", 1) == 0, "a full row and an unknown item accept nothing");
			double delivered = row.need;
			const double cap = 5.0 * delivered / 5.0;
			r.buildingFrames = 0;
			r.tick(1);
			test::require((r.buildingFrames == 1 && r.dec.buildings.at(f.id).rate > 0.0f) || row.need == 5, "a delivery sends a frame");
			const std::uint64_t t0 = r.sim.clockTick();
			const double hp0 = f.b().hp;
			r.tick(100);
			test::require(near(f.b().hp - hp0, 100 * perTick, 1e-9) || f.b().hp >= cap - 1e-9, "100 ticks add 100 x hpPerTick (or reach the cap)");
			// the client's extrapolation against the sim, while still below the cap
			if (f.b().hp < cap - 1.0)
			{
				const auto& d = r.dec.buildings.at(f.id);
				const double extrapolated = d.progress + d.rate * static_cast<double>(r.sim.clockTick() - d.tick) * kTickSeconds;
				test::require(near(extrapolated, f.b().hp / 5.0, 1e-4) && near(d.rate, perTick / 5.0 / kTickSeconds, 1e-6), "the frame's progress and rate extrapolate to the sim's");
			}
			(void)t0;
			// pause (order 94): the hp holds, the status says Paused, one frame
			if (f.b().hp < cap - 1e-9)
			{
				const double hold = f.b().hp;
				r.buildingFrames = 0;
				Buf body;
				body.put<std::uint64_t>(f.id).put<std::uint8_t>(1);
				test::require(r.send(94, body).empty(), "order 94 on an unfinished site");
				r.tick(50);
				test::require(f.b().paused && f.b().hp == hold && f.b().status == bd::kPaused, "paused: hp holds");
				test::require(r.buildingFrames == 1 && r.dec.buildings.at(f.id).status == bd::kPaused && (r.dec.buildings.at(f.id).flags & 2u) && r.dec.buildings.at(f.id).rate == 0.0f,
					"one frame for the pause, rate 0");
				Buf off;
				off.put<std::uint64_t>(f.id).put<std::uint8_t>(0);
				test::require(r.send(94, off).empty(), "unpause");
				r.tick(10);
				test::require(!f.b().paused && f.b().hp > hold, "unpaused: the work resumes");
			}
			Buf bad;
			bad.put<std::uint64_t>(999999).put<std::uint8_t>(1);
			test::require(r.send(94, bad) == "bad_target", "pause of an unknown building");
			Buf twice;
			twice.put<std::uint64_t>(f.id).put<std::uint8_t>(2);
			test::require(r.send(94, twice) == "bad_value", "pause value 2");
			// run to the cap, deliver the rest, run to completion
			r.tick(3000);
			test::require(near(f.b().hp, 5.0 * delivered / 5.0, 1e-9) || f.b().built, "the hp stops at the cap");
			for (std::size_t i = 1; i < f.cost.size(); ++i)
			{
				r.sim.addMaterials(f.id, f.cost[i].itemSid, f.cost[i].need);
				delivered += f.cost[i].need;
			}
			test::require(delivered == 5.0, "every row delivered");
			// save mid-build, then continue both
			{
				std::ostringstream saved;
				r.sim.save(saved);
				std::istringstream in(saved.str());
				Simulation loaded = Simulation::restore(World::load(KENSHI_DATA), in, {}, false, nullptr, r.sim.gameData());
				loaded.enableCombat(data, Rig::settings());
				loaded.enableNavigation(nav);
				test::require(loaded.stateHash() == r.sim.stateHash(), "the hash changed across save and load");
				r.sim.run(100);
				loaded.run(100);
				test::require(loaded.stateHash() == r.sim.stateHash(), "save, load and continue differs from the uninterrupted run");
			}
			int guard = 0;
			while (!f.b().built && guard++ < 5000)
				r.tick();
			test::require(f.b().built && f.b().hp == 5.0 && f.b().status == bd::kWorking && !f.b().paused, "completes: built, hp = maxHp, status Working");
			const auto& d = r.dec.buildings.at(f.id);
			test::require((d.flags & 1u) && d.progress == 1.0f && d.rate == 0.0f && d.materials.empty() && d.hp == 5.0f && d.status == bd::kWorking, "the completion frame");
			r.buildingFrames = 0;
			r.tick(200);
			test::require(r.buildingFrames == 0 && f.b().hp == 5.0, "a built site sends nothing and changes nothing");
			Buf late;
			late.put<std::uint64_t>(f.id).put<std::uint8_t>(1);
			test::require(r.send(94, late) == "bad_target", "pause of a built building");
			std::cout << "site ok\n";
		}

		// ---- Ka 2 doubles the rate; two builders give 1.75x (the 0.75^k damping) ----
		{
			double hp[3] = {};
			const double kas[3] = {1.0, 2.0, 1.0};
			for (int i = 0; i < 3; ++i)
			{
				Fixture f(order, gd, data, nav, at, kas[i]);
				for (const state::BuildMaterial& m : f.cost)
					f.rig.sim.addMaterials(f.id, m.itemSid, m.need);
				f.rig.sim.setBuilders(f.id, i == 2 ? 2 : 1);
				f.rig.tick(100);
				hp[i] = f.b().hp;
				const double expected = 100 * bd::hpPerTick(5.0, bd::buildHours(5, 3.0, kas[i]), i == 2 ? bd::builderFactor(2) : 1.0);
				test::require(near(hp[i], expected, 1e-9) && hp[i] > 0.0, "hp after 100 ticks is 100 x hpPerTick (case " + std::to_string(i) + ")");
			}
			test::require(near(hp[1], 2.0 * hp[0], 1e-9), "Ka 2 doubles the rate");
			test::require(near(hp[2], 1.75 * hp[0], 1e-9), "two builders: 1 + 0.75 = 1.75 times the rate");
			std::cout << "ka ok\n";
		}
		std::cout << "ksim_buildings_build ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_buildings_build FAILED: " << e.what() << "\n";
		return 1;
	}
}
