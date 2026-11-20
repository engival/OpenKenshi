// Trade (docs/reference/MINE_SELL_SLICE.md M4-M5; SIMLINK order 60 talk, TRADE 53, order 68 against a stock, order 86, events 13 and 40):
// the Hub barman after a new game, the price chain, buy and sell, every refusal, the closes, a save in the middle of a trade, and the
// whole demo (mine, take, walk, talk, sell) on the real data. Everything is looked up in the data; nothing here names an id.
// argv[1]: the Hub navmesh cache (`knav_bake`, ctest fixture knav_cache).

#include "trade_rig.hpp"
#include "ksim/dev/build_paths.hpp"


int main(int argc, char** argv)
{
	try
	{
		test::require(argc == 2, "usage: ksim_trade HUB.knav");
		test::require(kSaveVersion == 118, "the save version is 118 (v118 task ds/k16.6-fought-saved: Fighter::fought, the has-fought mark; v117 task ds/slice-load-parity: BoardStates occupancy map and occupyTown; v116 task ds/k16.6.483: RunStates::farOrders, the leg-by-leg far move orders; v115 reserved for k16.6.12.10; v114 task ds/k16.8.50: Fighter::pathSamples, the pair-distance cache; v113 task ds/perception-aim-xp: FighterSkills::perception; v112 task ds/passive-hold: Fighter::passive and Fighter::holdPosition; v111 task ds/drive-strafe: Fighter::driveStrafe; v110 task ds/unsaved-audit: state::RunStates, the limb play, alarm board and alarm wait; v109 task ds/save-walk-arrest: Fighter::walkHeading / walkFacing and ArrestStates; v108 task ds/container-piece: Container building / piece; v107 task ds/slave-timers: Fighter::slaveStamp; v106 task ds/spectate-stagger: Inventory slot kSheathed, the weapon on the back; v105 task ds/save-import3b: TownState::info, ExtraTown, FactionState rank / trust, WorldEventState used; v104 task ds/limb-tint: GroundItem tint, a severed limb's skin, blood and scale; v103 task ds/save-import2: MemberDelta look block, the name and sex of an imported member; v102 task ds/stragglers: state::StragglerOf, the link of a member split off its squad at demotion; v101 task ds/member-slot: state::MemberSlot, the build slot fixed at spawn; v100 task ds/corpses: CorpseDecay deathTick / cause and MemberDelta kit, the off-screen death and the stored medic's kit; v99 task ds/delta-store: state::MemberDelta, the per-member delta kept across demotion; v98 task ds/stealth-ko: Fighter::stealthKo and FighterSkills::thievery / assassination, the stealth KO under way; v97 task ds/turret: Fighter::mount and FighterSkills::turrets, the turret a gunner mans; v96 task ds/ranged: Fighter::ranged and FighterSkills::crossbows, the carried crossbow; v95 task ds/slaver-deliver: BoardStates owned / depot, the ownership set and the slaver depot; v94 task ds/board-pick: BoardStates startedAt / traderState / occupyHeld; v93 task ds/town-port: state::TalkHold, the character the player talked to stands still; v92 the Board save; v91 task ds/kidnap: Fighter::captive; v90 task ds/arrest-2: state::CropPlot, the farm crop growth and ripe food; v89 task ds/provenance: InventoryEntry planted / stolen, GroundItem::stolen; v88 task ds/construction: PlayerBuilding ruin / dismantled / furniture / mounted, FighterSkills::engineer; v87 task locks-save: state::LockStates, the picked / cut cage and shackle locks; v86 task CH group 3: Routine::shopping / shoppingDay / shoppingTimer / shoppingLatch / shoppingShop / shoppingX-Z, the shopping list and the 119 timer; v85 task indoors: Fighter::indoors, the interior-and-ceiling state; v84 task CH: state::TakeOut plan / step / liftOrdered / dropSet, the intruder eviction's G4 chain; v83 task patrol: the 216 town search; v82 task CB: Routine::plan / step, the plan the slaver chain runs; v81 task CC: Fighter::diceSeed, the stable combat dice identity; v80 unused); v79 Fighter::attackType, the animal technique family (task BQ); v78 task BZ: Fighter::aggressorSquads, the squad map's AGGRESSOR bit that goal 34 reads; v77 state::WorldOptions, the new-game cod / gdm / ht (task BT); v76 Fighter::restYaw / restRolls / restYawValid, the ragdoll rest pose (task BG); v75 Fighter::met, the dialogue memory (task BB); v74 Fighter::combatGoal (task AT, the goal that started a fight); v73 CaravanOrder::postedAt (T4d, the delivery order's age); v72 state::WorldEventState dead + playerTouched (task AN); v71 state::Container key + lock (task AL); v70 FactionState::cash (the dialogue economy's per-faction money); v69 the real-economy overlay: EconomySettings, TownStock, EconomyProducer, CaravanOrder, SupplyBoard; v68 the slave trade: the captivity fields and the chain; v67 Fighter::aimYaw; v66 Fighter::hitList / holdSteer; v65 Fighter::moveMode / speedGroup; v64 Conversation::replies / menuTick; v63 state::ItemNode; v62 GroundItem::ownerSid; v61 Fighter::goal; v60 state::BuildingLooks; v59 SquadInstance::homeBuilding; v58 state::DialogueMemory, Conversation::follower; v57 state::Conversation / DialogueShown; v56 ItemQuality::uniform; v55 producer collision bounds");
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);
		const auto gd = std::make_shared<const kdata::GameData>(kworld::loadMergedGameData(order));
		const std::shared_ptr<const CombatData> data = CombatData::build(*gd, KENSHI_DATA);
		const std::shared_ptr<MeshNavigator> nav = MeshNavigator::load(argv[1]);
		const std::string base = std::string(KSIM_BINARY_DIR) + "/ksim_trade_test";
		fs::remove_all(base);
		const std::string saveDir = ks::prepareSaveDir(base + "/saves");
		const grid::Rules rules(*gd);

		const auto itemByName = [&](const std::string& name) -> const kdata::Record&
		{
			for (const kdata::Record* record : gd->byType(kworld::itemTypeByName("ITEM")))
			{
				if (record->name == name)
					return *record;
			}
			throw test::Failure("the data has no ITEM " + name);
		};
		const kdata::Record& iron = itemByName("Raw Iron");
		const kdata::Record& copper = itemByName("Copper");
		const auto baseOf = [](const kdata::Record& r) { return static_cast<int>(std::floor(static_cast<float>(r.intOr("value", 0)) * 0.6f + 0.5f)); };

		// ---- the price rules, from the merged data ----
		{
			const trade::Book book(*gd, 7);
			const trade::Constants& k = book.constants();
			test::require(std::fabs(k.trade - 0.6) < 1e-6 && std::fabs(k.margin - 0.3) < 1e-6 && std::fabs(k.loot - 0.5) < 1e-6 && std::fabs(k.lootGear - 0.25) < 1e-6,
				"CONSTANTS from the merged data: trade price mult 0.6, margin 0.3, loot 0.5, loot GEAR 0.25");
			test::require(iron.boolOr("trade item", false) && copper.boolOr("trade item", false), "Raw Iron and Copper are `trade item`s");
			test::require(book.isTradeGood(iron.stringID) && book.isTradeGood(copper.stringID), "and in the VENDOR_LIST `all trade goods`");
			test::require(book.basePrice(iron) == baseOf(iron) && book.basePrice(copper) == baseOf(copper), "base = floor(value x 0.6 + 0.5)");
			std::cout << "Raw Iron value " << iron.intOr("value", 0) << " base " << baseOf(iron) << ", Copper value " << copper.intOr("value", 0) << " base " << baseOf(copper) << "\n";
			// townFactor: per (town, item), in [1 - M, 1 + M], a pure function
			double lo = 9, hi = 0;
			int different = 0;
			for (std::uint64_t town = 1; town <= 200; ++town)
			{
				const double a = book.townFactor(town, iron.stringID);
				lo = std::min(lo, a);
				hi = std::max(hi, a);
				different += a != book.townFactor(town + 1000, iron.stringID);
				test::require(a == trade::Book(*gd, 7).townFactor(town, iron.stringID), "the factor is a pure function of seed, town and item");
			}
			test::require(lo >= 0.7 - 1e-6 && hi <= 1.3 + 1e-6 && hi - lo > 0.5, "factors fill [0.7, 1.3]: " + std::to_string(lo) + " .. " + std::to_string(hi));
			test::require(different > 150, "towns differ");
			test::require(book.townFactor(5, iron.stringID) != trade::Book(*gd, 8).townFactor(5, iron.stringID), "another world seed, another factor");
			test::require(book.townFactor(5, iron.stringID) != book.townFactor(5, copper.stringID), "items differ in a town");
			test::require(book.townFactor(0, iron.stringID) == 1.0, "no town, no factor");
			// A non-trade-good item has factor 1
			const kdata::Record* plain = nullptr;
			for (const kdata::Record* record : gd->byType(kworld::itemTypeByName("ITEM")))
			{
				if (!book.isTradeGood(record->stringID) && record->intOr("value", 0) > 0 && !record->boolOr("trade item", false) && record->findRefs("ingredients") == nullptr)
				{
					plain = record;
					break;
				}
			}
			test::require(plain != nullptr, "the data has a plain priced item");
			test::require(book.townFactor(5, plain->stringID) == 1.0, "an item outside `all trade goods` has no factor");
			// quote: trade item buy == sell; others sell x loot mult; E from the squad template
			trade::Terms terms;
			terms.townId = 5;
			state::Inventory::Entry entry;
			entry.itemSid = iron.stringID;
			const trade::Quote q = book.quote(terms, entry);
			const int expect = static_cast<int>(static_cast<float>(book.townFactor(5, iron.stringID)) * static_cast<float>(baseOf(iron)));
			test::require(q.buy == expect && q.sell == expect, "a trade item buys and sells at int(factor x base): " + std::to_string(q.buy) + " / " + std::to_string(q.sell));
			terms.buyMult = 0.5;
			test::require(book.quote(terms, entry).buy == static_cast<int>(0.5f * static_cast<float>(expect)), "E = the template's buy mult");
			terms.buyMult = 1.0;
			entry.itemSid = plain->stringID;
			const trade::Quote p = book.quote(terms, entry);
			test::require(p.buy == book.basePrice(*plain) && p.sell == static_cast<int>(static_cast<float>(p.buy) * 0.5f), "a plain item sells at the loot price mult");
			terms.buysIllegal = true;
			const trade::Quote smuggler = book.quote(terms, entry);
			test::require(smuggler.sell == -1 && smuggler.refusal == trade::Refusal::IllegalOnly && smuggler.buy == p.buy, "a smuggler buys only illegal goods (UNSURE reading)");
			std::cout << "prices ok\n";
		}

		Rig r(order, gd, data, nav, saveDir);
		r.newGame("Nobodies");
		test::require(r.members.size() == 5, "Nobodies: five characters");
		const std::uint64_t me = r.members[0];
		const std::uint64_t buddy = r.members[1];
		r.tick(60);

		// ---- the barman: found by data, a Fighter at Full detail near the player right after the new game ----
		flecs::entity barSquad;
		{
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
			// Homed in the Hub's Bar by the town populate, ~345 u from the start (the town point).
			test::require(barSquad.is_valid() && best < 400.0, "a `Bar blackmarket` merchant squad stands in the start town's bar, within 400 units (" + std::to_string(best) + ")");
		}
		const std::uint64_t stockId = barSquad.get<state::Id>()->value;
		std::uint64_t barman = 0;
		{
			test::require(trader::isMerchant(*gd->find(barSquad.get<state::SquadInstance>()->templateSid)), "his squad template is a merchant (is trader + vendors)");
			test::require(barSquad.get<Detail>()->level == DetailLevel::Full, "his squad is at Full detail (the player areas follow the player)");
			r.ecs().query_builder<const state::Id>().with<state::MemberOf>(barSquad).build().each([&](flecs::entity, const state::Id& id)
			{
				test::require(r.sim->combat()->find(id.value) != nullptr, "every member of his squad is a Fighter (merchantAtHome)");
				barman = barman == 0 ? id.value : barman;
			});
			test::require(barman != 0 && !barSquad.get<state::Inventory>()->items.empty() && barSquad.get<state::TraderStock>()->money > 0, "he has stock and a wallet");
			test::require(!r.fighter(barman).playerControlled, "he is not the player's");
			std::cout << "barman " << barman << " squad " << stockId << " wallet " << barSquad.get<state::TraderStock>()->money << " stock entries "
				<< barSquad.get<state::Inventory>()->items.size() << " at distance " << std::hypot(r.fighter(me).x - r.fighter(barman).x, r.fighter(me).z - r.fighter(barman).z) << "\n";
		}
		// T3d follow-up: the barman works behind his counter in the Bar (his home is the placement that carries his template), about 350 u from the town
		// point where the new game starts. The player goes to him wherever he stands (looked up, not hard-coded), as the test always assumed he was close.
		{
			state::Fighter& hero = r.fighter(me);
			const state::Fighter& bar = r.fighter(barman);
			// He walks to his post first: the Node Shopkeeper marker behind the counter (kenshi-k16.6.106), so the ranges below hold still.
			for (int settle = 0; settle < 40; ++settle)
			{
				const double x = bar.x;
				const double z = bar.z;
				r.tick(10);
				if (bar.x == x && bar.z == z)
					break;
			}
			hero.x = bar.x + 2.0;
			hero.z = bar.z;
			hero.y = bar.y;
			hero.clearRoute();
		}
		// The barman walks to his post in legs with pauses (the KO / hostile checks above send him back; a joined stair adds legs): still for 100 ticks is at the post.
		const auto settleBarman = [&]
		{
			for (int settle = 0, still = 0; settle < 100 && still < 10; ++settle)
			{
				const double x = r.fighter(barman).x;
				const double z = r.fighter(barman).z;
				r.tick(10);
				still = r.fighter(barman).x == x && r.fighter(barman).z == z ? still + 1 : 0;
			}
		};
		const std::string templateSid = barSquad.get<state::SquadInstance>()->templateSid;
		const trade::Book& book = r.sim->tradeBook();
		const auto barmanSquadMoney = [&]() { return static_cast<std::int64_t>(barSquad.get<state::TraderStock>()->money); };
		const auto stockInv = [&]() -> const state::Inventory& { return *barSquad.get<state::Inventory>(); };

		// ---- talk: the refusals ----
		{
			test::require(r.talk(me, 99999999) == "bad_target", "talk on nothing: bad_target");
			test::require(r.talk(me, me) == "bad_target", "talk on yourself: bad_target");
			test::require(r.talk(me, stockId) == "bad_target", "talk on a squad id (not a character): bad_target");
			test::require(r.talk(me, buddy).empty(), "talk on a conscious non-merchant: accepted (a dialogue, DIALOGUE.md 13)");
			state::Fighter& bar = r.fighter(barman);
			bar.unconscious = true;
			test::require(r.talk(me, barman) == "not_allowed", "talk on a KO'd merchant: not_allowed");
			bar.unconscious = false;
			bar.asleep = true;
			test::require(r.talk(me, barman) == "not_allowed", "talk on a sleeping merchant: not_allowed");
			bar.asleep = false;
			bar.down = true;
			test::require(r.talk(me, barman) == "not_allowed", "talk on a downed merchant: not_allowed");
			bar.down = false;
			// hostile: the merchant's faction thinks the player faction is at or below -30
			const flecs::entity theirs(r.ecs().c_ptr(), barSquad.get<state::SquadInstance>()->faction.id);
			test::require(theirs.is_valid(), "the barman's squad has a faction");
			r.ecs().entity(theirs.id()).set<state::Relation>(r.playerFaction(), state::Relation{-50.0});
			test::require(r.talk(me, barman) == "not_allowed", "talk on a merchant whose faction is hostile: not_allowed");
			r.ecs().entity(theirs.id()).set<state::Relation>(r.playerFaction(), state::Relation{0.0});
			test::require(r.events.empty() || r.countEvents(40) == 0, "refused talks open nothing");
			test::require(r.trades.empty(), "and send no TRADE");
			std::cout << "talk refusals ok\n";
		}

		// ---- talk: opens the trade ----
		r.setWallet(1000);
		r.give(me, iron.stringID, 3);
		r.give(me, copper.stringID, 2);
		test::require(r.talk(me, barman).empty(), "talk on the barman accepted");
		r.tick(40);
		const Trade* t = r.lastTrade();
		test::require(t != nullptr, "TRADE arrived");
		test::require(r.events.end() != std::find_if(r.events.begin(), r.events.end(), [&](const Ev& e) { return e.kind == 13 && e.a == me && e.b == barman && e.action == 6 && e.result == 0; }),
			"event 13 action 6 result 0 marks the arrival");
		test::require(t->trader == barman && t->actor == me && t->stock == stockId && t->templateSid == templateSid, "TRADE names the trader character, the actor, the stock (his squad) and the template");
		test::require(t->traderMoney == barmanSquadMoney(), "and his wallet");
		test::require(t->chests.empty(), "no owned container yet: the chest tail is k = 0 and the frame still decodes");
		const Inv& stockWire = r.inv.at(stockId);
		test::require(stockWire.items.size() == stockInv().items.size() && stockWire.gridItems == static_cast<int>(stockInv().items.size()) && t->stockVersion == stockWire.version,
			"INVENTORY + ITEM_GRID under the squad id while the trade is open");
		test::require(t->stockPrices.size() == stockWire.items.size() && t->actorPrices.size() == r.inv.at(me).items.size() && t->actorVersion == r.inv.at(me).version,
			"one price per stock entry and per actor entry, the versions are the INVENTORYs'");
		{
			const trade::Terms terms = book.termsOf(barSquad);
			test::require(terms.townId != 0 && terms.town != nullptr, "the barman has a home town (the price factor's town)");
			int unpriced = 0;
			for (const Trade::Price& price : t->stockPrices)
			{
				const trade::Quote q = book.quote(terms, stockInv().items[static_cast<std::size_t>(price.index)]);
				test::require(price.buy == q.buy && price.sell == q.sell, "stock price " + std::to_string(price.index) + " is the Book's");
				test::require(price.buy >= 0 && (price.buy > 0 || price.sell == -1), "an entry with no price cannot be sold either");
				unpriced += price.buy == 0;
			}
			std::cout << unpriced << " of " << t->stockPrices.size() << " stock entries have no price (records with no `value`: blueprints, maps)\n";
			for (const auto& price : t->actorPrices)
			{
				const std::string& item = std::get<0>(r.inv.at(me).items[static_cast<std::size_t>(price.first)]);
				test::require(price.second == book.quote(terms, state::Inventory::Entry{item, 1, 0, {}}).sell || std::get<2>(r.inv.at(me).items[static_cast<std::size_t>(price.first)]) != 0,
					"actor price is the trader's buying price");
			}
		}
		// Raw Iron and copper at the Hub (the price check)
		const trade::Terms hubTerms = book.termsOf(barSquad);
		const int ironUnit = book.quote(hubTerms, state::Inventory::Entry{iron.stringID, 1, 0, {}}).sell;
		const int copperUnit = book.quote(hubTerms, state::Inventory::Entry{copper.stringID, 1, 0, {}}).sell;
		{
			const double fi = book.townFactor(hubTerms.townId, iron.stringID);
			const double fc = book.townFactor(hubTerms.townId, copper.stringID);
			test::require(ironUnit == static_cast<int>(static_cast<float>(fi) * static_cast<float>(baseOf(iron))) && copperUnit == static_cast<int>(static_cast<float>(fc) * static_cast<float>(baseOf(copper))),
				"the Hub prices are int(townFactor x base) (E 1.0, no culture on ores)");
			std::cout << "Hub: Raw Iron " << ironUnit << " (base " << baseOf(iron) << ", factor " << fi << "), Copper " << copperUnit << " (base " << baseOf(copper) << ", factor " << fc << ")\n";
		}
		{
			const std::string bm = std::to_string(hubTerms.buyMult);
			test::require(std::fabs(hubTerms.buyMult - 1.0) < 1e-9 && !hubTerms.buysIllegal, "the barman: buy mult 1.0, buys illegal 0 (" + bm + ")");
		}

		// ---- sell one Raw Iron ----
		{
			const std::int64_t money0 = r.wallet();
			const std::int64_t shop0 = barmanSquadMoney();
			const std::uint32_t mine0 = r.carried(me, iron.stringID);
			const std::uint32_t theirs0 = Rig::countIn(r.inv.at(stockId), iron.stringID);
			const std::size_t trades0 = r.trades.size();
			const Inv& mineWire = r.inv.at(me);
			const int index = Rig::indexOf(mineWire, iron.stringID);
			test::require(index >= 0, "the actor's INVENTORY lists the Raw Iron");
			test::require(r.transfer68(me, stockId, mineWire.version, static_cast<std::uint16_t>(index), 1, false).empty(), "sell: order 68 take 0 accepted");
			r.tick(3);
			test::require(r.carried(me, iron.stringID) == mine0 - 1, "the unit left the character");
			test::require(Rig::countIn(r.inv.at(stockId), iron.stringID) == theirs0 + 1, "and is in the stock (wire)");
			test::require(r.wallet() == money0 + ironUnit && barmanSquadMoney() == shop0 - ironUnit, "money moved both ways in the same tick at the computed price");
			test::require(r.money.back() == money0 + ironUnit, "PLAYER_SQUADS carries the new money");
			test::require(r.trades.size() > trades0 && r.trades.back().traderMoney == shop0 - ironUnit && r.trades.back().stockVersion == r.inv.at(stockId).version
				&& r.trades.back().actorVersion == r.inv.at(me).version, "TRADE is resent with the new wallet and versions");
			std::cout << "sold 1 Raw Iron for " << ironUnit << "\n";

			// ---- buy it back (1 unit of the stock's Raw Iron) ----
			const std::int64_t money1 = r.wallet();
			const std::int64_t shop1 = barmanSquadMoney();
			const Inv& st = r.inv.at(stockId);
			const int at = Rig::indexOf(st, iron.stringID);
			test::require(at >= 0, "the stock holds Raw Iron");
			const int buyUnit = r.trades.back().stockPrices[static_cast<std::size_t>(at)].buy;
			test::require(buyUnit == ironUnit, "a trade item buys at the price it sells at");
			test::require(r.transfer68(me, stockId, st.version, static_cast<std::uint16_t>(at), 1, true).empty(), "buy: order 68 take 1 accepted");
			r.tick(3);
			test::require(r.carried(me, iron.stringID) == mine0 && r.wallet() == money1 - buyUnit && barmanSquadMoney() == shop1 + buyUnit, "bought: the unit and the money moved");
			std::cout << "buy ok\n";

			// ---- the same sell and buy back with the clock paused (kenshi-k16.20.5): applied at once, frames sent, the tick does not move ----
			r.context.paused = true;
			const std::uint64_t tick0 = r.sim->clockTick();
			const std::int64_t money2 = r.wallet();
			const std::size_t logged0 = r.context.orderLog.size();
			const std::size_t trades1 = r.trades.size();
			const int paused = Rig::indexOf(r.inv.at(me), iron.stringID);
			test::require(r.transfer68(me, stockId, r.inv.at(me).version, static_cast<std::uint16_t>(paused), 1, false).empty(), "paused sell accepted");
			test::require(r.carried(me, iron.stringID) == mine0 - 1 && r.wallet() == money2 + ironUnit, "paused: the sell moved the unit and the money at once");
			test::require(r.money.back() == money2 + ironUnit && r.trades.size() > trades1 && r.trades.back().actorVersion == r.inv.at(me).version,
				"paused: PLAYER_SQUADS and TRADE went out with the order");
			const int pausedAt = Rig::indexOf(r.inv.at(stockId), iron.stringID);
			test::require(r.transfer68(me, stockId, r.inv.at(stockId).version, static_cast<std::uint16_t>(pausedAt), 1, true).empty(), "paused buy back accepted");
			test::require(r.carried(me, iron.stringID) == mine0 && r.wallet() == money2, "paused: the buy back restored the unit and the money");
			test::require(r.sim->clockTick() == tick0 && r.sim->pendingCommands() == 0, "paused: no tick ran and nothing waits in the queue");
			test::require(r.context.orderLog.size() == logged0 + 2 && r.context.orderLog.back().tick == tick0 + 1 && r.context.orderLog.back().sim,
				"paused: both moves are logged for the next tick");
			r.context.paused = false;
			r.tick(3);
			test::require(r.carried(me, iron.stringID) == mine0 && r.wallet() == money2, "unpaused: the moves are not applied twice");
			std::cout << "paused sell and buy back ok\n";
		}

		// ---- the short wallet: a partial ----
		{
			const Inv& st = r.inv.at(stockId);
			int at = -1;
			std::uint32_t have = 0;
			for (std::size_t i = 0; i < st.items.size(); ++i)
			{
				if (std::get<2>(st.items[i]) == 0 && std::get<1>(st.items[i]) >= 3 && r.trades.back().stockPrices[i].buy > 0)
				{
					at = static_cast<int>(i);
					have = std::get<1>(st.items[i]);
					break;
				}
			}
			test::require(at >= 0, "the stock has a stack of 3 or more");
			const std::string item = std::get<0>(st.items[static_cast<std::size_t>(at)]);
			const int unit = r.trades.back().stockPrices[static_cast<std::size_t>(at)].buy;
			// afford 2 units and a bit
			r.setWallet(unit * 2 + unit / 2);
			const std::uint32_t mine0 = r.carried(me, item);
			const std::uint32_t shop0 = stockInv().carried(item);
			const std::int64_t shopMoney0 = barmanSquadMoney();
			test::require(r.transfer68(me, stockId, st.version, static_cast<std::uint16_t>(at), 3, true).empty(), "buy 3 with money for 2.5: accepted");
			r.tick(3);
			test::require(r.carried(me, item) == mine0 + 2 && stockInv().carried(item) == shop0 - 2, "a short wallet moves floor(money / unit) = 2 units");
			test::require(r.wallet() == unit / 2 && barmanSquadMoney() == shopMoney0 + 2 * unit, "and only their price");
			(void) have;
			// nothing affordable: cant_afford, nothing moved
			const Inv& st2 = r.inv.at(stockId);
			const int at2 = Rig::indexOf(st2, item);
			test::require(r.wallet() < unit && r.transfer68(me, stockId, st2.version, static_cast<std::uint16_t>(at2), 1, true) == "cant_afford", "no money for one unit: cant_afford");
			r.setWallet(0);
			test::require(r.transfer68(me, stockId, st2.version, static_cast<std::uint16_t>(at2), 1, true) == "cant_afford", "no money at all: cant_afford");

			// the shop's short wallet on a sale: shop_cant_afford, and a partial
			r.setWallet(500);
			const int sellUnit = r.trades.back().stockPrices[static_cast<std::size_t>(at2)].sell;
			test::require(sellUnit > 0, "the stocked item has a trader buying price");
			r.give(me, item, 5);
			r.tick(2);
			int mineAt = -1;
			std::uint32_t most = 0;
			for (std::size_t i = 0; i < r.inv.at(me).items.size(); ++i)
			{
				const auto& e = r.inv.at(me).items[i];
				if (std::get<0>(e) == item && std::get<2>(e) == 0 && std::get<1>(e) > most)
				{
					most = std::get<1>(e);
					mineAt = static_cast<int>(i);
				}
			}
			test::require(most >= 4, "the actor holds a stack of 4 or more of " + item);
			const_cast<state::TraderStock*>(barSquad.get<state::TraderStock>())->money = sellUnit / 2;
			test::require(r.transfer68(me, stockId, r.inv.at(me).version, static_cast<std::uint16_t>(mineAt), 1, false) == "shop_cant_afford", "the shop cannot pay one unit: shop_cant_afford");
			const_cast<state::TraderStock*>(barSquad.get<state::TraderStock>())->money = sellUnit * 3 + sellUnit / 2;
			const std::uint32_t mine1 = r.carried(me, item);
			const std::string why = r.transfer68(me, stockId, r.inv.at(me).version, static_cast<std::uint16_t>(mineAt), static_cast<std::uint16_t>(most), false);
			test::require(why.empty(), "sell a stack of " + std::to_string(most) + " to a shop with money for 3.5: accepted (" + why + ")");
			r.tick(3);
			test::require(r.carried(me, item) == mine1 - 3 && barmanSquadMoney() == sellUnit / 2 && r.wallet() == 500 + 3 * sellUnit, "the shop's short wallet moves 3 units");
			const_cast<state::TraderStock*>(barSquad.get<state::TraderStock>())->money = 5000;
			std::cout << "short wallet ok\n";
		}

		// ---- the other refusals ----
		{
			const Inv& st = r.inv.at(stockId);
			// 63 (by record), 64 and 65 on the stock
			test::require(r.transfer63(me, stockId, iron.stringID, 1, true) == "not_allowed", "order 63 take on a stock: not_allowed");
			test::require(r.transfer63(me, stockId, iron.stringID, 1, false) == "not_allowed", "order 63 give to a stock: not_allowed");
			test::require(r.move64(stockId, st.version, 0) == "not_allowed", "order 64 on a stock: not_allowed");
			test::require(r.arrange65(stockId) == "not_allowed", "order 65 on a stock: not_allowed");
			// 68: the usual reasons
			const int ironAt = Rig::indexOf(r.inv.at(me), iron.stringID);
			test::require(ironAt >= 0, "the actor has Raw Iron");
			test::require(r.transfer68(me, stockId, r.inv.at(me).version + 7, static_cast<std::uint16_t>(ironAt), 1, false) == "stale", "a stale actor version: stale");
			test::require(r.transfer68(me, stockId, st.version + 7, 0, 1, true) == "stale", "a stale stock version: stale");
			test::require(r.transfer68(me, stockId, r.inv.at(me).version, 4000, 1, false) == "bad_value", "an index past the end: bad_value");
			test::require(r.transfer68(me, stockId, r.inv.at(me).version, static_cast<std::uint16_t>(ironAt), 0, false) == "bad_value", "count 0: bad_value");
			test::require(r.transfer68(me, stockId, r.inv.at(me).version, static_cast<std::uint16_t>(ironAt), 500, false) == "not_allowed", "more than the stack holds: not_allowed");
			test::require(r.transfer68(me, 99999999, r.inv.at(me).version, static_cast<std::uint16_t>(ironAt), 1, false) == "bad_target", "no such container: bad_target");
			// not_for_sale: an entry with no price, both ways
			int unpricedAt = -1;
			for (const Trade::Price& price : r.trades.back().stockPrices)
				unpricedAt = price.buy == 0 && unpricedAt < 0 ? price.index : unpricedAt;
			test::require(unpricedAt >= 0, "the stock has an entry with no price");
			test::require(r.transfer68(me, stockId, st.version, static_cast<std::uint16_t>(unpricedAt), 1, true) == "not_for_sale", "buying an unpriced entry: not_for_sale");
			const std::string unpricedItem = std::get<0>(st.items[static_cast<std::size_t>(unpricedAt)]);
			r.give(me, unpricedItem, 1);
			r.tick(2);
			test::require(r.transfer68(me, stockId, r.inv.at(me).version, static_cast<std::uint16_t>(Rig::indexOf(r.inv.at(me), unpricedItem)), 1, false) == "not_for_sale",
				"selling an unpriced entry: not_for_sale (" + unpricedItem + ")");
			// illegal_only: a smuggler's template buys only illegal goods (UNSURE reading of `buys illegal`)
			const kdata::Record* smugglerTemplate = nullptr;
			for (const kdata::Record* record : gd->byType(kworld::itemTypeByName("SQUAD_TEMPLATE")))
			{
				if (record->boolOr("buys illegal", false))
					smugglerTemplate = smugglerTemplate != nullptr ? smugglerTemplate : record;
			}
			if (smugglerTemplate != nullptr)
			{
				state::SquadInstance instance = *barSquad.get<state::SquadInstance>();
				instance.templateSid = smugglerTemplate->stringID;
				barSquad.set<state::SquadInstance>(instance);
				test::require(r.transfer68(me, stockId, r.inv.at(me).version, static_cast<std::uint16_t>(ironAt), 1, false) == "illegal_only", "a smuggler buys only illegal goods: illegal_only");
				instance.templateSid = templateSid;
				barSquad.set<state::SquadInstance>(instance);
				std::cout << "illegal_only through the template '" << smugglerTemplate->name << "'\n";
			} else {
				std::cout << "no SQUAD_TEMPLATE in the data has `buys illegal`: illegal_only is checked on the Book only\n";
			}
			// no_room: a 1 x 1 pack holding something else
			{
				flecs::entity mineEntity = r.entityWithId(me);
				const state::Inventory original = *mineEntity.get<state::Inventory>();
				state::Inventory cramped;
				cramped.gridW = 1;
				cramped.gridH = 1;
				cramped.version = original.version + 1;
				state::Inventory::Entry filler;
				filler.itemSid = copper.stringID;
				filler.count = 1;
				filler.section = 0;
				filler.slot = state::Inventory::kCarried;
				cramped.items.push_back(filler);
				mineEntity.set<state::Inventory>(cramped);
				r.setWallet(100000);
				const Inv& stNow = r.inv.at(stockId);
				int buyAt = -1;
				for (const Trade::Price& price : r.trades.back().stockPrices)
				{
					const std::string& item = std::get<0>(stNow.items[static_cast<std::size_t>(price.index)]);
					// a plain ITEM: gear would go into its free equipment slot
					if (buyAt < 0 && price.buy > 0 && item != copper.stringID && gd->find(item)->type == kworld::itemTypeByName("ITEM") && std::get<2>(stNow.items[static_cast<std::size_t>(price.index)]) == 0)
						buyAt = price.index;
				}
				test::require(buyAt >= 0 && r.transfer68(me, stockId, stNow.version, static_cast<std::uint16_t>(buyAt), 1, true) == "no_room", "a full pack: no_room, nothing moves");
				test::require(r.wallet() == 100000, "and no money moved");
				mineEntity.set<state::Inventory>(original);
				// selling to a stock with no room: fill the stock grid with copies, then sell
				r.setWallet(1000);
			}
			std::cout << "refusals ok\n";
		}

		// ---- the range: 1000 units, per transfer, never a close ----
		{
			// The barman may still be walking to his counter (the arc takes a few ticks longer than the polyline did): sample where he stands.
			for (int wait = 0; wait < 400 && r.fighter(barman).walking(); ++wait)
				r.tick(1);
			r.tick(2);
			const double bx = r.fighter(barman).x;
			const double bz = r.fighter(barman).z;
			const int copperAt = Rig::indexOf(r.inv.at(me), copper.stringID);
			test::require(copperAt >= 0, "the actor has copper");
			const std::size_t closes0 = r.countEvents(40);
			r.teleport(me, bx + 1000.0, bz);
			test::require(r.transfer68(me, stockId, r.inv.at(me).version, static_cast<std::uint16_t>(copperAt), 1, false) == "out_of_range", "exactly 1000 units away: out_of_range (d^2 < 1e6 passes)");
			r.teleport(me, bx, bz + 1500.0);
			test::require(r.transfer68(me, stockId, r.inv.at(me).version, static_cast<std::uint16_t>(copperAt), 1, false) == "out_of_range", "1500 units away: out_of_range");
			r.tick(3);
			test::require(r.countEvents(40) == closes0, "distance closes nothing (no event 40)");
			r.teleport(me, bx + 999.0, bz);
			const std::int64_t money0 = r.wallet();
			test::require(r.transfer68(me, stockId, r.inv.at(me).version, static_cast<std::uint16_t>(copperAt), 1, false).empty(), "999 units away: the sale goes through");
			r.tick(3);
			test::require(r.wallet() == money0 + copperUnit, "and pays the Hub copper price");
			r.teleport(me, bx + 5.0, bz);
			r.tick(3);
			test::require(r.countEvents(40) == closes0, "the trade is still open after the walk back");
			std::cout << "range ok\n";
		}

		// ---- MQ12: a sale from an owned container (order 68 with the trailing source), range from the container ----
		{
			settleBarman();
			// The ranges below are exact (999 / 1001 u): the barman's next leg (a walk off his post, which starts some ticks after he settles and
			// whose timing follows the route lengths) would move the edge. His route is cleared each tick for this block.
			const auto holdBarman = [&](int ticks)
			{
				for (int i = 0; i < ticks; ++i)
				{
					r.fighter(barman).clearRoute();
					r.tick(1);
				}
			};
			const double bx = r.fighter(barman).x;
			const double bz = r.fighter(barman).z;
			const std::string mineSid = r.entityWithId(me).get<state::Allegiance>()->factionSid;
			test::require(!mineSid.empty(), "the actor has a faction");
			const std::size_t closes0 = r.countEvents(40);
			const std::uint64_t chest = r.spawnChest(mineSid, bx + 999.0, bz, iron.stringID, 5);
			const std::uint64_t far = r.spawnChest(mineSid, bx, bz + 1001.0, iron.stringID, 5);
			const std::uint64_t foreign = r.spawnChest("not-my-faction", bx + 10.0, bz, iron.stringID, 5);
			const std::uint64_t nobody = r.spawnChest("", bx + 10.0, bz, iron.stringID, 5);
			const auto chestIron = [&](std::uint64_t box) { return r.sim->settledInventory(box).carried(iron.stringID); };
			const auto sell = [&](std::uint64_t box) { return r.transfer68(me, stockId, r.sim->settledInventory(box).version, 0, 1, false, box); };
			r.teleport(me, bx + 5000.0, bz); // the actor is far away: the range is the container's to the trader
			const std::int64_t money0 = r.wallet();
			test::require(sell(chest).empty(), "a sale from an owned chest 999 units from the trader (the actor 5000 away) is accepted");
			holdBarman(3);
			test::require(chestIron(chest) == 4 && r.wallet() == money0 + ironUnit, "the unit left the chest and the money arrived");
			test::require(sell(far) == "out_of_range" && chestIron(far) == 5, "a chest 1001 units away: out_of_range, nothing moved");
			test::require(sell(foreign) == "not_allowed" && chestIron(foreign) == 5, "a chest of another faction: not_allowed");
			test::require(sell(nobody) == "not_allowed" && chestIron(nobody) == 5, "a chest nobody owns: not_allowed");
			holdBarman(3);
			// The TRADE frame's chest-prices tail: the same predicate as `source`, sorted by id, priced like the actor block.
			{
				const Trade* t = r.lastTrade();
				test::require(t != nullptr, "a TRADE frame is up");
				std::vector<std::uint64_t> ids;
				for (const Trade::Chest& c : t->chests)
					ids.push_back(c.container);
				test::require(std::find(ids.begin(), ids.end(), chest) != ids.end(), "the owned chest in range is in the chest tail");
				test::require(std::find(ids.begin(), ids.end(), far) == ids.end() && std::find(ids.begin(), ids.end(), foreign) == ids.end()
					&& std::find(ids.begin(), ids.end(), nobody) == ids.end(), "out of range, foreign and unowned chests are absent");
				test::require(std::is_sorted(ids.begin(), ids.end()), "sorted by container id");
				const Trade::Chest& mine = *std::find_if(t->chests.begin(), t->chests.end(), [&](const Trade::Chest& c) { return c.container == chest; });
				test::require(mine.version == r.sim->settledInventory(chest).version && mine.prices.size() == r.sim->settledInventory(chest).items.size()
					&& std::all_of(mine.prices.begin(), mine.prices.end(), [&](const std::pair<int, int>& p) { return p.second == ironUnit; })
					&& mine.prices[0].first == 0 && mine.prices.back().first == static_cast<int>(mine.prices.size()) - 1,
					"the chest's version, and one Hub-priced iron entry per INVENTORY entry in flat order");
				const std::size_t frames0 = r.trades.size();
				holdBarman(3);
				test::require(r.trades.size() == frames0, "an unchanged set sends no new TRADE");
				const std::uint32_t version0 = mine.version;
				test::require(sell(chest).empty(), "sell one more unit from the chest");
				r.tick(3);
				const Trade* u = r.lastTrade();
				const auto again = std::find_if(u->chests.begin(), u->chests.end(), [&](const Trade::Chest& c) { return c.container == chest; });
				test::require(r.trades.size() > frames0 && again != u->chests.end() && again->version != version0
					&& again->version == r.sim->settledInventory(chest).version, "a chest version change resends TRADE with the new version");
				// A container entering the set (a new owned chest in range) resends.
				const std::size_t frames1 = r.trades.size();
				const std::uint64_t extra = r.spawnChest(mineSid, bx + 20.0, bz, iron.stringID, 2);
				r.tick(3);
				const Trade* v = r.lastTrade();
				test::require(r.trades.size() > frames1 && std::any_of(v->chests.begin(), v->chests.end(), [&](const Trade::Chest& c) { return c.container == extra; }),
					"a chest entering the set resends TRADE");
			}
			test::require(r.countEvents(40) == closes0, "a failed sale leaves the window open");
			r.teleport(me, bx + 5.0, bz);
			std::cout << "container source ok\n";
		}

		// ---- MQ13: the shared reach rule ----
		{
			test::require(std::fabs(characterReach(2.6, false, false) - 23.6) < 1e-9 && std::fabs(characterReach(2.6, false, true) - 44.6) < 1e-9,
				"a human target: 23.6 while the actor moves, 44.6 once it stops");
			test::require(std::fabs(characterReach(2.6, true, false) - 16.6) < 1e-9 && std::fabs(characterReach(2.6, true, true) - 30.6) < 1e-9,
				"a KO'd target: 16.6 / 30.6");
			test::require(kReachGroundItem == 21.0 && kReachStorage == 25.0, "ground item 21, storage 25");
			const InteractTarget barTarget = findInteractTarget(r.ecs(), r.sim->combat(), barman);
			const kdata::Record* barRace = gd->find(r.fighter(barman).raceSid);
			test::require(barRace != nullptr, "the barman's race record");
			const double radius = barRace->floatOr("pathfind footprint radius", 2.6f); // 4 for the barman's race (a human's is 2.6)
			test::require(std::fabs(barTarget.reach - (radius + 21.0)) < 1e-6 && std::fabs(barTarget.reachStopped - (radius + 42.0)) < 1e-6,
				"the barman's reach is R + 21 / R + 42 from his race's footprint radius " + std::to_string(radius));
			const std::uint64_t chest = r.spawnChest("", 0.0, 0.0, iron.stringID, 1);
			test::require(findInteractTarget(r.ecs(), r.sim->combat(), chest).reach == 25.0, "a storage container's reach is 25");
			state::Fighter& walker = r.fighter(me);
			walker.clearRoute();
			test::require(barTarget.reachFor(walker) == barTarget.reachStopped, "an actor with no route uses the stopped value");
			walker.route.push_back({0.0, 0.0, 0.0});
			walker.routeNext = 0;
			test::require(barTarget.reachFor(walker) == barTarget.reach, "an actor on a route uses the tight value");
			walker.clearRoute();
			std::cout << "reach ok\n";
		}

		// ---- the closes: client, order, trader down / gone; each with event 40 ----
		{
			const auto reasonsOf = [&](std::uint64_t trader)
			{
				std::vector<int> reasons;
				for (const Ev& e : r.events)
				{
					if (e.kind == 40 && e.a == trader)
						reasons.push_back(e.result);
				}
				return reasons;
			};
			const auto reopen = [&]
			{
				// The AI-on barman stands at his counter node (row 20), not his old home point: talk from the customer side of it.
				settleBarman();
				r.teleport(me, r.fighter(barman).x + 5.0, r.fighter(barman).z);
				test::require(r.talk(me, barman).empty(), "talk accepted");
				const std::size_t before = r.trades.size();
				for (int wait = 0; wait < 60 && r.trades.size() == before; ++wait) // the player may have to walk round the counter to reach him
					r.tick(10);
				test::require(r.trades.size() > before, "a TRADE frame follows the talk");
			};
			std::size_t n40 = reasonsOf(barman).size();
			// 1. the client closes: order 86, reason 0; the feed stops with no event 11 for the squad id
			const std::size_t trades0 = r.trades.size();
			test::require(r.tradeClose().empty(), "trade_close accepted");
			r.tick(2);
			test::require(reasonsOf(barman).size() == n40 + 1 && reasonsOf(barman).back() == 0, "event 40 reason 0 (client)");
			n40 = reasonsOf(barman).size();
			{
				flecs::entity e = barSquad;
				state::Inventory v = *e.get<state::Inventory>();
				++v.version;
				e.set<state::Inventory>(std::move(v));
			}
			const std::uint32_t seenVersion = r.inv.at(stockId).version;
			r.tick(5);
			test::require(r.inv.at(stockId).version == seenVersion && r.trades.size() == trades0, "after the close the stock feed and TRADE simply stop");
			test::require(std::none_of(r.events.begin(), r.events.end(), [&](const Ev& e) { return e.kind == 11 && e.a == stockId; }), "no event 11 for the squad id");
			test::require(r.tradeClose().empty(), "closing nothing is accepted too");
			r.tick(2);
			test::require(reasonsOf(barman).size() == n40, "and sends no second event 40");
			{
				const int ironNow = Rig::indexOf(r.inv.at(me), iron.stringID);
				test::require(r.transfer68(me, stockId, r.inv.at(me).version, static_cast<std::uint16_t>(ironNow), 1, false) == "not_allowed", "a transfer with no open trade: not_allowed");
			}
			// 2. reopen: the stock feed starts afresh
			reopen();
			test::require(r.inv.at(stockId).version == seenVersion + 1 || r.inv.at(stockId).version > seenVersion, "the reopened trade resends the stock");
			// 3. MQ11: a move order of either kind keeps the window (window.close_on_move off); with the parameter on a non-queued one closes it
			r.fighter(me).clearRoute();
			test::require(r.moveTo(me, r.fighter(me).x + 20.0, r.fighter(me).z, true).empty(), "a queued move accepted");
			r.tick(2);
			test::require(reasonsOf(barman).size() == n40, "a queued order does not close the trade");
			test::require(r.moveTo(me, r.fighter(me).x + 5.0, r.fighter(me).z, false).empty(), "a move accepted");
			r.tick(2);
			test::require(reasonsOf(barman).size() == n40, "a non-queued move keeps the trade open (MQ11: the original never closes on a move)");
			r.tick(40);
			r.fighter(me).clearRoute();
			r.context.tradeCloseOnMove = true;
			test::require(r.moveTo(me, r.fighter(me).x + 5.0, r.fighter(me).z, false).empty(), "a move accepted");
			r.tick(2);
			test::require(reasonsOf(barman).size() == n40 + 1 && reasonsOf(barman).back() == 1, "close_on_move on: a non-queued move closes it, event 40 reason 1");
			r.context.tradeCloseOnMove = false;
			n40 = reasonsOf(barman).size();
			r.tick(40);
			r.fighter(me).clearRoute();
			reopen();
			// 4. MQ11: a KO of either party removes nothing, the window stays; the trader out of the detail bounds closes it (reason 2)
			r.fighter(barman).unconscious = true;
			r.fighter(barman).koTimer = 1.0e9; // the combat step wakes a character whose KO timer ran out
			r.tick(2);
			test::require(reasonsOf(barman).size() == n40, "the trader KO'd: the window stays open");
			r.fighter(barman).unconscious = false;
			r.fighter(barman).koTimer = 0.0;
			r.tick(2);
			test::require(reasonsOf(barman).size() == n40, "and after he wakes");
			const double bx = r.fighter(barman).x;
			const double bz = r.fighter(barman).z;
			r.teleport(barman, bx + 60000.0, bz);
			r.tick(3);
			test::require(reasonsOf(barman).size() == n40 + 1 && reasonsOf(barman).back() == 2, "the trader out of the detail bounds: event 40 reason 2");
			r.teleport(barman, bx, bz);
			n40 = reasonsOf(barman).size();
			r.tick(3);
			reopen();

			// talking to the trader again while trading resets the window without a close
			reopen();
			n40 = reasonsOf(barman).size();
			reopen();
			test::require(reasonsOf(barman).size() == n40, "talking to the same trader again closes nothing");
			std::cout << "closes ok\n";
		}

		// ---- the buyback list (kenshi-k16.20.16): what a faction sold in this window comes back at the price the merchant paid ----
		{
			r.setWallet(1000000);
			const flecs::entity mineFaction = r.playerFaction(); // a second player faction is made below, and playerFaction() finds the last
			const auto cash = [&]() { return mineFaction.get<state::PlayerFaction>()->money; };
			const state::Inventory packBefore = *r.entityWithId(me).get<state::Inventory>();
			{
				state::Inventory empty = packBefore; // an empty pack: the earlier cases left the grid full of ore and bought goods
				empty.items.clear();
				++empty.version;
				r.entityWithId(me).set<state::Inventory>(std::move(empty));
			}
			std::string item;
			for (const kdata::Record* record : gd->byType(kworld::itemTypeByName("ITEM"))) // a stackable plain item: he pays less than he asks
			{
				state::Inventory::Entry probe{record->stringID, 1, 0, {}};
				const trade::Quote quote = book.quote(hubTerms, probe);
				if (record->intOr("stackable", 1) < 3 || quote.sell <= 0 || quote.buy <= quote.sell)
					continue;
				try
				{
					r.give(me, record->stringID, 3); // two sold, then one
				} catch (const std::exception&) { continue; } // no room
				item = record->stringID;
				break;
			}
			test::require(!item.empty(), "the data has a stackable plain item that sells below its buy price");
			{
				state::Inventory held = *barSquad.get<state::Inventory>(); // two of his own: the sold units will merge into a mixed stack
				state::Inventory::Entry own{item, 2, 0, {}};
				test::require(grid::autoplace(held, grid::Rules(*gd), own) == 0, "room in his stock for two of his own units");
				++held.version;
				barSquad.set<state::Inventory>(std::move(held));
			}
			r.tick(2); // the wire follows the set INVENTORYs
			const auto stockAt = [&]() { return Rig::indexOf(r.inv.at(stockId), item); };
			const auto sellUnits = [&](std::uint16_t units)
			{
				const std::string why = r.transfer68(me, stockId, r.inv.at(me).version, static_cast<std::uint16_t>(Rig::indexOf(r.inv.at(me), item)), units, false);
				test::require(why.empty(), "the sale is accepted (" + why + ")");
				r.tick(3);
			};
			const auto buyUnits = [&](std::uint16_t units)
			{
				const std::string why = r.transfer68(me, stockId, r.inv.at(stockId).version, static_cast<std::uint16_t>(stockAt()), units, true);
				test::require(why.empty(), "the buy is accepted (" + why + ")");
				r.tick(3);
			};
			const Trade::Price opening = r.lastTrade()->stockPrices[static_cast<std::size_t>(stockAt())];
			test::require(r.lastTrade()->buyback.empty(), "nothing sold yet: b = 0");
			const std::uint64_t logged0 = r.context.orderLog.size();
			const std::uint32_t own = Rig::countIn(r.inv.at(stockId), item);
			const std::int64_t cash0 = cash();
			sellUnits(2);
			const std::int64_t paid = cash() - cash0;
			test::require(paid > 0 && paid % 2 == 0, "the merchant paid for two units");
			const int unit = static_cast<int>(paid / 2);
			const Trade::Price now = r.lastTrade()->stockPrices[static_cast<std::size_t>(stockAt())];
			test::require(unit < now.buy, "the price he paid is below the price he asks (" + std::to_string(unit) + " < " + std::to_string(now.buy) + ")");
			test::require(Rig::countIn(r.inv.at(stockId), item) == own + 2, "the sold units joined his own stack");
			{
				const Trade& t = *r.lastTrade();
				test::require(t.buyback.size() == 1 && t.buyback[0].index == stockAt() && t.buyback[0].count == 2 && t.buyback[0].price == unit,
					"the tail lists the merged stack's two units at the price paid (a mixed stack: his own units are not listed)");
				test::require(t.stockPrices[static_cast<std::size_t>(stockAt())].buy == now.buy, "the n block keeps the normal buy price");
			}
			// A mixed stack: three units cost two at the buyback price and one at the buy price.
			const std::int64_t cash1 = cash();
			buyUnits(3);
			test::require(cash1 - cash() == 2 * unit + now.buy, "three units bought back: two at the price paid, one at the buy price (" + std::to_string(cash1 - cash()) + ")");
			test::require(r.lastTrade()->buyback.empty(), "the entry is empty after the units came back: b = 0");
			const std::int64_t cash3 = cash();
			sellUnits(1);
			const int unit1 = static_cast<int>(cash() - cash3);
			test::require(r.lastTrade()->buyback.size() == 1 && r.lastTrade()->buyback[0].count == 1 && r.lastTrade()->buyback[0].price == unit1, "one unit listed");
			// The faction rule: another player faction's character sees no tail and pays the normal price in the same window.
			const std::uint64_t window = std::strtoull(r.context.orderLog.back().command.text.substr(r.context.orderLog.back().command.text.rfind(':') + 1).c_str(), nullptr, 10);
			test::require(window != 0, "the order carries its trade window");
			std::string otherSid;
			r.ecs().each([&](flecs::entity e, const state::FactionState& faction)
			{
				if (otherSid.empty() && !e.has<state::PlayerFaction>() && e.id() != barSquad.get<state::SquadInstance>()->faction.id)
					otherSid = faction.templateSid;
			});
			test::require(!otherSid.empty(), "the data has a second faction");
			r.sim->createPlayerFaction(otherSid);
			r.entityWithId(buddy).set<state::Allegiance>(state::Allegiance{otherSid});
			Simulation::TradeView mine, theirs;
			test::require(r.sim->tradeView(me, barman, window, mine) && mine.buyback.size() == 1, "the seller's view has the entry");
			test::require(r.sim->tradeView(buddy, barman, window, theirs) && theirs.buyback.empty(), "another faction's view: b = 0");
			// Close (86), reopen: the list is gone and a buy costs the normal price.
			test::require(r.tradeClose().empty(), "trade_close accepted");
			r.tick(2);
			r.teleport(me, r.fighter(barman).x + 5.0, r.fighter(barman).z);
			test::require(r.talk(me, barman).empty(), "talk accepted");
			r.tick(10);
			test::require(r.lastTrade()->buyback.empty(), "reopened: no tail");
			const std::int64_t cash4 = cash();
			buyUnits(1);
			test::require(cash4 - cash() == r.lastTrade()->stockPrices[static_cast<std::size_t>(stockAt())].buy, "reopened: the normal price (paid " + std::to_string(cash4 - cash()) + ", buy " + std::to_string(r.lastTrade()->stockPrices[static_cast<std::size_t>(stockAt())].buy) + ", unit1 " + std::to_string(unit1) + ")");
			{
				state::Inventory restore = packBefore;
				restore.version = r.entityWithId(me).get<state::Inventory>()->version + 1;
				r.entityWithId(me).set<state::Inventory>(std::move(restore));
			}
			std::cout << "buyback ok (unit " << unit << ", buy " << now.buy << ", opening " << opening.buy << ", logged " << r.context.orderLog.size() - logged0 << " orders)\n";
		}

		// ---- a save in the middle of a trade, then load and continue to the same hash ----
		{
			r.setWallet(1000);
			const std::uint64_t buddyFrom = r.members[1];
			(void) buddyFrom;
			r.tick(5);
			test::require(r.save("mid").empty(), "save_game accepted");
			const std::uint64_t savedHash = r.sim->stateHash();
			const std::uint32_t seq0 = r.seq;
			const auto script = [&]
			{
				r.seq = seq0;
				r.tick(1);
				test::require(r.talk(me, barman).empty(), "talk after the save accepted");
				r.tick(20);
				const int at = Rig::indexOf(r.inv.at(me), iron.stringID);
				test::require(at >= 0, "the actor has Raw Iron");
				test::require(r.transfer68(me, stockId, r.inv.at(me).version, static_cast<std::uint16_t>(at), 1, false).empty(), "sale after the save accepted");
				r.tick(30);
			};
			script();
			const std::uint64_t hashA = r.sim->stateHash();
			const std::size_t n40 = r.countEvents(40);
			test::require(r.load("mid").empty(), "load_game accepted");
			test::require(r.sim->stateHash() == savedHash, "the loaded world is the saved one (same hash)");
			test::require(r.countEvents(40) == n40 + 1 && r.events.back().kind == 40 && r.events.back().result == 2, "the load closed the open trade: event 40 reason 2 in its write");
			script();
			test::require(r.sim->stateHash() == hashA, "save during an open trade, load, continue: the same hash as the uninterrupted run");
			std::cout << "save ok\n";
		}

		// ---- a wire detail: order 86 carries no body ----
		{
			Buf payload;
			payload.put<std::uint32_t>(++r.seq).put<std::uint16_t>(86).put<std::uint8_t>(0);
			std::vector<std::uint8_t> bytes;
			ks::BinaryWriter w(bytes);
			test::require(!r.session->onFrame(ks::kOrder, payload.bytes.data(), payload.bytes.size(), w), "order 86 with a body is malformed");
		}

		// ---- the whole demo on the real data (the user's acceptance path) ----
		{
			Rig e(order, gd, data, nav, saveDir);
			e.newGame("Wanderer");
			const std::uint64_t hero = e.members[0];
			e.tick(60);
			std::uint64_t ticks = 0;
			const auto advance = [&](int n)
			{
				e.tick(n);
				ticks += static_cast<std::uint64_t>(n);
			};
			// the barman, by data
			flecs::entity squad;
			double best = 1e18;
			e.ecs().each([&](flecs::entity entity, const state::SquadInstance& sq, const state::TraderStock&, const state::Position& at)
			{
				const kdata::Record* t = gd->find(sq.templateSid);
				const double d = std::hypot(at.x - e.fighter(hero).x, at.z - e.fighter(hero).z);
				if (t != nullptr && t->name == "Bar blackmarket" && d < best)
				{
					best = d;
					squad = entity;
				}
			});
			test::require(squad.is_valid(), "the Hub barman's squad");
			std::uint64_t keeper = 0;
			e.ecs().query_builder<const state::Id>().with<state::MemberOf>(squad).build().each([&](flecs::entity, const state::Id& id) { keeper = keeper == 0 ? id.value : keeper; });
			const std::uint64_t shop = squad.get<state::Id>()->value;
			const double barX = e.fighter(keeper).x;
			const double barZ = e.fighter(keeper).z;

			// The Hub's wall gates are ruins with no doors (user, in game 2026-11-04; NAVMESH_CACHE "Conversion"), so nothing seals the
			// town (ff4017c: a gate spawned locked once did); any gate door near the Hub must start open.
			{
				int gates = 0;
				e.ecs().each([&](const state::Door& door)
				{
					if (door.gate && std::hypot(door.x - e.fighter(hero).x, door.z - e.fighter(hero).z) < 2500.0)
					{
						test::require(door.state == 0, "a Hub gate did not start open");
						++gates;
					}
				});
				test::require(gates == 0, "the Hub's ruined gates have doors");
				std::cout << "demo: no Hub gate doors\n";
			}
			// 2. the nearest copper deposit to the barman that the navmesh reaches
			const producers::Book producerBook(*gd);
			const kworld::ZoneCell hub = kworld::zoneCellOf(ks::kHubX, ks::kHubZ);
			bool found = false;
			int reachable = 0, routable = 0, outsideLinked = 0;
			std::array<double, 3> first{};
			ProducerSite site;
			double bestDistance = 1e18, useX = 0, useY = 0, useZ = 0;
			for (int cz = hub.z - 2; cz <= hub.z + 2; ++cz)
			{
				for (int cx = hub.x - 2; cx <= hub.x + 2; ++cx)
				{
					for (const ProducerSite& candidate : e.sim->wildProducerSites(cx, cz))
					{
						const producers::Spec* spec = producerBook.find(candidate.recordSid);
						if (spec == nullptr || spec->resource != 3)
							continue;
						const double d = std::hypot(candidate.x - barX, candidate.z - barZ);
						const auto spot = nav->walker().snap({candidate.x, candidate.y, candidate.z}, producers::kUseSnap);
						if (!spot)
							continue;
						++reachable;
						if (reachable == 1)
							first = {spot->x, spot->y, spot->z};
						else if (nav->routeFrom({first[0], first[1], first[2]}, spot->x, spot->z, spot->y).found)
							++outsideLinked;
						// A partial route (approach) ends short of the spot: a foliage rock cuts its footprint, and the spot snapped to
						// the far side of it can be a pocket the hero cannot reach.
						// At the character's clearance, as the move order below will: a route Detour finds at radius 0 can cross a gap
						// narrower than the body, and the order would answer no_route (NAVMESH_CACHE.md "Clearance").
						const auto route = nav->routeFrom({e.fighter(hero).x, e.fighter(hero).y, e.fighter(hero).z}, spot->x, spot->z, spot->y, false,
							e.sim->pathRadius(e.fighter(hero)));
						// An approach route is the clearance radius stopping the end off the walls (4.5 u at a human's 4): still in reach
						// of the deposit's use point, and the order below routes the same way.
						if (!route.found || route.points.empty() || (route.approach
							&& std::hypot(route.points.back().x - spot->x, route.points.back().z - spot->z) > producers::kUseSnap))
							continue;
						++routable;
						if (d >= bestDistance)
							continue;
						found = true;
						site = candidate;
						bestDistance = d;
						useX = spot->x;
						useY = spot->y;
						useZ = spot->z;
					}
				}
			}
			std::cout << "demo: copper deposits within 2 zone cells: " << reachable << " on the navmesh, " << routable << " with a route from the start, " << outsideLinked << " linked to the first deposit\n";
			test::require(found, "a wild copper deposit on the Hub navmesh with a route from the start");
			std::cout << "demo: the nearest copper deposit (" << gd->find(site.recordSid)->name << " #" << site.id << ") is " << bestDistance << " units from the barman\n";
			const std::string ore = producerBook.find(site.recordSid)->output;

			// walk there
			const std::int64_t money0 = e.wallet();
			const std::string moved = e.moveTo(hero, useX, useZ, false, useY);
			test::require(moved.empty(), "move_to the deposit accepted (" + moved + ")");
			int guard = 0;
			while (std::hypot(e.fighter(hero).x - useX, e.fighter(hero).z - useZ) > 30.0 && guard++ < 400)
				advance(20);
			test::require(guard < 400, "the hero reached the deposit");
			const std::uint64_t walkTicks = ticks;
			advance(30);
			// 3. use it until one ore
			std::string used = e.interact(hero, site.id, 4);
			test::require(used.empty(), "use accepted (" + used + ")");
			guard = 0;
			while (e.carried(hero, ore) == 0 && e.entityWithId(site.id).get<state::Inventory>()->carried(ore) == 0 && guard++ < 6000)
				advance(20);
			test::require(guard < 6000, "one ore was made");
			advance(5);
			const std::uint64_t mineTicks = ticks;
			// 4. take it with 68
			const Inv& deposit = e.inv.at(site.id);
			test::require(deposit.items.size() == 1 && std::get<1>(deposit.items[0]) >= 1, "the deposit's INVENTORY holds the ore");
			test::require(e.transfer68(hero, site.id, deposit.version, 0, 1, true).empty(), "take the ore with order 68");
			advance(3);
			test::require(e.carried(hero, ore) == 1, "the hero carries one " + ore);
			const std::uint64_t takeTicks = ticks;
			// 5. walk to the barman and talk. The order walks there itself, one click (the walker used to stall on a route corner on the
			// climb to the west gate; knav slide now tries every polygon touching a corner).
			test::require(e.talk(hero, keeper).empty(), "talk to the barman accepted from the deposit");
			guard = 0;
			const int reclicks = 0;
			const std::size_t trades0 = e.trades.size();
			while (e.trades.size() == trades0 && guard++ < 1000)
			{
				advance(20);
				test::require(e.fighter(hero).walking() || e.entityWithId(hero).has<state::InteractTask>() || e.trades.size() > trades0,
					"the walk to the barman did not stop short (no re-click needed)");
			}
			test::require(e.trades.size() > trades0, "the walk ended in an open trade (TRADE arrived)");
			const std::uint64_t talkTicks = ticks;
			// 6. sell
			const trade::Book& rules2 = e.sim->tradeBook();
			const int unit = rules2.quote(rules2.termsOf(squad), state::Inventory::Entry{ore, 1, 0, {}}).sell;
			test::require(unit > 0, "the barman buys it");
			const int at = Rig::indexOf(e.inv.at(hero), ore);
			test::require(at >= 0, "the ore is in the hero's INVENTORY");
			test::require(e.transfer68(hero, shop, e.inv.at(hero).version, static_cast<std::uint16_t>(at), 1, false).empty(), "sell accepted");
			advance(3);
			// 7. the money rose
			test::require(e.carried(hero, ore) == 0 && e.wallet() == money0 + unit && e.money.back() == money0 + unit && e.wallet() > money0, "the money rose by the price: " + std::to_string(money0) + " -> " + std::to_string(e.wallet()));
			const double seconds = static_cast<double>(ticks) * kTickSeconds;
			std::cout << "demo: walk to the deposit " << static_cast<double>(walkTicks) * kTickSeconds << " s, mining " << static_cast<double>(mineTicks - walkTicks) * kTickSeconds
				<< " s, take " << static_cast<double>(takeTicks - mineTicks) * kTickSeconds << " s, walk back and talk " << static_cast<double>(talkTicks - takeTicks) * kTickSeconds
				<< " s; total " << seconds << " s of sim time (" << ticks << " ticks, " << seconds / kGameDaySeconds * 24.0 << " game hours, " << reclicks << " re-click); sold 1 " << gd->find(ore)->name << " for " << unit << " (" << money0 << " -> " << e.wallet() << ")\n";
		}
		std::cout << "ksim_trade ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_trade FAILED: " << e.what() << "\n";
		return 1;
	}
}
