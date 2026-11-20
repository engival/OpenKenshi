// Item provenance, the stolen mark (kenshi-k16.6.7; kinspect dt43 D, findings/dt43_stolen.md; ARCHITECTURE.md "Item provenance"):
//  1. the first victim sticks: a stolen stack taken from another faction's box keeps its first victim;
//  2. a stack merges with the same victim only (A + A one stack, A + B two);
//  3. the mark clears when the stack goes into a box of the victim faction, into the pack or body of a victim-faction character, and when the
//     victim faction's own character takes it;
//  4. the mark clears into shop stock (a fence launders), and the fencing crime still reads the mark the stack was sold with.
// The Hub barman after a new game (the ksim_trade rig). argv[1]: the Hub navmesh cache (`knav_bake`, ctest fixture knav_cache).

#include "trade_rig.hpp"
#include "ksim/dev/build_paths.hpp"

namespace
{

constexpr const char* kVictimA = "stolen-mark-victim-a";
constexpr const char* kVictimB = "stolen-mark-victim-b";

// `count` of `item` in `who`'s pack as one stack stolen from `victim`.
void giveStolen(Rig& r, std::uint64_t who, const std::string& item, std::uint32_t count, const std::string& victim)
{
	flecs::entity e = r.entityWithId(who);
	state::Inventory pack = *e.get<state::Inventory>();
	state::Inventory::Entry entry;
	entry.itemSid = item;
	entry.count = count;
	entry.stolen = victim;
	entry.slot = state::Inventory::kCarried;
	test::require(grid::autoplace(pack, grid::Rules(*r.gd), entry) == 0, "giveStolen: no room");
	++pack.version;
	e.set<state::Inventory>(std::move(pack));
}

// The stacks of `item` an inventory holds, as (count, stolen mark) in order.
std::vector<std::pair<std::uint32_t, std::string>> stacksOf(const state::Inventory& inv, const std::string& item)
{
	std::vector<std::pair<std::uint32_t, std::string>> out;
	for (const state::Inventory::Entry& entry : inv.items)
		if (entry.itemSid == item)
			out.push_back({entry.count, entry.stolen});
	return out;
}

std::string describe(const std::vector<std::pair<std::uint32_t, std::string>>& stacks)
{
	std::string text;
	for (const auto& [count, mark] : stacks)
		text += " [" + std::to_string(count) + " from '" + mark + "']";
	return text.empty() ? " none" : text;
}

// A storage box of `owner` at (x, z), on the ground level of the first character, with `count` of `item`.
std::uint64_t boxAt(Rig& r, const std::string& owner, double x, double z, const std::string& item, std::uint32_t count)
{
	const std::uint64_t id = r.spawnChest(owner, x, z, item, count);
	flecs::entity box = r.entityWithId(id);
	state::Container container = *box.get<state::Container>();
	container.y = container.useY = r.fighter(r.members[0]).y;
	box.set<state::Container>(std::move(container));
	return id;
}

// A stolen stack of `item` lying in a box of `owner`.
std::uint64_t chestWith(Rig& r, const std::string& owner, double x, double z, const std::string& item, std::uint32_t count, const std::string& mark)
{
	const std::uint64_t id = boxAt(r, owner, x, z, item, count);
	flecs::entity box = r.entityWithId(id);
	state::Inventory stock = *box.get<state::Inventory>();
	for (state::Inventory::Entry& entry : stock.items)
		if (entry.itemSid == item)
			entry.stolen = mark;
	++stock.version;
	box.set<state::Inventory>(std::move(stock));
	return id;
}

// `who`'s pack without any `item`.
void dropAll(Rig& r, std::uint64_t who, const std::string& item)
{
	flecs::entity e = r.entityWithId(who);
	state::Inventory pack = *e.get<state::Inventory>();
	pack.items.erase(std::remove_if(pack.items.begin(), pack.items.end(), [&](const state::Inventory::Entry& entry) { return entry.itemSid == item; }), pack.items.end());
	++pack.version;
	e.set<state::Inventory>(std::move(pack));
}

void setFaction(Rig& r, std::uint64_t who, const std::string& sid)
{
	state::Allegiance side;
	side.factionSid = sid;
	r.entityWithId(who).set<state::Allegiance>(side);
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc == 2, "usage: ksim_stolen_mark HUB.knav");
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);
		const auto gd = std::make_shared<const kdata::GameData>(kworld::loadMergedGameData(order));
		const std::shared_ptr<const CombatData> data = CombatData::build(*gd, KENSHI_DATA);
		const std::shared_ptr<MeshNavigator> nav = MeshNavigator::load(argv[1]);
		const std::string base = std::string(KSIM_BINARY_DIR) + "/ksim_stolen_mark_test";
		fs::remove_all(base);
		const std::string saveDir = ks::prepareSaveDir(base + "/saves");

		std::string iron;
		for (const kdata::Record* record : gd->byType(kworld::itemTypeByName("ITEM")))
			if (record->name == "Raw Iron")
				iron = record->stringID;
		test::require(!iron.empty(), "the data has no Raw Iron");

		std::string stk; // a plain ITEM that stacks, so that "merge" means something (Raw Iron's cap is 1)
		for (const kdata::Record* record : gd->byType(kworld::itemTypeByName("ITEM")))
			if (stk.empty() && grid::Rules(*gd).stackCap(record->stringID, nullptr) >= 8)
				stk = record->stringID;
		test::require(!stk.empty(), "no stackable ITEM");

		Rig r(order, gd, data, nav, saveDir);
		r.newGame("Nobodies");
		const std::uint64_t me = r.members[0];
		const std::uint64_t buddy = r.members[1];
		r.tick(60);
		const_cast<state::Fighter&>(r.fighter(me)).skills.thievery = 100.0; // a sure take (steal chance 127%): this test is about the mark, not the roll
		const state::Fighter& hero = r.fighter(me);
		const double x = hero.x;
		const double z = hero.z;
		r.teleport(buddy, x + 1.0, z);
		const auto packOf = [&](std::uint64_t who) -> const state::Inventory& { return *r.entityWithId(who).get<state::Inventory>(); };
		const auto boxOf = [&](std::uint64_t id) -> const state::Inventory& { return *r.entityWithId(id).get<state::Inventory>(); };

		// ---- 1. the first victim sticks ----
		{
			const std::uint64_t chest = chestWith(r, kVictimB, x + 1.0, z + 1.0, stk, 4, kVictimA);
			const std::string why1 = r.transfer63(me, chest, stk, 4, true);
			test::require(why1.empty(), "take from B's box refused: " + why1);
			r.tick(2);
			const auto mine = stacksOf(packOf(me), stk);
			test::require(mine.size() == 1 && mine[0].first == 4 && mine[0].second == kVictimA,
				"the first victim did not stick: the stack taken from B's box reads" + describe(mine));
			std::cout << "first victim sticks:" << describe(mine) << "\n";
			dropAll(r, me, stk);
		}

		// ---- 2. a stack merges with the same victim only ----
		{
			giveStolen(r, me, stk, 2, kVictimA);
			const std::uint64_t chest = chestWith(r, kVictimB, x + 1.0, z + 1.0, stk, 3, kVictimA);
			test::require(r.transfer63(me, chest, stk, 3, true).empty(), "take of a stack stolen from A refused");
			r.tick(2);
			auto mine = stacksOf(packOf(me), stk);
			test::require(mine.size() == 1 && mine[0].first == 5 && mine[0].second == kVictimA, "the same victim's stacks did not merge:" + describe(mine));
			const std::uint64_t other = chestWith(r, kVictimA, x + 1.0, z + 1.0, stk, 2, kVictimB);
			test::require(r.transfer63(me, other, stk, 2, true).empty(), "take of a stack stolen from B refused");
			r.tick(2);
			mine = stacksOf(packOf(me), stk);
			test::require(mine.size() == 2 && mine[0].first == 5 && mine[0].second == kVictimA && mine[1].first == 2 && mine[1].second == kVictimB,
				"another victim's stack merged or the first one split:" + describe(mine));
			std::cout << "merge by victim:" << describe(mine) << "\n";
			dropAll(r, me, stk);
		}

		// ---- 3. the mark clears at the victim faction's own box, pack and body ----
		{
			giveStolen(r, me, stk, 3, kVictimA);
			const std::uint64_t home = boxAt(r, kVictimA, x + 1.0, z + 1.0, stk, 1); // a clean unit already in A's own box
			test::require(r.transfer63(me, home, stk, 3, false).empty(), "put into A's box refused");
			r.tick(2);
			const auto inBox = stacksOf(boxOf(home), stk);
			test::require(inBox.size() == 1 && inBox[0].first == 4 && inBox[0].second.empty(), "the mark did not clear into the victim faction's box:" + describe(inBox));
			test::require(stacksOf(packOf(me), stk).empty(), "the pack kept units after the put");
			std::cout << "into the owner's box:" << describe(inBox) << "\n";

			giveStolen(r, me, stk, 3, kVictimA);
			giveStolen(r, me, stk, 1, kVictimB);
			const std::uint64_t other = boxAt(r, kVictimB, x + 1.0, z + 1.0, stk, 1);
			// a stolen stack put into another faction's box keeps its mark (nothing clears it there)
			test::require(r.transfer63(me, other, stk, 3, false).empty(), "put into B's box refused");
			r.tick(2);
			const auto inOther = stacksOf(boxOf(other), stk);
			bool kept = false;
			for (const auto& [count, mark] : inOther)
				kept = kept || (count == 3 && mark == kVictimA);
			test::require(kept, "a box of another faction cleared the mark:" + describe(inOther));
			dropAll(r, me, stk);

			// given to a character of the victim faction (a KO'd body of A)
			giveStolen(r, me, stk, 3, kVictimA);
			setFaction(r, buddy, kVictimA);
			r.fighter(buddy).unconscious = true;
			test::require(r.transfer63(me, buddy, stk, 3, false).empty(), "give to A's body refused");
			r.tick(2);
			const auto onBody = stacksOf(packOf(buddy), stk);
			test::require(onBody.size() == 1 && onBody[0].first == 3 && onBody[0].second.empty(), "the mark did not clear on a give to the victim faction:" + describe(onBody));
			std::cout << "given to the owner:" << describe(onBody) << "\n";
			dropAll(r, buddy, stk);
			r.fighter(buddy).unconscious = false;

			// the victim faction's own character takes it back from a thief's box
			const std::uint64_t thiefBox = chestWith(r, kVictimB, x + 1.0, z + 1.0, stk, 2, kVictimA);
			const std::string meBefore = factionOf(r.ecs(), me);
			setFaction(r, me, kVictimA);
			test::require(r.transfer63(me, thiefBox, stk, 2, true).empty(), "take by the victim faction's character refused");
			r.tick(2);
			const auto back = stacksOf(packOf(me), stk);
			test::require(back.size() == 1 && back[0].first == 2 && back[0].second.empty(), "the mark did not clear when its own faction took it back:" + describe(back));
			setFaction(r, me, meBefore);
			dropAll(r, me, stk);
			std::cout << "taken back by the owner faction:" << describe(back) << "\n";
		}

		// ---- 4. shop stock: a fence launders ----
		{
			flecs::entity barSquad;
			double best = 1e18;
			r.ecs().each([&](flecs::entity e, const state::SquadInstance& sq, const state::TraderStock&, const state::Position& at)
			{
				const kdata::Record* t = gd->find(sq.templateSid);
				const double d = std::hypot(at.x - r.fighter(me).x, at.z - r.fighter(me).z);
				if (t != nullptr && t->name == "Bar blackmarket" && d < best)
				{
					best = d;
					barSquad = e;
				}
			});
			test::require(barSquad.is_valid(), "no Bar blackmarket merchant squad");
			const std::uint64_t stockId = barSquad.get<state::Id>()->value;
			std::uint64_t barman = 0;
			r.ecs().query_builder<const state::Id>().with<state::MemberOf>(barSquad).build().each([&](flecs::entity, const state::Id& id)
			{
				barman = barman == 0 ? id.value : barman;
			});
			test::require(barman != 0, "the barman has no character");
			{
				state::Fighter& hero2 = r.fighter(me);
				const state::Fighter& bar = r.fighter(barman);
				hero2.x = bar.x + 2.0;
				hero2.z = bar.z;
				hero2.y = bar.y;
				hero2.clearRoute();
			}
			const_cast<state::TraderStock*>(barSquad.get<state::TraderStock>())->money = 100000;
			dropAll(r, me, iron);
			giveStolen(r, me, iron, 1, kVictimA); // Raw Iron stacks to 1: one unit is one stack
			test::require(r.talk(me, barman).empty(), "talk on the barman refused");
			r.tick(40);
			const int at = Rig::indexOf(r.inv.at(me), iron);
			test::require(at >= 0, "the actor's Raw Iron is not in the client's inventory");
			const std::uint32_t stockBefore = barSquad.get<state::Inventory>()->carried(iron);
			const std::string why = r.transfer68(me, stockId, r.inv.at(me).version, static_cast<std::uint16_t>(at), 1, false);
			test::require(why.empty(), "the sale of stolen iron refused: " + why);
			r.tick(3);
			const state::Inventory& stock = *barSquad.get<state::Inventory>();
			test::require(stock.carried(iron) == stockBefore + 1, "the shop did not take the unit");
			for (const state::Inventory::Entry& entry : stock.items)
				test::require(entry.itemSid != iron || entry.stolen.empty(), "shop stock holds a stolen mark: '" + entry.stolen + "'");
			std::cout << "shop stock:" << describe(stacksOf(stock, iron)) << "\n";
		}
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_stolen_mark FAILED: " << e.what() << "\n";
		return 1;
	}
}
