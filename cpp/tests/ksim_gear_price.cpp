// Gear prices (kinspect findings/s72_static.md sections 2-4; docs/reference/MINE_SELL_SLICE.md "Gear prices"): the Book's item+0x1f0 and the
// barman's quotes for the two live observations, from the real merged data.
//   3a Iron Club (rebirth record 52295, value 80) made by Unknown ("price mod" 0.3) at Rusting Blade (level 10): Value 198, Sell 49.
//   3b Rag Loincloth (2308, value 100, slot 6, no part coverage): Value 50, Sell 12.
// Plus the rest of the s72 3a table, and the crafted-armour table path (s72 2c Chainmail example).

#include "ksim/trade.hpp"

#include "kdata/format.hpp"
#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"

#include <cmath>
#include <iostream>
#include <string>
#include <string_view>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;

const kdata::Record& named(const kdata::GameData& gd, const char* type, std::string_view name)
{
	for (const kdata::Record* record : gd.byType(kworld::itemTypeByName(type)))
	{
		if (record->name == name)
			return *record;
	}
	throw test::Failure(std::string("the data has no ") + type + " " + std::string(name));
}

const kdata::Record& bySid(const kdata::GameData& gd, std::string_view sid, std::string_view name)
{
	const kdata::Record* record = gd.find(std::string(sid));
	if (record == nullptr || record->name != name)
		throw test::Failure("the data has no " + std::string(sid) + " named " + std::string(name));
	return *record;
}

// A grade (MATERIAL_SPECS_WEAPON) of a maker by name, with its level in percent (the `weapon models` v0).
state::ItemQuality gradeOf(const kdata::GameData& gd, const kdata::Record& maker, const std::string& grade)
{
	for (const kdata::RefEntry& entry : maker.findRefs("weapon models")->entries)
	{
		const kdata::Record* spec = entry.opaqueEntry ? nullptr : gd.find(entry.targetID);
		if (spec != nullptr && spec->name == grade)
		{
			state::ItemQuality quality;
			quality.maker = maker.stringID;
			quality.model = spec->stringID;
			quality.level = entry.v0 / 100.0;
			quality.grade = state::ItemQuality::gradeOf(quality.level);
			return quality;
		}
	}
	throw test::Failure(maker.name + " has no grade " + grade);
}

} // namespace

