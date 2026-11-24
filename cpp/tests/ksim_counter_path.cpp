// Open on a container behind the Hub bar counter (kenshi-k16.8.12, user 2026-11-12 playtest kenshi-2vt; the original's rule is oracle kenshi-rwh.95):
// the order walks the navmesh round the counter's end to the box's use point and acts on arrival there (no opening across the counter, no
// refusal), and a standing NPC is never pushed by a passer-by (the barman at his node stays put while the player walks through him).
// argv[1]: the Hub navmesh cache (`knav_bake`, ctest fixture knav_cache).

#include "trade_rig.hpp"
#include "ksim/dev/build_paths.hpp"

#include <cmath>
#include <cstdio>
#include <sstream>

namespace
{

// The Hub bar's counter runs north-south at x -50563 .. -50552 (customer side west); the stock behind it lies east of x -50550.
constexpr double kCounterEast = -50550.0;
constexpr double kBarZ0 = -1260.0, kBarZ1 = -1160.0;

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc == 2, "usage: ksim_counter_path HUB.knav");
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);
		const auto gd = std::make_shared<const kdata::GameData>(kworld::loadMergedGameData(order));
		const std::shared_ptr<const CombatData> data = CombatData::build(*gd, KENSHI_DATA);
		const std::shared_ptr<MeshNavigator> nav = MeshNavigator::load(argv[1]);
		const std::string saveDir = ks::prepareSaveDir(std::string(KSIM_BINARY_DIR) + "/ksim_counter_path_test/saves");
		Rig r(order, gd, data, nav, saveDir);
		r.newGame("Nobodies");
		const std::uint64_t me = r.members[0];
		std::uint64_t barman = 0;
		r.ecs().each([&](flecs::entity e, const state::SquadInstance& sq, const state::TraderStock&, const state::Position& at)
		{
			const kdata::Record* record = gd->find(sq.templateSid);
			if (record == nullptr || record->name != "Bar blackmarket" || std::hypot(at.x + 50560.0, at.z + 1190.0) > 100.0)
				return;
			r.ecs().query_builder<const state::Id>().with<state::MemberOf>(e).build().each([&](flecs::entity, const state::Id& id)
			{
				barman = barman == 0 ? id.value : barman;
			});
		});
		test::require(barman != 0, "the Hub's Bar blackmarket squad has a barman");
		// The camera looks at the bar: the barman's zone is loaded (rwh.202 section 3: +-340 u round the player only, the Nobodies start is further off).
		r.sim->enqueue(Command::setView(-50560.0, -1190.0));
		r.tick(1200); // the barman walks to his post (ksim_barman_node)

		const auto onMesh = [&](std::uint64_t who)
		{
			const state::Fighter& f = r.fighter(who);
			return nav->walker().step({f.x, f.y, f.z}, 0.0, 0.0).onMesh;
		};
		const auto place = [&](std::uint64_t who, double x, double z, double y = 1077.5)
		{
			state::Fighter& f = r.fighter(who);
			const auto at = nav->walker().snap({x, y, z}, 3.0);
			test::require(at.has_value(), "a mesh point at the bar (" + std::to_string(x) + ", " + std::to_string(z) + ")");
			f.x = at->x;
			f.y = at->y;
			f.z = at->z;
			f.clearRoute();
			r.tick(10);
		};
		// Order 60, action 5 (loot) with the open flag: the walk ends at the box without taking anything.
		const auto open = [&](std::uint64_t box)
		{
			Buf b;
			b.put<std::uint16_t>(1).put<std::uint64_t>(me).put<std::uint64_t>(box).put<std::uint8_t>(5).put<std::uint8_t>(0).put<std::uint8_t>(1);
			return r.send(b, 60);
		};
		struct Box
		{
			std::uint64_t id;
			double x, z;
			bool locked;
		};
		std::vector<Box> behind;
		r.ecs().each([&](const state::Id& id, const state::Container& c)
		{
			if (c.useX > kCounterEast && c.useX < kCounterEast + 40.0 && c.useZ > kBarZ0 && c.useZ < kBarZ1)
				behind.push_back({id.value, c.useX, c.useZ, c.locked});
		});
		test::require(!behind.empty(), "the Hub bar has stock east of the counter");

		// ---- a standing NPC is never pushed: the player walks through the barman at his node, within 3 u ----
		{
			const state::Fighter& man = r.fighter(barman);
			const double bx = man.x;
			const double bz = man.z;
			// Mesh points 14 u either side of him: the straight walk between them goes through him.
			const auto from = nav->walker().snap({bx, man.y, bz - 14.0}, 4.0);
			const auto to = nav->walker().snap({bx, man.y, bz + 14.0}, 4.0);
			test::require(from && to, "a straight mesh line through the barman");
			place(me, from->x, from->z, man.y);
			test::require(r.moveTo(me, to->x, to->z).empty(), "the move order past the barman is accepted");
			// The barman's own errands (he walks to the shop door a few ticks in) are his; only a tick where he stands before and after counts.
			double closest = 1.0e9;
			double nudge = 0.0;
			int standingTicks = 0;
			for (int t = 0; t < 120; ++t)
			{
				const bool walked = man.walking();
				const double px = man.x;
				const double pz = man.z;
				r.tick(1);
				const double d = std::hypot(r.fighter(me).x - bx, r.fighter(me).z - bz);
				if (!walked && !man.walking())
				{
					closest = std::min(closest, d);
					nudge = std::max(nudge, std::hypot(man.x - px, man.z - pz));
					++standingTicks;
				}
			}
			std::printf("closest %.2f u over %d standing ticks, barman moved at most %.4f u in one\n", closest, standingTicks, nudge);
			test::require(closest < 3.0, "the player passed within 3 u of the barman (closest " + std::to_string(closest) + ")");
			test::require(nudge < 0.1, "the barman was pushed " + std::to_string(nudge) + " u by a passer-by");
			test::require(onMesh(barman), "the barman stays on the mesh");
		}

		// ---- from the customer side, within the straight-line reach: the walk goes round, on the mesh, and arrives ----
		int walked = 0;
		for (const Box& box : behind)
		{
			if (box.locked)
				continue;
			place(me, -50575.0, -1200.0);
			const std::size_t seen = r.events.size();
			test::require(open(box.id).empty(), "the open order on a box behind the counter is accepted");
			double travelled = 0.0;
			double px = r.fighter(me).x;
			double pz = r.fighter(me).z;
			int off = 0;
			bool done = false;
			double crossed = 0.0; // the most the player stood inside the counter's footprint (east of its west face, west of the stock)
			for (int t = 0; t < 1200 && !done; ++t)
			{
				r.tick(1);
				const state::Fighter& f = r.fighter(me);
				travelled += std::hypot(f.x - px, f.z - pz);
				px = f.x;
				pz = f.z;
				if (!onMesh(me))
					++off;
				if (f.x > -50563.0 && f.x < kCounterEast - 1.0 && f.z > -1214.0 && f.z < -1166.0)
					crossed = std::max(crossed, std::min(f.x + 50563.0, kCounterEast - f.x));
				for (std::size_t i = seen; i < r.events.size(); ++i)
					done = done || (r.events[i].kind == 13 && r.events[i].b == box.id);
			}
			const state::Fighter& f = r.fighter(me);
			test::require(done, "the open order on box " + std::to_string(box.id) + " ended");
			test::require(off == 0, "the walk to box " + std::to_string(box.id) + " left the mesh");
			test::require(crossed == 0.0, "the walk to box " + std::to_string(box.id) + " went through the counter");
			test::require(travelled > 40.0, "the order on box " + std::to_string(box.id) + " acted across the counter (walked " + std::to_string(travelled) + " u)");
			std::printf("box %llu: ended %.2f u from its use point\n", static_cast<unsigned long long>(box.id), std::hypot(f.x - box.x, f.z - box.z));
			test::require(std::hypot(f.x - box.x, f.z - box.z) <= kArriveContainer && f.x > kCounterEast,
				"the player ended at the use point of box " + std::to_string(box.id) + ", behind the counter (" + std::to_string(std::hypot(f.x - box.x, f.z - box.z)) + " u off)");
			++walked;
		}
		test::require(walked > 0, "an unlocked box behind the counter");

		// ---- a box on the customer side is opened without walking round ----
		{
			place(me, -50575.0, -1200.0);
			// the customer-side box nearest the spot (its id moves with the container set, so it is found, not fixed)
			std::uint64_t counterBox = 0;
			double nearest = 1e18;
			r.ecs().each([&](const state::Id& id, const state::Container& c)
			{
				const double d = std::hypot(c.useX + 50575.0, c.useZ + 1200.0);
				if (c.useX < kCounterEast && d < nearest)
				{
					nearest = d;
					counterBox = id.value;
				}
			});
			test::require(counterBox != 0 && nearest < 20.0, "a box on the customer side of the counter");
			const std::size_t seen = r.events.size();
			test::require(open(counterBox).empty(), "the open order on the counter's own box is accepted");
			r.tick(40);
			bool done = false;
			for (std::size_t i = seen; i < r.events.size(); ++i)
				done = done || (r.events[i].kind == 13 && r.events[i].b == counterBox && r.events[i].result == 0);
			test::require(done && r.fighter(me).x < -50560.0, "a box on the customer side opens from the customer side");
		}

		// ---- two characters on one spot at the counter's west face: both stand on the mesh (standing, so neither is pushed) ----
		{
			test::require(r.members.size() >= 2, "the start squad has a second character");
			const std::uint64_t other = r.members[1];
			place(other, -50565.0, -1187.0);
			place(me, -50565.0, -1187.0);
			int off = 0;
			for (int t = 0; t < 200; ++t)
			{
				r.tick(1);
				off += onMesh(other) ? 0 : 1;
				off += onMesh(me) ? 0 : 1;
			}
			test::require(off == 0, "a character left the mesh");
		}
		std::cout << "ksim_counter_path ok (" << walked << " boxes behind the counter)\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_counter_path: " << e.what() << "\n";
		return 1;
	}
}
