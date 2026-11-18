// The jobs on top of the construction and medical goals (task ds/jobs-3; kinspect dt24_101 sections 3-5, dt24_ai_enum_map): 125 JOB_BUILDER
// (Engineer: the nearest player building with work left -> 71 with a missing material in the pack, 2 on a site the delivered materials let it
// build, 95 on a damaged one), 57 JOB_REPAIR_ROBOT (Robotics: the Medic walk with a robot repair kit and a patient whose robotic parts are
// hurt). 55 RECRUIT_AT_JOBCENTER has no resolver (a null stub, dt24_101 section 5): nothing to test. Bare worlds, rows set by hand.

#include "dt27_rig.hpp"

#include "ksim/world/buildings.hpp"
#include "ksim/combat/combat_data.hpp"
#include "ksim/ai/jobs.hpp"
#include "ksim/ai/routines.hpp"

#include <algorithm>

namespace {

using namespace ksim;
using namespace dt27;

std::vector<std::string> resourceItems(Rig& rig, std::size_t want)
{
	std::vector<std::string> items;
	for (const kdata::Record* record : rig.gd->byType(kworld::itemTypeByName("ITEM")))
		if (const ItemInfo* info = rig.data->item(record->stringID); info != nullptr && info->function == 0 && items.size() < want)
			items.push_back(record->stringID);
	test::require(items.size() == want, "the data lacks plain resource ITEMs");
	return items;
}

std::string kitOf(Rig& rig, std::int32_t function)
{
	for (const auto& [sid, info] : rig.data->items)
		if (info.function == function && info.charges > 0.0)
			return sid;
	test::require(false, "the data lacks a kit of the function");
	return {};
}

std::uint64_t playerFaction(Rig& rig)
{
	std::uint64_t id = 0;
	ecsOf(rig.sim).each([&](const state::Id& key, const state::PlayerFaction&) { id = id == 0 ? key.value : std::min(id, key.value); });
	return id;
}

void joinPlayer(Rig& rig)
{
	std::string sid;
	ecsOf(rig.sim).each([&](const state::PlayerFaction&, const state::FactionState& state) { sid = state.templateSid; });
	test::require(!sid.empty(), "the rig has no player faction");
	state::Allegiance side;
	side.factionSid = sid;
	for (const std::uint64_t id : {rig.leader, rig.member})
		rig.entityOf(id).set<state::Allegiance>(side);
}

std::uint64_t makeSite(Rig& rig, double dx, std::vector<state::BuildMaterial> mats, double hp, bool built)
{
	state::PlayerBuilding b;
	b.ownerFaction = playerFaction(rig);
	b.x = kX + dx;
	b.z = kZ;
	b.y = rig.fighterOf(rig.member).y;
	b.built = built;
	b.hp = hp;
	b.maxHp = 0.0;
	for (const state::BuildMaterial& row : mats)
		b.maxHp += row.need;
	b.mats = std::move(mats);
	b.status = buildings::statusOf(b);
	flecs::entity entity = state::createInstance(ecsOf(rig.sim));
	const std::uint64_t id = entity.get<state::Id>()->value;
	entity.set<state::PlayerBuilding>(std::move(b));
	return id;
}

flecs::entity siteEntity(Rig& rig, std::uint64_t id)
{
	flecs::entity found;
	ecsOf(rig.sim).each([&](flecs::entity e, const state::Id& key, const state::PlayerBuilding&)
	{
		if (key.value == id)
			found = e;
	});
	return found;
}
const state::PlayerBuilding& siteOf(Rig& rig, std::uint64_t id) { return *siteEntity(rig, id).get<state::PlayerBuilding>(); }

void give(Rig& rig, std::uint64_t who, const std::string& item, std::uint32_t count)
{
	state::Inventory pack;
	if (const state::Inventory* packed = rig.entityOf(who).get<state::Inventory>())
		pack = *packed;
	state::Inventory::Entry entry;
	entry.itemSid = item;
	entry.count = count;
	test::require(grid::autoplace(pack, grid::Rules(*rig.gd), entry) == 0, "the items did not fit the pack");
	++pack.version;
	rig.entityOf(who).set<state::Inventory>(pack);
}

void addRow(Rig& rig, std::uint64_t who, std::uint16_t type)
{
	state::Jobs jobs;
	if (const state::Jobs* held = rig.entityOf(who).get<state::Jobs>())
		jobs = *held;
	test::require(addJob(jobs, type, 0) > 0, "the job row was not added");
	rig.entityOf(who).set<state::Jobs>(std::move(jobs));
}

void engineer()
{
	const auto boot = [](Rig& rig)
	{
		joinPlayer(rig);
		rig.fighterOf(rig.member).skills.engineer = 100.0;
	};
	{
		// Without the row nothing is built, whatever stands around.
		Rig rig({}, 0);
		boot(rig);
		const std::vector<std::string> items = resourceItems(rig, 1);
		const std::uint64_t site = makeSite(rig, 8.0, {{items[0], 3, 0}}, 0.0, false);
		give(rig, rig.member, items[0], 3);
		rig.tick(300);
		test::require(siteOf(rig, site).mats[0].have == 0 && rig.sim.constructionOrderCount() == 0, "125: a character without the row worked on a site");
	}
	{
		// Materials in the pack -> 71; then the delivered materials let it build -> 2, to the end; then nothing is left and the goal ends.
		Rig rig({}, 0);
		boot(rig);
		const std::vector<std::string> items = resourceItems(rig, 1);
		const std::uint64_t site = makeSite(rig, 8.0, {{items[0], 3, 0}}, 0.0, false);
		give(rig, rig.member, items[0], 3);
		addRow(rig, rig.member, 125);
		test::require(rig.until(30.0, [&] { return siteOf(rig, site).mats[0].have == 3; }), "125: the materials were never delivered (71)");
		const bool finished = rig.until(600.0, [&] { return siteOf(rig, site).built; });
		test::require(finished, "125: the site was never finished (2): hp " + std::to_string(siteOf(rig, site).hp) + " of " + std::to_string(siteOf(rig, site).maxHp) + " orders " + std::to_string(rig.sim.constructionOrderCount()));
		test::require(rig.until(5.0, [&] { return rig.sim.constructionOrderCount() == 0; }), "125: the order outlived a whole building");
		rig.tick(100);
		test::require(rig.sim.constructionOrderCount() == 0, "125: a finished building was ordered again");
		const state::Jobs* jobs = rig.entityOf(rig.member).get<state::Jobs>();
		test::require(jobs != nullptr && jobs->running == 0, "125: the goal is still running on a finished site");
		std::cout << "  125: 71 with the material in the pack, then 2 to the end, then idle\n";
	}
	{
		// Nothing carried and nothing the delivered materials allow: skipped. A damaged building: 95.
		Rig rig({}, 0);
		boot(rig);
		const std::vector<std::string> items = resourceItems(rig, 1);
		const std::uint64_t empty = makeSite(rig, 8.0, {{items[0], 3, 0}}, 0.0, false);
		addRow(rig, rig.member, 125);
		rig.tick(100);
		test::require(rig.sim.constructionOrderCount() == 0 && siteOf(rig, empty).hp == 0.0, "125: worked on a site with no material to build with");
		const std::uint64_t hurt = makeSite(rig, 10.0, {{items[0], 10, 10}}, 10.0, true);
		test::require(!buildings::removeHp(*siteEntity(rig, hurt).get_mut<state::PlayerBuilding>(), 4.0, 1.0), "the hit destroyed the building");
		const double before = siteOf(rig, hurt).hp;
		test::require(rig.until(30.0, [&] { return siteOf(rig, hurt).hp > before; }), "125: the damaged building was not repaired (95)");
		test::require(rig.until(600.0, [&] { return siteOf(rig, hurt).built; }), "125: the repair never finished");
		std::cout << "  125: an unbuildable empty site is skipped, a damaged building is repaired\n";
	}
	{
		// The row removed under a running goal drops its order; a non-player character never gets one.
		Rig rig({}, 0);
		boot(rig);
		const std::vector<std::string> items = resourceItems(rig, 1);
		makeSite(rig, 8.0, {{items[0], 3, 0}}, 0.0, false);
		give(rig, rig.member, items[0], 3);
		addRow(rig, rig.member, 125);
		test::require(rig.until(10.0, [&] { return rig.sim.constructionOrderCount() == 1; }), "125: no order was given");
		state::Jobs jobs = *rig.entityOf(rig.member).get<state::Jobs>();
		jobs.rows.clear();
		rig.entityOf(rig.member).set<state::Jobs>(std::move(jobs));
		rig.tick(30);
		test::require(rig.sim.constructionOrderCount() == 0, "125: the order survived the removed row");

		Rig stranger({}, 0);
		const std::uint64_t site = makeSite(stranger, 8.0, {{items[0], 3, 0}}, 0.0, false);
		give(stranger, stranger.member, items[0], 3);
		addRow(stranger, stranger.member, 125);
		stranger.tick(300);
		test::require(siteOf(stranger, site).mats[0].have == 0 && stranger.sim.constructionOrderCount() == 0, "125: a non-player character built");
		std::cout << "  125: a removed row drops its order, a non-player character is refused\n";
	}
}

double roboticHealth(const state::Fighter& f)
{
	double sum = 0.0;
	for (const state::FighterPart& part : f.parts)
		if (part.robotic)
			sum += part.health + part.pool;
	return sum;
}

void robotics()
{
	const auto hurtRobot = [](Rig& rig, std::uint64_t who)
	{
		state::Fighter& f = rig.fighterOf(who);
		test::require(!f.parts.empty(), "the patient has no parts");
		state::FighterPart& part = f.parts.front();
		part.robotic = true;
		part.health = std::max(0.0, part.maxHealth - part.wear - 30.0);
		part.pool = 0.0;
	};
	{
		Rig rig({}, 0);
		const std::string robotKit = kitOf(rig, kItemFunctionRobotKit);
		hurtRobot(rig, rig.leader);
		rig.tick(2);
		give(rig, rig.member, robotKit, 1);
		const double before = roboticHealth(rig.fighterOf(rig.leader));
		rig.tick(200);
		test::require(roboticHealth(rig.fighterOf(rig.leader)) == before, "57: a character without the row repaired a robot");
		addRow(rig, rig.member, 57);
		test::require(rig.until(20.0, [&] { return roboticHealth(rig.fighterOf(rig.leader)) > before; }), "57: the robot was not repaired");
		std::cout << "  57: a robot repair kit heals the hurt robotic part of an ally\n";
	}
	{
		// A medkit is not a robot kit: the row finds nothing to do. A robot kit and only flesh hurt: nothing either.
		Rig rig({}, 0);
		hurtRobot(rig, rig.leader);
		give(rig, rig.member, kitOf(rig, kItemFunctionMedkit), 1);
		rig.tick(2);
		const double before = roboticHealth(rig.fighterOf(rig.leader));
		addRow(rig, rig.member, 57);
		rig.tick(200);
		test::require(roboticHealth(rig.fighterOf(rig.leader)) == before && rig.entityOf(rig.member).get<state::InteractTask>() == nullptr,
			"57: a medkit repaired a robot");

		Rig flesh({}, 0);
		give(flesh, flesh.member, kitOf(flesh, kItemFunctionRobotKit), 1);
		for (state::FighterPart& part : flesh.fighterOf(flesh.leader).parts)
			if (!part.robotic)
				part.health = std::max(0.0, part.health - 10.0);
		flesh.tick(2);
		addRow(flesh, flesh.member, 57);
		flesh.tick(200);
		test::require(flesh.entityOf(flesh.member).get<state::InteractTask>() == nullptr, "57: a robot kit went to a flesh injury");
		std::cout << "  57: a medkit or a flesh-only patient gives the row nothing to do\n";
	}
}

} // namespace

int main()
{
	try
	{
		engineer();
		robotics();
		std::cout << "ksim_jobs3: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_jobs3: FAILED: " << e.what() << "\n";
		return 1;
	}
}
