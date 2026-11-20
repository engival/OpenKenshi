// A merchant's talk runs his dialogue; the trade reply opens the trade (kenshi-k16.9.3; DIALOGUE.md 13.5, DIALOGUE_ACTIONS_STATIC DA_TRADE):
//  1. dialogue on: the talk to the Hub barman (Bar blackmarket) opens a REPLY_MENU with his trade reply and no TRADE yet; answering it
//     (order 88) sends TRADE 53 for him to the session, and a buy works;
//     the reply ends the conversation first (event 44 reason 0 before TRADE, no menu left open);
//  1b. the bar locked (houseLocked): no root passes, no menu, no trade (the original opens nothing; kenshi-k16.9.12);
//  2. dialogue off: the talk opens the trade directly, as before (the direct open of MINE_SELL_SLICE M4).
// argv[1]: the Hub navmesh cache (`knav_bake`, ctest fixture knav_cache).

#include "ksim/dialogue/dialogue_book.hpp"

#include "trade_rig.hpp"
#include "ksim/dev/build_paths.hpp"

namespace
{

// The barman of the nearest `Bar blackmarket` squad, and the player standing next to him.
std::uint64_t barmanOf(Rig& r, std::uint64_t me)
{
	flecs::entity squad;
	double best = 1e18;
	r.ecs().each([&](flecs::entity e, const state::SquadInstance& sq, const state::TraderStock&, const state::Position& at)
	{
		const kdata::Record* record = r.gd->find(sq.templateSid);
		const double d = std::hypot(at.x - r.fighter(me).x, at.z - r.fighter(me).z);
		if (record != nullptr && record->name == "Bar blackmarket" && d < best)
		{
			best = d;
			squad = e;
		}
	});
	test::require(squad.is_valid() && best < 400.0, "a `Bar blackmarket` squad stands in the start town's bar");
	std::uint64_t barman = 0;
	r.ecs().query_builder<const state::Id>().with<state::MemberOf>(squad).build().each([&](flecs::entity, const state::Id& id)
	{
		barman = barman == 0 ? id.value : barman;
	});
	test::require(barman != 0 && r.sim->traderSquad(barman) != 0, "the barman is a merchant's character");
	state::Fighter& hero = r.fighter(me);
	const state::Fighter& bar = r.fighter(barman);
	hero.x = bar.x + 2.0;
	hero.z = bar.z;
	hero.y = bar.y;
	hero.clearRoute();
	r.tick(3); // the session hears of the barman (menu_query needs a target in its view)
	return barman;
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc == 2, "usage: ksim_barman_trade HUB.knav");
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);
		const auto gd = std::make_shared<const kdata::GameData>(kworld::loadMergedGameData(order));
		const std::shared_ptr<const CombatData> data = CombatData::build(*gd, KENSHI_DATA);
		const std::shared_ptr<MeshNavigator> nav = MeshNavigator::load(argv[1]);
		const std::string saveDir = ks::prepareSaveDir(std::string(KSIM_BINARY_DIR) + "/ksim_barman_trade_test/saves");

		// ---- dialogue on: the greeting, the menu, the trade reply ----
		{
			Rig r(order, gd, data, nav, saveDir);
			r.newGame("Nobodies");
			r.sim->enableDialogue(DialogueBook::build(r.sim->gameData()));
			r.sim->setReplyMenus(true);
			const std::uint64_t me = r.members[0];
			r.tick(60);
			const std::uint64_t barman = barmanOf(r, me);
			r.setWallet(5000);
			// order 99 menu_query: Talk To (bit 0) applies; the query changes no hash, no order log, queues nothing
			const std::uint64_t hash = r.sim->stateHash();
			const std::size_t logged = r.context.orderLog.size();
			test::require(r.menuQuery(me, barman).empty() && !r.menuReplies.empty(), "menu_query on the open barman answered");
			test::require(r.menuReplies.back().id == me && r.menuReplies.back().target == barman && r.menuReplies.back().entries == 1, "MENU_ENTRIES: talk applies (bit 0), nothing else");
			test::require(r.menuQuery(me, me) == "bad_value", "menu_query on oneself: bad_value");
			test::require(r.menuQuery(me, 0xdeadbeef) == "bad_target", "menu_query on an unknown target: bad_target");
			test::require(r.menuQuery(barman, me) == "bad_target", "menu_query by a non-player id: bad_target");
			test::require(r.sim->stateHash() == hash && r.context.orderLog.size() == logged && r.sim->conversation(barman) == nullptr, "a menu query changes nothing");
			test::require(r.talk(me, barman).empty(), "talk on the barman accepted");
			r.tick(60);
			const auto menu = std::find_if(r.events.begin(), r.events.end(), [&](const Ev& e) { return e.kind == 43 && e.a == barman && e.b == me; });
			test::require(menu != r.events.end(), "the barman's greeting opened a REPLY_MENU for the player");
			test::require(r.trades.empty(), "no TRADE before the reply");
			int option = -1;
			for (std::size_t i = 0; i < menu->options.size(); ++i)
			{
				std::cout << "reply " << i << ": " << menu->options[i] << "\n";
				std::string lower = menu->options[i];
				std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
				option = option < 0 && lower.find("no, sorry") == std::string::npos ? static_cast<int>(i) : option; // the trade reply's text is a variant ("Let's trade." / "Show me your goods.")
			}
			test::require(option >= 0, "the menu has a trade reply");
			Buf reply;
			reply.put<std::uint64_t>(me).put<std::uint64_t>(barman).put<std::uint32_t>(menu->seq).put<std::uint8_t>(static_cast<std::uint8_t>(option));
			test::require(r.send(reply, 88).empty(), "the trade reply accepted");
			r.tick(20);
			const Trade* t = r.lastTrade();
			test::require(t != nullptr && t->trader == barman && t->actor == me, "answering the trade reply opened TRADE 53 for the barman");
			// The exe ends the dialogue before it opens the window: CONVERSATION_END (reason 0) reached the client no later than TRADE (one tick packet writes TRADE before its events), and no menu is open.
			const auto ended = std::find_if(r.events.begin(), r.events.end(), [&](const Ev& e) { return e.kind == 44 && e.a == barman && e.b == me; });
			test::require(ended != r.events.end() && ended->result == 0, "the trade reply ended the conversation (event 44 reason 0)");
			test::require(ended->packet <= t->packet, "and the end came in the TRADE's packet or an earlier one");
			test::require(r.sim->openMenus().empty() && r.sim->conversation(barman) == nullptr, "no menu and no conversation stay open behind TRADE");
			const std::uint64_t stock = t->stock;
			const std::int64_t money = r.wallet();
			const Inv& st = r.inv.at(stock);
			test::require(!st.items.empty() && !t->stockPrices.empty(), "the stock and its prices are on the wire");
			const auto cheap = std::find_if(t->stockPrices.begin(), t->stockPrices.end(), [&](const Trade::Price& p) { return p.buy > 0 && p.buy < money; });
			test::require(cheap != t->stockPrices.end(), "an affordable item");
			const std::string refused = r.transfer68(me, stock, st.version, static_cast<std::uint16_t>(cheap->index), 1, true);
			test::require(refused.empty(), "a buy through the opened window is accepted (" + refused + ")");
			r.tick(3);
			test::require(r.wallet() < money, "the buy took money");
			std::cout << "dialogue on ok\n";
		}

		// ---- dialogue on, the bar locked up: no root passes, the talk is nothing_to_say and no trade opens ----
		{
			Rig r(order, gd, data, nav, saveDir);
			r.newGame("Nobodies");
			r.sim->enableDialogue(DialogueBook::build(r.sim->gameData()));
			r.sim->setReplyMenus(true);
			const std::uint64_t me = r.members[0];
			r.tick(60);
			const std::uint64_t barman = barmanOf(r, me);
			r.fighter(barman).houseLocked = true;
			test::require(r.menuQuery(me, barman).empty() && r.menuReplies.back().entries == 0, "menu_query on the locked-up barman: bit 0 = 0");
			test::require(r.talk(me, barman).empty(), "talk on the locked-up barman accepted");
			r.tick(1);
			test::require(r.fighter(barman).houseLocked, "the bar is still locked");
			r.tick(60);
			test::require(std::none_of(r.events.begin(), r.events.end(), [](const Ev& e) { return e.kind == 43; }), "a locked bar: no reply menu");
			test::require(r.trades.empty(), "a locked bar: no trade opens (no fallback)");
			const state::Conversation* talk = r.sim->conversation(barman); // an ambient monologue of his may run; the talk (EV_PLAYER_TALK_TO_ME = 1) opens none
			test::require(talk == nullptr || talk->event != 1, "a locked bar: the talk opened no conversation");
			std::cout << "locked bar ok\n";
		}

		// ---- k16.6.169: the player walks up to the barman, who greets him (event 3, no interrupt root); the talk lands while that greeting
		// runs: the greeting ends and the talk root opens ("Looking to buy?" with its menu), not nothing_to_say ----
		{
			Rig r(order, gd, data, nav, saveDir);
			r.newGame("Wanderer");
			r.sim->enableDialogue(DialogueBook::build(r.sim->gameData()));
			r.sim->setReplyMenus(true);
			const std::uint64_t me = r.members[0];
			r.tick(60);
			const state::Fighter start = r.fighter(me);
			const std::uint64_t barman = barmanOf(r, me);
			r.fighter(me).x = start.x; // barmanOf stood him by the barman; walk from the start spot instead
			r.fighter(me).y = start.y;
			r.fighter(me).z = start.z;
			test::require(r.talk(me, barman).empty(), "talk on the barman accepted");
			r.tick(300);
			test::require(std::any_of(r.events.begin(), r.events.end(), [&](const Ev& e) { return e.kind == 43 && e.a == barman && e.b == me; }),
				"a talk that lands during the barman's own greeting opens his menu");
			std::cout << "talk during greeting ok\n";
		}

		// ---- dialogue off: the direct open ----
		{
			Rig r(order, gd, data, nav, saveDir);
			r.newGame("Nobodies");
			const std::uint64_t me = r.members[0];
			r.tick(60);
			const std::uint64_t barman = barmanOf(r, me);
			test::require(r.menuQuery(me, barman).empty() && r.menuReplies.back().entries == 1, "dialogue off: menu_query on a merchant: bit 0 = 1");
			test::require(r.talk(me, barman).empty(), "talk on the barman accepted");
			r.tick(40);
			const Trade* t = r.lastTrade();
			test::require(t != nullptr && t->trader == barman && t->actor == me, "dialogue off: the talk opens the trade directly");
			test::require(std::none_of(r.events.begin(), r.events.end(), [](const Ev& e) { return e.kind == 43; }), "and no reply menu");
			std::cout << "dialogue off ok\n";
		}
		// ---- paused trade and the order log (kenshi-k16.20.5): a replay of the log reproduces the live order of events ----
		// Live: a buy of item X is queued (the clock runs), the clock is paused, a buy of item Y applies at once and takes the money X needed,
		// the clock resumes and the queued buy fails. The log must hold Y before X, or a replay (enqueue in log order) buys X instead.
		{
			struct Run
			{
				Rig rig;
				std::uint64_t me = 0, stock = 0;
				std::uint16_t a = 0, b = 0; // two stock entries; the wallet affords either, not both
				Run(const kworld::LoadOrder& o, const std::shared_ptr<const kdata::GameData>& g, const std::shared_ptr<const CombatData>& d,
					const std::shared_ptr<MeshNavigator>& n, const std::string& dir)
					: rig(o, g, d, n, dir)
				{
					rig.newGame("Nobodies");
					me = rig.members[0];
					rig.tick(60);
					const std::uint64_t barman = barmanOf(rig, me);
					test::require(rig.talk(me, barman).empty(), "talk on the barman accepted");
					rig.tick(40);
					const Trade* t = rig.lastTrade();
					test::require(t != nullptr && t->trader == barman, "the trade is open");
					stock = t->stock;
					std::vector<Trade::Price> priced;
					for (const Trade::Price& p : t->stockPrices)
					{
						InventoryOrder probe; // an entry that fits the player's grid (no_room otherwise)
						probe.kind = 68;
						probe.id = me;
						probe.container = stock;
						probe.version = rig.inv.at(stock).version;
						probe.index = static_cast<std::uint16_t>(p.index);
						probe.count = 1;
						probe.flag = 1;
						probe.section = 255;
						probe.trader = barman;
						rig.setWallet(1000000);
						if (p.buy > 0 && rig.sim->checkInventoryOrder(probe).empty())
							priced.push_back(p);
					}
					test::require(priced.size() >= 2, "two priced stock entries");
					a = static_cast<std::uint16_t>(priced[0].index);
					b = static_cast<std::uint16_t>(priced[1].index);
					rig.setWallet(std::max(priced[0].buy, priced[1].buy));
				}
				std::uint32_t carried(std::uint16_t index) { return rig.carried(me, std::get<0>(rig.inv.at(stock).items[index])); }
			};
			Run live(order, gd, data, nav, saveDir);
			const std::size_t logged0 = live.rig.context.orderLog.size();
			const std::uint32_t haveA = live.carried(live.a);
			const std::uint32_t haveB = live.carried(live.b);
			const std::string itemA = std::get<0>(live.rig.inv.at(live.stock).items[live.a]);
			const std::string itemB = std::get<0>(live.rig.inv.at(live.stock).items[live.b]);
			test::require(itemA != itemB, "two different items");
			const std::string queued = live.rig.transfer68(live.me, live.stock, live.rig.inv.at(live.stock).version, live.a, 1, true);
			test::require(queued.empty(), "the queued buy is accepted (" + queued + ")");
			live.rig.context.paused = true;
			const std::size_t statsBefore = live.rig.charStatsIds.size();
			test::require(live.rig.transfer68(live.me, live.stock, live.rig.inv.at(live.stock).version, live.b, 1, true).empty(), "the paused buy is accepted");
			test::require(live.rig.carried(live.me, itemB) == haveB + 1, "the paused buy applied at once");
			// The pack got heavier: CHAR_STATS (encumbrance) follows at once, not at the 1 Hz slot a paused clock never reaches (k16.20.15).
			test::require(std::find(live.rig.charStatsIds.begin() + static_cast<std::ptrdiff_t>(statsBefore), live.rig.charStatsIds.end(), live.me)
				!= live.rig.charStatsIds.end(), "the paused buy sends the buyer's CHAR_STATS");
			live.rig.context.paused = false;
			live.rig.tick(5);
			test::require(live.rig.carried(live.me, itemA) == haveA, "the queued buy failed: the paused one took the money first");
			const auto& log = live.rig.context.orderLog;
			test::require(log.size() == logged0 + 2 && log[logged0].immediate && !log[logged0 + 1].immediate && log[logged0].tick == log[logged0 + 1].tick,
				"the log holds the paused buy ahead of the queued one for the same tick");
			Run replay(order, gd, data, nav, saveDir);
			test::require(replay.rig.sim->clockTick() == live.rig.sim->clockTick() - 5 && replay.rig.sim->stateHash() != live.rig.sim->stateHash(), "the replay starts at the live run's start");
			for (std::size_t i = logged0; i < log.size(); ++i)
				replay.rig.sim->enqueue(log[i].command);
			replay.rig.tick(5);
			test::require(replay.rig.sim->clockTick() == live.rig.sim->clockTick() && replay.rig.sim->stateHash() == live.rig.sim->stateHash(),
				"the replay of the order log reaches the same state hash");
			std::cout << "paused order log replay ok\n";
		}
		// ---- the buyback list and the order log (kenshi-k16.20.16): the windows ride in the logged orders, so a replay charges the same prices ----
		// Live: sell two of a stackable item, buy three back (two at the price paid), sell one, close the window (86), talk again, buy one (the
		// normal price: the list went with the window). The replay enqueues the logged commands only; it has no Session and sees no close, so
		// the new window id in the last buy's command is what drops the list there.
		{
			struct Run
			{
				Rig rig;
				std::uint64_t me = 0, barman = 0, stock = 0;
				std::string item;
				Run(const kworld::LoadOrder& o, const std::shared_ptr<const kdata::GameData>& g, const std::shared_ptr<const CombatData>& d,
					const std::shared_ptr<MeshNavigator>& n, const std::string& dir)
					: rig(o, g, d, n, dir)
				{
					rig.newGame("Nobodies");
					me = rig.members[0];
					rig.tick(60);
					barman = barmanOf(rig, me);
					test::require(rig.talk(me, barman).empty(), "talk on the barman accepted");
					rig.tick(40);
					stock = rig.lastTrade()->stock;
					rig.setWallet(1000000);
					const flecs::entity squad = rig.entityWithId(stock);
					const trade::Terms terms = rig.sim->tradeBook().termsOf(squad);
					state::Inventory pack = *rig.entityWithId(me).get<state::Inventory>();
					pack.items.clear();
					++pack.version;
					rig.entityWithId(me).set<state::Inventory>(std::move(pack));
					for (const kdata::Record* record : g->byType(kworld::itemTypeByName("ITEM")))
					{
						const trade::Quote quote = rig.sim->tradeBook().quote(terms, state::Inventory::Entry{record->stringID, 1, 0, {}});
						if (record->intOr("stackable", 1) < 3 || quote.sell <= 0 || quote.buy <= quote.sell)
							continue;
						try
						{
							rig.give(me, record->stringID, 3);
						} catch (const std::exception&) { continue; }
						item = record->stringID;
						break;
					}
					test::require(!item.empty(), "a stackable plain item that sells below its buy price");
					state::Inventory held = *squad.get<state::Inventory>();
					state::Inventory::Entry own{item, 2, 0, {}};
					test::require(grid::autoplace(held, grid::Rules(*g), own) == 0, "room in his stock");
					++held.version;
					squad.set<state::Inventory>(std::move(held));
					rig.tick(2);
				}
				void sell(std::uint16_t units)
				{
					test::require(rig.transfer68(me, stock, rig.inv.at(me).version, static_cast<std::uint16_t>(Rig::indexOf(rig.inv.at(me), item)), units, false).empty(), "sale accepted");
					rig.tick(3);
				}
				void buy(std::uint16_t units)
				{
					test::require(rig.transfer68(me, stock, rig.inv.at(stock).version, static_cast<std::uint16_t>(Rig::indexOf(rig.inv.at(stock), item)), units, true).empty(), "buy accepted");
					rig.tick(3);
				}
			};
			Run live(order, gd, data, nav, saveDir);
			const std::size_t logged0 = live.rig.context.orderLog.size();
			const std::uint64_t tick0 = live.rig.sim->clockTick();
			const std::int64_t cash0 = live.rig.wallet();
			live.sell(2);
			const std::int64_t unit = (live.rig.wallet() - cash0) / 2;
			const std::int64_t cash1 = live.rig.wallet();
			live.buy(3);
			const std::int64_t askUnit = live.rig.lastTrade()->stockPrices[static_cast<std::size_t>(Rig::indexOf(live.rig.inv.at(live.stock), live.item))].buy;
			test::require(cash1 - live.rig.wallet() == 2 * unit + askUnit && askUnit > unit, "live: two units back at the price paid, one at the buy price");
			live.sell(1);
			test::require(live.rig.tradeClose().empty(), "trade_close accepted");
			live.rig.tick(2);
			test::require(live.rig.talk(live.me, live.barman).empty(), "talk again accepted");
			live.rig.tick(40);
			test::require(live.rig.lastTrade()->buyback.empty(), "reopened: no tail");
			const std::int64_t cash2 = live.rig.wallet();
			live.buy(1);
			test::require(cash2 - live.rig.wallet() == askUnit, "reopened: the normal price");
			Run replay(order, gd, data, nav, saveDir);
			test::require(replay.rig.sim->clockTick() == tick0 && replay.rig.sim->stateHash() != live.rig.sim->stateHash(), "the replay starts at the live run's start");
			const auto& log = live.rig.context.orderLog;
			for (std::size_t i = logged0; i < log.size(); ++i)
			{
				if (log[i].sim)
					replay.rig.sim->enqueue(log[i].command);
			}
			replay.rig.tick(static_cast<int>(live.rig.sim->clockTick() - tick0));
			test::require(replay.rig.sim->clockTick() == live.rig.sim->clockTick() && replay.rig.sim->stateHash() == live.rig.sim->stateHash(),
				"the replay of the order log reaches the same state hash");
			std::cout << "buyback order log replay ok\n";
		}
		std::cout << "ksim_barman_trade ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cout << "ksim_barman_trade: " << e.what() << "\n";
		return 1;
	}
}