int main()
{
	try
	{
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);
		const kdata::GameData gd = kworld::loadMergedGameData(order);
		const trade::Book book(gd, 1);
		const trade::Terms barman; // E = 1: no template, town or faction
		const trade::Constants& k = book.constants();
		test::require(std::fabs(k.sword - 0.4) < 1e-6 && std::fabs(k.armour - 0.6) < 1e-6 && std::fabs(k.clothing - 1.0) < 1e-6 && std::fabs(k.lootGear - 0.25) < 1e-6,
			"CONSTANTS: sword price mult 0.4, armor 0.6, clothing 1.0, loot GEAR 0.25 (the rebirth merge s72 assumes)");

		// ---- 3a ----
		{
			const kdata::Record& club = bySid(gd, "52295-rebirth.mod", "Iron Club");
			test::require(club.intOr("value", 0) == 80, "the Iron Club's value is 80");
			const kdata::Record& unknown = named(gd, "WEAPON_MANUFACTURER", "Unknown");
			test::require(std::fabs(unknown.floatOr("price mod", 0.0f) - 0.3f) < 1e-6f, "Unknown's price mod is 0.3");
			const state::ItemQuality rusting = gradeOf(gd, unknown, "Rusting Blade");
			test::require(std::lround(rusting.level * 100.0) == 10, "Rusting Blade is level 10");
			const trade::Quote q = book.quote(barman, state::Inventory::Entry{club.stringID, 1, 0, rusting});
			std::cout << "Iron Club (Unknown / Rusting Blade): Value " << q.buy << ", barman pays " << q.sell << "\n";
			test::require(book.gearPrice(club, rusting.maker, 10) == 198 && q.buy == 198, "s72 3a: Iron Club Value 198");
			test::require(q.sell == 49 && q.refusal == trade::Refusal::None, "s72 4d: the barman pays int(198 x 0.25) = 49");
			// the other rows of the s72 3a table (Value / Sell, E = 1)
			const struct
			{
				const char* maker;
				const char* grade;
				int value;
				int sell;
			} rows[] = {
				{"Unknown", "Rusted Junk", 180, 45},
				{"Ancient", "Mid-Grade Salvage", 596, 149},
				{"Ancient", "Refitted Blade", 826, 206},
				{"Skeleton Smiths", "Mk I", 3360, 840},
				{"Edgewalkers", "Edge Type 3", 6627, 1656},
				{"Cross", "Meitou", 10072, 2518},
			};
			for (const auto& row : rows)
			{
				const state::ItemQuality quality = gradeOf(gd, named(gd, "WEAPON_MANUFACTURER", row.maker), row.grade);
				const trade::Quote r = book.quote(barman, state::Inventory::Entry{club.stringID, 1, 0, quality});
				test::require(r.buy == row.value && r.sell == row.sell, std::string("s72 3a table: ") + row.maker + " / " + row.grade);
			}
			test::require(book.basePrice(club) == book.gearPrice(club, "", 0), "basePrice of a record = an instance with no maker at level 0");
		}

		// ---- 3b ----
		{
			const kdata::Record& loincloth = bySid(gd, "2308-clothes_v1.mod", "Rag Loincloth");
			test::require(loincloth.intOr("value", 0) == 100 && loincloth.intOr("slot", 0) == 6, "the loincloth: value 100, slot 6");
			for (const int level : {0, 5, 50, 95})
				test::require(book.gearPrice(loincloth, "", level) == 50, "s72 3b: 50 at every level (no part coverage)");
			state::ItemQuality worn;
			worn.level = 1.0; // clothing without part coverage: level 1, no grade (simlink_scenario)
			const trade::Quote q = book.quote(barman, state::Inventory::Entry{loincloth.stringID, 1, 0, worn});
			std::cout << "Rag Loincloth: Value " << q.buy << ", barman pays " << q.sell << "\n";
			test::require(q.buy == 50 && q.sell == 12, "s72 3b / 4d: Value 50, the barman pays int(50 x 0.25) = 12");
		}

		// ---- 2c: the class / material table path, crafted tiers (Chainmail, value 12000, class 2, material 2, slot 8) ----
		{
			const kdata::Record& chain = bySid(gd, "2210-gamedata.base", "Chainmail");
			test::require(book.gearPrice(chain, "", 0) == 10800, "s72 2c: spawned Chainmail 10800");
			const int levels[] = {5, 20, 40, 60, 80, 95};
			const int want[] = {835, 2580, 8163, 17469, 30498, 42712};
			for (int i = 0; i < 6; ++i)
				test::require(book.gearPrice(chain, "", levels[i]) == want[i], "s72 2c: crafted Chainmail at level " + std::to_string(levels[i]));
			test::require(book.quote(barman, state::Inventory::Entry{chain.stringID, 1, 0, {}}).sell == 2700, "s72 4d: spawned Chainmail sells for 2700");
			state::ItemQuality uniform;
			uniform.uniform = "faction-uniform-test";
			const trade::Quote u = book.quote(barman, state::Inventory::Entry{chain.stringID, 1, 0, uniform});
			std::cout << "uniform Chainmail: Value " << u.buy << ", barman pays " << u.sell << "\n";
			test::require(u.buy == 3240 && u.sell == 810, "s72 4: a faction uniform prices x0.3 (Value 3240, sells 810)");
		}
		std::cout << "ksim_gear_price ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "error: " << e.what() << "\n";
		return 1;
	}
}
