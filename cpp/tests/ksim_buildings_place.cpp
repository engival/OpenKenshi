// Player buildings, Phase 1 (docs/reference/BUILDINGS_SLICE.md sections 2, 3, 12, 13): orders 90 build_check / 91 build_place with the
// checks the slice builds, the entity (hp 0, the record's materials, a PlayerTown), BUILDING 47 decoded by the client's own decoder
// (its trailing-bytes check catches layout drift), save, load and continue.
// argv[1]: the Hub navmesh cache (`knav_bake`, ctest fixture knav_cache).

#include "buildings_rig.hpp"

#include <sstream>

namespace {

using namespace bt;

bool near(double a, double b, double eps) { return std::fabs(a - b) <= eps; }

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc == 2, "usage: ksim_buildings_place HUB.knav");
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);
		const auto gd = std::make_shared<const kdata::GameData>(kworld::loadMergedGameData(order));
		const std::shared_ptr<const CombatData> data = CombatData::build(*gd, KENSHI_DATA);
		const std::shared_ptr<MeshNavigator> nav = MeshNavigator::load(argv[1]);

		// ---- no player faction: no_faction, before anything else ----
		{
			SpawnOptions options;
			options.seed = 7;
			Simulation bare = Simulation::newGame(KENSHI_DATA, order, options);
			BuildOrder o;
			o.record = "4092-TwoStorey.mod";
			test::require(bare.checkBuildPlace(o) == "no_faction", "no faction: no_faction");
			std::cout << "no faction ok\n";
		}

		Rig r(order, gd, data, nav);
		test::require(r.buildingFrames == 0 && r.buildingIds().empty(), "connect: no BUILDING frame while there are no buildings");
		const kdata::Record* shack = r.byName("Small Shack");
		test::require(shack != nullptr, "the Small Shack record");
		const std::vector<std::string> unlocked = r.unlocked();
		test::require(std::find(unlocked.begin(), unlocked.end(), shack->stringID) != unlocked.end(), "the Small Shack is unlocked at the start");

		const std::optional<BuildOrder> found = r.findSite(shack->stringID);
		test::require(found.has_value(), "a valid Small Shack site exists in the scan");
		BuildOrder site = *found;
		std::cout << "site " << site.x << " " << site.y << " " << site.z << "\n";

		// ---- the refusals ----
		{
			const kdata::Record* wind = r.byName("Wind Generator");
			test::require(wind != nullptr && std::find(unlocked.begin(), unlocked.end(), wind->stringID) == unlocked.end(), "the Wind Generator is not unlocked at the start");
			BuildOrder o = site;
			o.record = wind->stringID;
			test::require(r.check(o) == "locked", "a locked record");
			o = site;
			o.record = "no-such-record";
			test::require(r.check(o) == "locked", "an unknown record is locked too");
			o = site;
			o.floor = 4;
			test::require(r.check(o) == "bad_value", "floor 4");
			o = site;
			o.qw = 0.0;
			test::require(r.check(o) == "bad_value", "a zero quaternion");
			o = site;
			o.y += 100.0;
			test::require(r.check(o) == "bad_value", "a site 100 above the ground");
			o = site;
			o.x = 400000.0;
			test::require(r.check(o) == "bad_value", "outside the world");
			// A record whose placement tests are not built is refused, never accepted: a mine, a well, a farm, a piece that snaps.
			for (const char* name : {"Stone Mine", "Well", "Vegetable Farm S", "Small Research Bench"})
			{
				const kdata::Record* rec = r.byName(name);
				test::require(rec != nullptr, std::string("the ") + name + " record");
				o = site;
				o.record = rec->stringID;
				test::require(r.check(o) == "unsupported_site", std::string(name) + " is unsupported_site");
			}
			// Near a town: refused as unsupported_site until Q2 / Q3 are answered.
			double tx = 0.0, tz = 0.0;
			bool haveTown = false;
			r.sim.world().ecs().each([&](const state::TownState& t) { if (!haveTown && (t.x != 0.0 || t.z != 0.0)) { tx = t.x; tz = t.z; haveTown = true; } });
			test::require(haveTown, "a town to stand next to");
			o = site;
			o.x = tx + 100.0;
			o.z = tz;
			o.y = *r.sim.groundHeight(o.x, o.z);
			test::require(r.check(o) == "too_close_town", "100 units from a town: too_close_town");
			std::cout << "refusals ok\n";
		}

		// ---- the town test on the exe's live probes (item50_buildings.md), the radius, the ground and the slope rules ----
		{
			namespace bd = ksim::buildings;
			std::uint64_t faction = 0;
			r.sim.world().ecs().each([&](const state::Id& i, const state::PlayerFaction&) { faction = i.value; });
			const auto townAt = [&](const std::string& name, const kdata::Record*& rec, double& x, double& z)
			{
				rec = nullptr;
				for (const kdata::Record* t : gd->byType(kworld::itemTypeByName("TOWN")))
				{
					if (t->name == name)
						rec = t;
				}
				test::require(rec != nullptr, "the TOWN " + name);
				bool at = false;
				r.sim.world().ecs().each([&](const state::TownState& t) { if (t.rootSid == rec->stringID && !at) { x = t.x; z = t.z; at = true; } });
				test::require(at, name + " has a position");
			};
			// Along some direction the nearest town stays the one tested, so the boundary is exact there.
			const auto probe = [&](const std::string& town, const std::string& recordName, double blocked, double clear)
			{
				const kdata::Record* t = nullptr;
				double tx = 0.0, tz = 0.0;
				townAt(town, t, tx, tz);
				const kdata::Record* rec = r.byName(recordName);
				test::require(rec != nullptr, recordName);
				const bool creates = rec->boolOr("creates player town", true);
				for (int i = 0; i < 64; ++i)
				{
					const double a = i * 3.14159265358979 / 32.0;
					const std::string inside = r.sim.townBlocker(tx + blocked * std::cos(a), tz + blocked * std::sin(a), creates, faction);
					const std::string outside = r.sim.townBlocker(tx + clear * std::cos(a), tz + clear * std::sin(a), creates, faction);
					if (inside == t->stringID && outside.empty())
						return;
				}
				throw test::Failure(recordName + " vs " + town + ": no direction blocks at " + std::to_string(blocked) + " and clears at " + std::to_string(clear));
			};
			probe("Stoat", "Wind Generator", 6200.0, 6300.0); // TOWN, mult 1: 2500 x 2.5 = 6250
			probe("Stoat", "Campfire", 2400.0, 2600.0); // `creates player town` false: 2500 x 1.0
			probe("Ravaged Hamlet", "Wind Generator", 1900.0, 2100.0); // POI: the 2000 floor
			probe("Ravaged Hamlet", "Campfire", 400.0, 500.0); // flag false: 437.5, no floor
			test::require(!r.byName("Campfire")->boolOr("creates player town", true) && r.byName("Wind Generator")->boolOr("creates player town", true), "the probes' flags");
			test::require(bd::townRadius(2, 1.0) == 2500.0 && bd::townRadius(2, 0.05) == 2500.0 && bd::townRadius(2, 10.0) == 12500.0 && bd::townRadius(2, 0.2) == 500.0, "radius = base x clamped mult (below 0.1 reads 1.0)");
			test::require(bd::townRadius(3, 1.0) == 937.5 && bd::townRadius(4, 1.0) == 875.0 && bd::townRadius(9, 1.0) == 437.5 && bd::townRadius(0, 1.0) == 1250.0
				&& bd::townRadius(8, 1.0) == 0.0 && bd::townRadius(10, 1.0) == 0.0, "the base by type");
			test::require(bd::townBlocks(4, 875.0, 1999.0, true) && !bd::townBlocks(4, 875.0, 2001.0, true) && bd::townBlocks(9, 437.5, 1999.0, true) && bd::townBlocks(9, 437.5, 436.0, false) && !bd::townBlocks(9, 437.5, 500.0, false)
				&& !bd::townBlocks(8, 5000.0, 1.0, true), "RUINS / POI use k 1.0 with the 2000 floor; a marker never blocks");
			const bd::TownShape one = bd::playerTownShape({{100.0, 200.0}});
			test::require(one.x == 100.0 && one.z == 200.0 && one.radius == 1000.0, "one building: its own centre, radius clamped up to 1000");
			const bd::TownShape two = bd::playerTownShape({{0.0, 0.0}, {3000.0, 0.0}});
			test::require(two.x == 1500.0 && two.radius == 1700.0, "the mean centre and max(dist) + 200");
			test::require(bd::playerTownShape({{0.0, 0.0}, {100000.0, 0.0}}).radius == 20000.0 && bd::playerTownShape({}).radius == 2500.0, "radius clamps to 20000; no buildings reads 2500");
			// Q4: only LAND parts are tested, at 98.0
			test::require(bd::groundBlocked({1}, 97.9) && !bd::groundBlocked({1}, 98.0) && !bd::groundBlocked({0, 2}, -50.0) && bd::groundBlocked({0, 1}, 0.0) && !bd::groundBlocked({}, 0.0), "ground: LAND needs Y >= 98");
			// Q5: normal.y >= 0.2 always; with `match slope` a tilt above max slope refuses; without it the tilt is clamped
			const double flat[4] = {5, 5, 5, 5};
			const double mild[4] = {0, 0, 10, 10}; // tilt atan(10 / 20) = 26.6 degrees
			const double steep[4] = {0, 0, 100, 100}; // normal.y 0.196
			test::require(!bd::slopeBlocked(flat, 10, 10, true, 25) && bd::slopeBlocked(mild, 10, 10, true, 25) && !bd::slopeBlocked(mild, 10, 10, false, 25)
				&& !bd::slopeBlocked(mild, 10, 10, true, 30) && bd::slopeBlocked(steep, 10, 10, false, 25), "slope: match slope refuses above max slope, otherwise only normal.y < 0.2 does");
			test::require(near(bd::builderFactor(1), 1.0, 1e-12) && near(bd::builderFactor(2), 1.75, 1e-12) && near(bd::builderFactor(3), 2.3125, 1e-12) && near(bd::builderFactor(4), 2.734375, 1e-12)
				&& bd::builderFactor(100) < 4.0 && bd::builderFactor(0) == 0.0, "builders add with a 0.75^k damping");
			test::require(r.buildingIds().empty() && r.buildingFrames == 0, "checks change nothing");
			std::cout << "town and ground ok\n";
		}

		// ---- a character in the way, then the dry run, then the placement ----
		{
			const std::uint64_t who = r.squad.players[0];
			state::Fighter& f = r.fighter(who);
			const double ox = f.x, oy = f.y, oz = f.z;
			f.x = site.x;
			f.y = site.y;
			f.z = site.z;
			test::require(r.check(site) == "characters_in_way", "a character on the site");
			f.x = ox;
			f.y = oy;
			f.z = oz;
			test::require(r.check(site).empty(), "the dry run on the free site is answered with event 9");
			r.tick(3);
			test::require(r.buildingIds().empty() && r.buildingFrames == 0, "build_check creates nothing and sends nothing");
		}

		const std::uint64_t id = r.placeOk(site);
		const state::PlayerBuilding* b = r.building(id);
		test::require(b != nullptr && b->recordSid == shack->stringID, "the entity holds the record");
		test::require(b->hp == 0.0 && !b->built && b->maxHp == 5.0 && b->workers == 0 && !b->paused && b->switchedOn, "a new site: hp 0, not built, maxHp 5");
		const auto cost = buildings::buildingCost(*shack);
		test::require(b->mats == cost && cost.size() >= 1 && buildings::totalMaterials(cost) == 5, "the materials are the record's construction list, none delivered");
		test::require(b->status == buildings::kNeedsMaterials, "at the material cap with nothing delivered");
		test::require(b->town != 0, "the shack creates a player town");
		std::uint64_t townMembers = 0;
		std::string townName;
		r.sim.world().ecs().each([&](const state::Id& i, const state::PlayerTown& t)
		{
			if (i.value == b->town)
			{
				townName = t.name;
				townMembers = t.members.size() == 1 && t.members[0] == id && t.owner == b->ownerFaction ? 1 : 0;
			}
		});
		{
			double radius = 0.0, tx = 0.0, tz = 0.0;
			r.sim.world().ecs().each([&](const state::Id& i, const state::PlayerTown& t) { if (i.value == b->town) { radius = t.radius; tx = t.x; tz = t.z; } });
			test::require(radius == 1000.0 && tx == site.x && tz == site.z, "one building: the town is centred on it with radius 1000");
		}
		test::require(townName == "Your Outpost" && townMembers == 1, "the town is \"Your Outpost\" with the building as its member");
		test::require(r.buildingFrames == 1 && r.buildingIds().size() == 1, "one BUILDING frame on first sight");
		{
			const auto& d = r.dec.buildings.at(id);
			test::require(r.dec.str(d.record) == shack->stringID, "wire: record");
			test::require(d.owner == b->ownerFaction && d.town == b->town, "wire: owner is the faction Id (u64), town the PlayerTown Id");
			test::require(near(d.pos[0], site.x, 0.01) && near(d.pos[1], site.y, 0.01) && near(d.pos[2], site.z, 0.01) && d.rot[0] == 1.0f && d.rot[1] == 0.0f, "wire: placement");
			test::require(d.floor == 0 && d.status == buildings::kNeedsMaterials && d.flags == 8u, "wire: floor, status, flags (switchedOn only)");
			test::require(d.progress == 0.0f && d.rate == 0.0f && d.hp == 0.0f && d.maxHp == 5.0f, "wire: progress, rate, hp, maxHp");
			test::require(d.materials.size() == cost.size() && r.dec.str(d.materials[0].item) == cost[0].itemSid && d.materials[0].have == 0
				&& d.materials[0].need == cost[0].need, "wire: the material rows");
			test::require(d.quality == 1.0f && d.powerValue == 0.0f && d.efficiency == 0.0f && d.workers == 0 && d.version == 1, "wire: neutral power / worker / efficiency fields, quality 1.0, version 1");
		}
		r.buildingFrames = 0;
		r.tick(200);
		test::require(r.buildingFrames == 0, "no resend while nothing changes");
		test::require(r.check(site) == "overlap", "the same site again: overlap");
		BuildOrder again = site;
		again.record = shack->stringID;
		test::require(r.place(again) == "overlap", "build_place on it: overlap");
		std::cout << "place ok\n";

		// ---- a reconnect re-sends it; save, load and continue ----
		{
			std::ostringstream saved;
			r.sim.save(saved);
			std::istringstream in(saved.str());
			Simulation loaded = Simulation::restore(World::load(KENSHI_DATA), in, {}, false, nullptr, r.sim.gameData());
			loaded.enableCombat(data, Rig::settings());
			loaded.enableNavigation(nav);
			test::require(loaded.stateHash() == r.sim.stateHash(), "the hash changed across save and load");
			const auto views = loaded.playerBuildings();
			test::require(views.size() == 1 && views[0].id == id && views[0].record == shack->stringID && views[0].version == 1 && views[0].town == b->town,
				"the building is in the save");
			::simlink::SimLinkDecoder dec2;
			ks::Session s2(loaded, r.context);
			std::vector<std::uint8_t> bytes;
			ks::BinaryWriter w(bytes);
			s2.begin(w);
			Buf hello;
			hello.put<std::uint32_t>(ks::kMagic).put<std::uint16_t>(ks::kProtocol).put<std::uint8_t>(0).put<std::uint64_t>(r.context.dataHash).str("test");
			test::require(s2.onFrame(ks::kHello, hello.bytes.data(), hello.bytes.size(), w), "hello refused");
			s2.sendTick(w, true);
			for (std::size_t at = 0; at < bytes.size();)
			{
				std::uint32_t len = 0;
				std::memcpy(&len, &bytes[at], 4);
				const std::vector<std::uint8_t> view = test::clientFrame(&bytes[at], 4u + len);
				test::require(dec2.feed(view.data(), view.size()), "decoder: " + dec2.error());
				at += 4 + len;
			}
			test::require(dec2.buildings.count(id) == 1 && dec2.buildings.at(id).owner == r.dec.buildings.at(id).owner, "the connect after a load carries the building");
			r.sim.run(100);
			loaded.run(100);
			test::require(loaded.stateHash() == r.sim.stateHash(), "save, load and continue differs from the uninterrupted run");
			std::cout << "save ok\n";
		}
		std::cout << "ksim_buildings_place ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_buildings_place FAILED: " << e.what() << "\n";
		return 1;
	}
}
