// The journey scenarios (scenarios/journey_walk, journey_mine, journey_sell; kenshi-k16.18.1 / k16.19.1 / k16.20.1): each one loads through
// SCENARIO_LOAD on the Hub world, carries its bead= header on the wire, and the step it asks a playtester to try can be driven by the order
// the client sends: the walk arrives, the mine order makes ore that order 68 takes into the Wanderer's pack, the sale raises the wallet.
// Everything is looked up in the data. argv[1]: the Hub navmesh cache (`knav_bake`, ctest fixture knav_cache).

#include "trade_rig.hpp"

#include "ksim/save/scenario_file.hpp"
#include "ksim/dev/build_paths.hpp"


namespace {

constexpr int kSeed = 5;

// What the world has near the Hub, by data: the barman's squad and body, and the nearest copper deposit the navmesh reaches from `from`.
struct Spots
{
	flecs::entity shopSquad;
	std::uint64_t shop = 0;
	std::uint64_t keeper = 0;
	double barX = 0.0, barZ = 0.0;
	ProducerSite site;
	double useX = 0.0, useY = 0.0, useZ = 0.0;
	std::string ore;
};

Spots findSpots(Rig& e, std::uint64_t hero)
{
	Spots s;
	double best = 1e18;
	e.ecs().each([&](flecs::entity entity, const state::SquadInstance& sq, const state::TraderStock&, const state::Position& at)
	{
		const kdata::Record* t = e.gd->find(sq.templateSid);
		const double d = std::hypot(at.x - e.fighter(hero).x, at.z - e.fighter(hero).z);
		if (t != nullptr && t->name == "Bar blackmarket" && d < best)
		{
			best = d;
			s.shopSquad = entity;
		}
	});
	test::require(s.shopSquad.is_valid(), "the Hub barman's squad");
	e.ecs().query_builder<const state::Id>().with<state::MemberOf>(s.shopSquad).build().each([&](flecs::entity, const state::Id& id) { s.keeper = s.keeper == 0 ? id.value : s.keeper; });
	s.shop = s.shopSquad.get<state::Id>()->value;
	s.barX = e.fighter(s.keeper).x;
	s.barZ = e.fighter(s.keeper).z;

	const producers::Book book(*e.gd);
	const kworld::ZoneCell hub = kworld::zoneCellOf(ks::kHubX, ks::kHubZ);
	double bestDistance = 1e18;
	bool found = false;
	for (int cz = hub.z - 2; cz <= hub.z + 2; ++cz)
	{
		for (int cx = hub.x - 2; cx <= hub.x + 2; ++cx)
		{
			for (const ProducerSite& candidate : e.sim->wildProducerSites(cx, cz))
			{
				const producers::Spec* spec = book.find(candidate.recordSid);
				if (spec == nullptr || spec->resource != 3)
					continue;
				const auto spot = e.nav->walker().snap({candidate.x, candidate.y, candidate.z}, producers::kUseSnap);
				if (!spot)
					continue;
				const auto route = e.nav->routeFrom({e.fighter(hero).x, e.fighter(hero).y, e.fighter(hero).z}, spot->x, spot->z, spot->y, false,
					e.sim->pathRadius(e.fighter(hero)));
				if (!route.found || route.points.empty() || (route.approach
					&& std::hypot(route.points.back().x - spot->x, route.points.back().z - spot->z) > producers::kUseSnap))
					continue;
				const double d = std::hypot(candidate.x - s.barX, candidate.z - s.barZ);
				if (d >= bestDistance)
					continue;
				found = true;
				bestDistance = d;
				s.site = candidate;
				s.useX = spot->x;
				s.useY = spot->y;
				s.useZ = spot->z;
				s.ore = book.find(candidate.recordSid)->output;
			}
		}
	}
	test::require(found, "a wild copper deposit on the Hub navmesh");
	return s;
}

// SCENARIO_LOAD `name` on the rig's admin session; the answer's bead tail is returned. The load's full resend puts the Wanderer on the wire.
std::string loadScenario(Rig& e, const std::string& name)
{
	e.connect(true);
	e.members.clear();
	e.money.clear();
	Buf b;
	b.put<std::uint32_t>(7000).str(name).put<std::uint32_t>(kSeed);
	std::vector<std::uint8_t> bytes;
	ks::BinaryWriter w(bytes);
	test::require(e.session->onFrame(ks::kScenarioLoad, b.bytes.data(), b.bytes.size(), w), "SCENARIO_LOAD closed the session");
	e.collect(bytes);
	std::string bead;
	for (const Frame& frame : frames(bytes))
	{
		if (frame.type != ks::kScenarioStarted)
			continue;
		Cursor c{frame.payload};
		c.get<std::uint32_t>();
		test::require(c.get<std::uint8_t>() == 0, "SCENARIO_LOAD " + name + " was refused");
		const auto str = [&]()
		{
			const std::uint16_t n = c.get<std::uint16_t>();
			std::string s(reinterpret_cast<const char*>(&frame.payload[c.at]), n);
			c.at += n;
			return s;
		};
		str(); // error
		str(); // name
		c.get<std::uint64_t>(); // fileHash
		c.get<std::uint32_t>(); // seed
		str(); // simHash
		c.get<double>(); // startGameTime
		c.get<float>();
		c.get<float>(); // the centre tail
		bead = str();
		test::require(c.done(), "SCENARIO_STARTED has trailing bytes");
	}
	e.tick(2);
	test::require(!e.members.empty(), name + ": the Wanderer is on the wire");
	return bead;
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc == 2, "usage: ksim_journey HUB.knav");
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);
		const auto gd = std::make_shared<const kdata::GameData>(kworld::loadMergedGameData(order));
		const std::shared_ptr<const CombatData> data = CombatData::build(*gd, KENSHI_DATA);
		const std::shared_ptr<MeshNavigator> nav = MeshNavigator::load(argv[1]);
		const std::string base = std::string(KSIM_BINARY_DIR) + "/ksim_journey_test";
		fs::remove_all(base);
		Rig e(order, gd, data, nav, ks::prepareSaveDir(base + "/saves"));
		e.context.scenariosDir = KSIM_SCENARIOS_DIR;

		const auto copperOf = [&](const std::string& sid) { return gd->find(sid) != nullptr && gd->find(sid)->name == "Copper"; };
		const auto advance = [&](int n, std::uint64_t& ticks) { e.tick(n); ticks += static_cast<std::uint64_t>(n); };

		// ---- journey_walk: click to move across the Hub, the Wanderer arrives ----
		{
			test::require(loadScenario(e, "journey_walk") == "kenshi-k16.18", "journey_walk: the bead on SCENARIO_STARTED");
			e.tick(60); // the residents become Fighters
			const std::uint64_t hero = e.members[0];
			const Spots spots = findSpots(e, hero);
			const double start = std::hypot(e.fighter(hero).x - spots.barX, e.fighter(hero).z - spots.barZ);
			test::require(start > 250.0 && start < 450.0, "journey_walk: the Wanderer starts a few hundred u from the Bar (" + std::to_string(start) + ")");
			const auto goal = nav->walker().snap({spots.barX, e.fighter(spots.keeper).y, spots.barZ}, 60.0);
			test::require(goal.has_value(), "journey_walk: the ground by the Bar is on the navmesh");
			test::require(e.moveTo(hero, goal->x, goal->z, false, goal->y).empty(), "journey_walk: move_to accepted");
			std::uint64_t ticks = 0;
			int guard = 0;
			while (std::hypot(e.fighter(hero).x - goal->x, e.fighter(hero).z - goal->z) > 20.0 && guard++ < 400)
				advance(10, ticks);
			test::require(guard < 400, "journey_walk: the Wanderer reached the Bar");
			std::cout << "journey_walk: " << start << " u to the Bar in " << static_cast<double>(ticks) * kTickSeconds << " s\n";
		}

		// ---- journey_mine: the mine order makes ore, order 68 takes it into the pack ----
		{
			test::require(loadScenario(e, "journey_mine") == "kenshi-k16.19", "journey_mine: the bead on SCENARIO_STARTED");
			e.tick(60);
			const std::uint64_t hero = e.members[0];
			const Spots spots = findSpots(e, hero);
			test::require(copperOf(spots.ore), "journey_mine: the deposit makes Copper");
			const double near = std::hypot(e.fighter(hero).x - spots.useX, e.fighter(hero).z - spots.useZ);
			test::require(near > 20.0 && near < 120.0, "journey_mine: the Wanderer starts by the deposit (" + std::to_string(near) + " u from its use spot)");
			test::require(e.carried(hero, spots.ore) == 0, "journey_mine: no ore yet");
			std::uint64_t ticks = 0;
			const auto drawn = [&]() { return e.sim->combat()->find(hero)->weapon.weaponType != 5; }; // CHAR kFlagDrawn's source
			const bool armed = drawn();
			test::require(e.interact(hero, spots.site.id, 4).empty(), "journey_mine: the use order is accepted");
			int guard = 0;
			while (e.carried(hero, spots.ore) == 0 && e.entityWithId(spots.site.id).get<state::Inventory>()->carried(spots.ore) == 0 && guard++ < 6000)
				advance(20, ticks);
			test::require(guard < 6000, "journey_mine: one ore was made");
			// rwh.272: Task_OperateMachine start sheathes the weapon (unconditional) and the stop never redraws it.
			test::require(!drawn(), "journey_mine: kFlagDrawn is off while operating (armed at start: " + std::to_string(armed) + ")");
			advance(5, ticks);
			const Inv& deposit = e.inv.at(spots.site.id);
			test::require(deposit.items.size() == 1 && std::get<1>(deposit.items[0]) >= 1, "journey_mine: the deposit's INVENTORY holds the ore");
			test::require(e.transfer68(hero, spots.site.id, deposit.version, 0, 1, true).empty(), "journey_mine: take the ore with order 68");
			advance(3, ticks);
			test::require(e.carried(hero, spots.ore) == 1, "journey_mine: the Wanderer carries one Copper");
			test::require(!drawn(), "journey_mine: kFlagDrawn stays off after the order (no redraw)");
			std::cout << "journey_mine: one Copper after " << static_cast<double>(ticks) * kTickSeconds << " s of sim time\n";
		}

		// ---- journey_sell: talk to the barman, sell the Copper the file put in the pack ----
		{
			test::require(loadScenario(e, "journey_sell") == "kenshi-k16.20", "journey_sell: the bead on SCENARIO_STARTED");
			e.tick(60);
			const std::uint64_t hero = e.members[0];
			const Spots spots = findSpots(e, hero);
			test::require(copperOf(spots.ore) && e.carried(hero, spots.ore) == 1, "journey_sell: the Wanderer carries one Copper");
			const double near = std::hypot(e.fighter(hero).x - spots.barX, e.fighter(hero).z - spots.barZ);
			test::require(near > 20.0 && near < 120.0, "journey_sell: the Wanderer starts by the barman (" + std::to_string(near) + " u)");
			const std::int64_t money0 = e.wallet();
			const std::size_t trades0 = e.trades.size();
			test::require(e.talk(hero, spots.keeper).empty(), "journey_sell: talk to the barman accepted");
			std::uint64_t ticks = 0;
			int guard = 0;
			while (e.trades.size() == trades0 && guard++ < 1000)
				advance(20, ticks);
			test::require(e.trades.size() > trades0, "journey_sell: the trade window opened");
			const trade::Book& book = e.sim->tradeBook();
			const int unit = book.quote(book.termsOf(spots.shopSquad), state::Inventory::Entry{spots.ore, 1, 0, {}}).sell;
			test::require(unit > 0, "journey_sell: the barman buys Copper");
			const int at = Rig::indexOf(e.inv.at(hero), spots.ore);
			test::require(at >= 0, "journey_sell: the Copper is in the Wanderer's INVENTORY");
			test::require(e.transfer68(hero, spots.shop, e.inv.at(hero).version, static_cast<std::uint16_t>(at), 1, false).empty(), "journey_sell: sell accepted");
			advance(3, ticks);
			test::require(e.carried(hero, spots.ore) == 0 && e.wallet() == money0 + unit, "journey_sell: the cats rose by the price: " + std::to_string(money0) + " -> " + std::to_string(e.wallet()));
			std::cout << "journey_sell: sold 1 Copper for " << unit << " cats\n";
		}
		std::cout << "ksim_journey ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_journey FAILED: " << e.what() << "\n";
		return 1;
	}
}
