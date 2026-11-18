// The reply menu at the server / wire level (docs/DIALOGUE.md 13.2 the pause, 13.3 the grace, 13.4 the resend; 13.8 rows 1-3), on the
// real data: an in-process ksim server (a thread running SimLinkServer on a loopback port) and a test client built on the client session's
// own decoder (client/src/net/simlink.cpp, compiled in unchanged). The sim-level lifecycle is `ksim_dialogue_events` "reply menus"; this pins
// what only the server can show:
//
//  1. Freeze: an open menu sets CLOCK's dialogue bit and stops the tick; a reply lifts it and applies on the next tick. The pause is
//     invisible to the sim: a twin loaded from the frozen save and answered on the same tick hashes the same later.
//  2. end_talk lifts it the same way, with CONVERSATION_END reason 4.
//  3. Reconnect inside the grace: the initial snapshot resends REPLY_MENU with the resend flag and the same menuSeq, and the freeze
//     re-engages.
//  4. Disconnect mid-menu: the gone session clears the hold, the world ticks again (Hold); the menu survives 5 s, and 30 s after the
//     drop the server's own logged EndConversation (reason 5) ends it.
//
// Fixtures order only the player: the characters are placed, and the player's talk and reply are the only orders sent.
//
// Not pinned here (13.8 rows that need something 13 calls UNSURE / not yet): row 2's "T's hold is gone, a move order applies"
// (`Fighter::hold` / `in_conversation` are not modelled under V1 Freeze), row 2's "the NPC picks a goal" (the fixture has no AI or
// squad for the bandits), the Hold-policy hash comparison (no pause_policy option forces Hold for a Freeze dialogue), and row 3's
// uninterrupted-hash equality (the grace's Hold ticks are real sim time, so only an instantaneous reconnect would match).

#include "ksim/core/clock.hpp"
#include "ksim/core/command.hpp"
#include "ksim/dialogue/dialogue_book.hpp"
#include "ksim/dialogue/dialogue_events.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/net/simlink.hpp"
#include "ksim/net/simlink_scenario.hpp"
#include "ksim/core/simulation.hpp"

#include "kworld/gamedata.hpp"
#include "kworld/heightmap.hpp"
#include "kworld/loadorder.hpp"

#include "net/simlink.hpp" // client/src/net/simlink.hpp

#include "test_util.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <numbers>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

namespace ks = ksim::simlink;
using ksim::state::Id;
using Wall = std::chrono::steady_clock;

constexpr int kNormal = 13;

void check(bool condition, const std::string& what)
{
	test::require(condition, what);
	std::printf("ok  %s\n", what.c_str());
}

void sleepMs(int ms)
{
	std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// ---- the world ----

struct Data
{
	std::shared_ptr<const kdata::GameData> gd;
	std::shared_ptr<const ksim::CombatData> combat;
	std::shared_ptr<const ksim::DialogueBook> book;
	kworld::LoadOrder order;
	kworld::Heightmap terrain;
	std::string servant; // the body every rig character is built from
	std::string nameless; // the player faction
	std::string starving; // Starving Bandits
	std::string starver; // a CHARACTER whose set roots the mugging (4853-D-dialogue-02.mod under EV_I_SEE_ENEMY_PLAYER)

	Data()
		: order(kworld::vanillaLoadOrder(KENSHI_DATA)),
		  terrain(kworld::Heightmap::open(std::string(KENSHI_DATA) + "/newland/land/fullmap.tif"))
	{
		gd = std::make_shared<const kdata::GameData>(kworld::loadMergedGameData(order));
		combat = ksim::CombatData::build(*gd, KENSHI_DATA);
		book = ksim::DialogueBook::build(gd);
		for (const kdata::Record* r : gd->byType(kworld::itemTypeByName("FACTION")))
		{
			if (r->name == "Nameless")
				nameless = r->stringID;
			if (r->stringID == "1305-gamedata.base")
				starving = r->stringID;
		}
		const auto rootsUnder = [&](const kdata::Record& character, int event, std::string_view root)
		{
			ksim::SpeakerKey key;
			key.character = character.stringID;
			key.leader = true;
			const ksim::PackageSet& set = book->packageSet(book->packageSetFor(key));
			const std::uint32_t index = book->dialogueIndex(root);
			for (const std::uint32_t position : set.rootsFor(event))
			{
				if (set.roots[position] == index)
					return true;
			}
			return false;
		};
		for (const kdata::Record* r : gd->byType(kworld::itemTypeByName("CHARACTER")))
		{
			if (r->name == "Escaped Servant" && servant.empty())
				servant = r->stringID;
			if (starver.empty() && rootsUnder(*r, ksim::ev::ISeeEnemyPlayer, "4853-D-dialogue-02.mod")
				&& rootsUnder(*r, ksim::ev::ISeeNeutralSquad, "4853-D-dialogue-02.mod"))
				starver = r->stringID;
		}
		check(!nameless.empty() && !starving.empty() && !servant.empty() && !starver.empty(),
			"data records: the Nameless and Starving Bandits factions, the servant body, the starver character");
	}
};

struct World
{
	std::shared_ptr<const Data> data;
	std::shared_ptr<ksim::MeshNavigator> nav; // held: enableNavigation keeps its address
	std::unique_ptr<ksim::Simulation> sim;
	ks::ServerContext context;
	std::uint64_t player = 0;
	std::vector<std::uint64_t> bandits;
	std::uint64_t rng = 7;

	World(std::shared_ptr<const Data> world, std::shared_ptr<ksim::MeshNavigator> navigation, bool freeze, std::uint64_t seed = 7)
		: data(std::move(world)), nav(std::move(navigation)), rng(seed)
	{
		context.dialogueFreeze = freeze; // the rows below the Hold ones run the V1 Freeze policy (`--dialogue-pause freeze`)
		ksim::CombatSettings settings;
		settings.substeps = 2;
		sim = std::make_unique<ksim::Simulation>(ksim::Simulation::load(KENSHI_DATA, seed));
		sim->setGameData(data->gd);
		sim->setClockTick(ksim::tickAt(ksim::kNewGameDay, ksim::kNewGameHour, 0));
		sim->enableCombat(data->combat, settings);
		sim->enableDialogue(data->book);
		sim->setReplyMenus(true); // the server's flag: a menu nobody answers is ended by the grace
		sim->enableNavigation(nav);
		context.gameData = data->gd;
		context.dataHash = ks::dataHash(data->order);
		context.squadLimit = 50;
		player = spawn("", data->nameless, 9, ks::kHubX, ks::kHubZ, std::numbers::pi, true);
		// A tight ring around the player: whichever starver starts the mugging is already inside the talk reach, so the player's talk
		// order (order 60) needs no walk (the route would work too, but the line's timer must not run out on the way).
		for (int i = 0; i < 12; ++i)
		{
			const double angle = i * (2.0 * std::numbers::pi / 12.0);
			bandits.push_back(spawn(data->starver, data->starving, 8, ks::kHubX + 4.0 * std::sin(angle), ks::kHubZ + 4.0 * std::cos(angle),
				0.0, false));
		}
	}

	std::uint64_t spawn(const std::string& character, const std::string& faction, int cls, double x, double z, double yaw, bool player)
	{
		ksim::state::Fighter f = data->combat->makeFighter(*data->gd, data->servant, "", rng);
		f.team = 0;
		f.npcClass = static_cast<std::uint8_t>(cls);
		f.playerControlled = player;
		f.attention = 0.5;
		f.yaw = yaw;
		const auto spot = nav->walker().snap({x, data->terrain.terrainHeight(x, z), z}, 100.0);
		test::require(spot.has_value(), "the Hub's ground is not on the navmesh");
		f.x = spot->x;
		f.y = spot->y;
		f.z = spot->z;
		flecs::entity e = sim->spawnFighter(std::move(f));
		e.set<ksim::state::Allegiance>(ksim::state::Allegiance{faction});
		if (!character.empty())
			e.set<ksim::state::CharacterInstance>(ksim::state::CharacterInstance{character});
		const std::uint64_t id = e.get<ksim::state::Id>()->value;
		if (!player)
			sim->dialogue()->setInputs(id, ksim::DialogueInputs{kNormal, false, false});
		return id;
	}
};

// ---- the server ----

struct Server
{
	std::promise<int> listening;
	ks::SimLinkServer server;
	std::atomic<bool> stop{false};
	std::thread thread;
	int port = 0;

	explicit Server(World& w, bool runWithoutClients = false)
		: server(*w.sim, w.context, options(runWithoutClients, &listening))
	{
		server.listen();
		port = listening.get_future().get();
		thread = std::thread([this]() { server.run(stop); });
	}
	~Server() { stopNow(); }
	Server(const Server&) = delete;
	Server& operator=(const Server&) = delete;

	// Stops the loop and joins: the sim is then the test thread's alone.
	void stopNow()
	{
		stop.store(true);
		if (thread.joinable())
			thread.join();
	}

private:
	static ks::ServerOptions options(bool runWithoutClients, std::promise<int>* listening)
	{
		ks::ServerOptions o;
		o.runWithoutClients = runWithoutClients;
		o.onListening = [listening](int port) { listening->set_value(port); };
		return o;
	}
};

// ---- the client (the client session's own decoder) ----

template <typename T>
T readAt(const std::uint8_t* p, std::size_t at)
{
	T v;
	std::memcpy(&v, p + at, sizeof(T));
	return v;
}

template <typename T>
void put(std::vector<std::uint8_t>& v, T value)
{
	const auto* b = reinterpret_cast<const std::uint8_t*>(&value);
	v.insert(v.end(), b, b + sizeof(T));
}

std::vector<std::uint8_t> rawOrder(std::uint32_t seq, std::uint16_t kind, const std::function<void(std::vector<std::uint8_t>&)>& body)
{
	std::vector<std::uint8_t> payload;
	put(payload, seq);
	put(payload, kind);
	if (body)
		body(payload);
	std::vector<std::uint8_t> out;
	const std::uint32_t len = static_cast<std::uint32_t>(payload.size() + 2);
	const std::uint16_t type = ks::kOrder;
	out.resize(6);
	std::memcpy(out.data(), &len, 4);
	std::memcpy(out.data() + 4, &type, 2);
	out.insert(out.end(), payload.begin(), payload.end());
	return out;
}

std::vector<std::uint8_t> helloFrame(std::uint64_t hash)
{
	std::vector<std::uint8_t> out;
	simlink::OrderWriter::hello(out, hash, "test");
	return out;
}

struct Client
{
	int fd = -1;
	std::vector<std::uint8_t> raw;
	simlink::SimLinkDecoder dec;
	std::vector<simlink::Event> events;
	std::uint64_t snapshots = 0;
	std::uint64_t lastSnapshotEnd = 0;
	std::uint64_t openTick = 0;
	bool inSnap = false;

	explicit Client(int port)
	{
		fd = ::socket(AF_INET, SOCK_STREAM, 0);
		test::require(fd >= 0, "client socket");
		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_port = htons(static_cast<std::uint16_t>(port));
		::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
		test::require(::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "connect");
	}
	~Client() { close(); }
	Client(const Client&) = delete;
	Client& operator=(const Client&) = delete;

	void close()
	{
		if (fd >= 0)
			::close(fd);
		fd = -1;
	}

	void send(const std::vector<std::uint8_t>& bytes)
	{
		std::size_t at = 0;
		while (at < bytes.size())
		{
			const ssize_t n = ::send(fd, bytes.data() + at, bytes.size() - at, MSG_NOSIGNAL);
			test::require(n > 0, "client send");
			at += static_cast<std::size_t>(n);
		}
	}

	void onFrame(std::uint16_t type, const std::uint8_t* p, std::size_t n)
	{
		const auto tickOf = [&]() { std::uint64_t t = 0; test::require(n >= 8, "short frame"); std::memcpy(&t, p, 8); return t; };
		switch (type)
		{
		case ks::kSnapBegin:
			test::require(!inSnap, "SNAPSHOT_BEGIN inside a snapshot");
			openTick = tickOf();
			test::require(openTick > lastSnapshotEnd || snapshots == 0, "snapshot ticks do not increase");
			inSnap = true;
			break;
		case ks::kSnapEnd:
			test::require(inSnap, "SNAPSHOT_END without BEGIN");
			test::require(tickOf() == openTick, "SNAPSHOT_END tick differs from BEGIN");
			lastSnapshotEnd = openTick;
			++snapshots;
			inSnap = false;
			break;
		case ks::kAppearance:
		case ks::kDynamic:
		case ks::kSquads:
		case ks::kBounds:
			test::require(inSnap, "character/squad/bounds record outside a snapshot");
			test::require(tickOf() == openTick, "record tick differs from its snapshot");
			break;
		case 23: // CHAR_ROUTE
			test::require(inSnap, "CHAR_ROUTE outside a snapshot");
			test::require(tickOf() == openTick, "CHAR_ROUTE tick differs from its snapshot");
			break;
		case ks::kEvent:
			test::require(!inSnap, "EVENT inside a snapshot");
			break;
		default:
			break;
		}
		// event 12 (position_corrected): the decoder predates it, so the test consumes it here.
		if (type == ks::kEvent && n >= 10 && readAt<std::uint16_t>(p, 8) == 12)
			return;
		test::require(dec.feed(p - 6, n + 6), "decoder (frame type " + std::to_string(type) + "): " + dec.error());
		while (!dec.events.empty())
		{
			events.push_back(dec.events.front());
			dec.events.pop_front();
		}
	}

	bool pump(const std::function<bool()>& pred, int timeoutMs)
	{
		const auto end = Wall::now() + std::chrono::milliseconds(timeoutMs);
		while (!pred())
		{
			if (Wall::now() > end)
				return false;
			pollfd p{fd, POLLIN, 0};
			if (::poll(&p, 1, 20) <= 0)
				continue;
			std::uint8_t block[65536];
			const ssize_t n = ::recv(fd, block, sizeof(block), 0);
			if (n <= 0)
				return pred();
			raw.insert(raw.end(), block, block + n);
			std::size_t at = 0;
			while (raw.size() - at >= 6)
			{
				std::uint32_t len;
				std::uint16_t type;
				std::memcpy(&len, &raw[at], 4);
				if (raw.size() - at - 4 < len)
					break;
				std::memcpy(&type, &raw[at + 4], 2);
				if (type == ks::kError)
				{
					dec.feed(&raw[at], 4 + len);
					raw.erase(raw.begin(), raw.begin() + static_cast<std::ptrdiff_t>(at + 4 + len));
					return true;
				}
				onFrame(type, &raw[at + 6], len - 2);
				at += 4 + len;
			}
			raw.erase(raw.begin(), raw.begin() + static_cast<std::ptrdiff_t>(at));
		}
		return true;
	}

	const simlink::Event* speech(std::string_view line) const
	{
		for (const simlink::Event& e : events)
		{
			if (e.kind == 41 && (line.empty() || dec.str(e.ref[1]) == line))
				return &e;
		}
		return nullptr;
	}
	const simlink::Event* answer(std::uint64_t who) const
	{
		for (const simlink::Event& e : events)
		{
			if (e.kind == 41 && e.a == who && (e.dir & 4) != 0)
				return &e;
		}
		return nullptr;
	}
	const simlink::Event* talkEnd(std::uint64_t speaker) const
	{
		for (const simlink::Event& e : events)
		{
			if (e.kind == 44 && e.a == speaker)
				return &e;
		}
		return nullptr;
	}
	const simlink::SimLinkDecoder::ReplyMenu* menu(std::uint64_t ch) const
	{
		for (const simlink::SimLinkDecoder::ReplyMenu& m : dec.menus)
		{
			if (m.ch == ch)
				return &m;
		}
		return nullptr;
	}
};

// ---- the sections ----

// The world runs until a starver speaks the mugging's first line; the player's talk opens the menu. Returns the speaker.
std::uint64_t openMenu(World& w, Client& c)
{
	c.send(helloFrame(w.context.dataHash));
	test::require(c.pump([&]() { return c.snapshots >= 1; }, 5000), "no initial snapshot after HELLO");
	test::require(c.dec.serverDataHash() == w.context.dataHash, "the server's HELLO carries the data hash the client sent");
	test::require(c.pump([&]() { return c.speech("4871-dialogue-02.mod") != nullptr; }, 20000), "the mugging's first line never reached the wire");
	const std::uint64_t speaker = c.speech("4871-dialogue-02.mod")->a;
	// Order 60 interact, Talk (6), queue 0, no flags: the player talks to the busy speaker.
	c.send(rawOrder(90, 60, [&](auto& v) {
		put<std::uint16_t>(v, 1);
		put<std::uint64_t>(v, w.player);
		put<std::uint64_t>(v, speaker);
		put<std::uint8_t>(v, 6);
		put<std::uint8_t>(v, 0);
	}));
	test::require(c.pump([&]() { return c.menu(w.player) != nullptr; }, 5000), "the talk opened no REPLY_MENU for the player");
	return speaker;
}

std::vector<std::uint8_t> replyOrder(std::uint64_t player, std::uint64_t speaker, std::uint32_t seq, std::uint8_t option)
{
	return rawOrder(91, 88, [&](auto& v) {
		put<std::uint64_t>(v, player);
		put<std::uint64_t>(v, speaker);
		put<std::uint32_t>(v, seq);
		put<std::uint8_t>(v, option);
	});
}

std::vector<std::uint8_t> moveOrder(std::uint64_t who, double x, double z)
{
	return rawOrder(93, 1, [&](auto& v) {
		put<std::uint16_t>(v, 1);
		put<std::uint64_t>(v, who);
		put<float>(v, static_cast<float>(x));
		put<float>(v, static_cast<float>(z));
		put<float>(v, 0.0f);
		put<std::uint8_t>(v, 0);
	});
}

std::vector<std::uint8_t> endTalkOrder(std::uint64_t player, std::uint64_t speaker)
{
	return rawOrder(92, 89, [&](auto& v) {
		put<std::uint64_t>(v, player);
		put<std::uint64_t>(v, speaker);
	});
}

// 13.8 row 1: the menu freezes the tick (CLOCK bit 1), the reply lifts it, and a twin answered at the same tick hashes the same.
void freezeAndReply(const std::shared_ptr<const Data>& data, std::shared_ptr<ksim::MeshNavigator> nav)
{
	World w(data, nav, true);
	Server server(w);
	Client c(server.port);
	const std::uint64_t speaker = openMenu(w, c);
	const simlink::SimLinkDecoder::ReplyMenu menu = *c.menu(w.player);
	check((menu.flags & simlink::SimLinkDecoder::ReplyMenu::kFreezes) != 0, "REPLY_MENU says it freezes (flags bit 2)");
	check((menu.flags & simlink::SimLinkDecoder::ReplyMenu::kResend) == 0, "the first REPLY_MENU is not a resend");
	check((c.dec.clock.pauseBits & 2) != 0, "CLOCK says the dialogue pause (paused bit 1)");
	check(w.context.menuHolds.size() == 1 && w.context.menuHolds[0] == speaker, "the server holds the menu's speaker");
	const std::uint64_t frozen = w.sim->clockTick();
	const std::uint64_t frozenHash = w.sim->stateHash();
	sleepMs(2000);
	check(w.sim->clockTick() == frozen, "the tick advanced while the menu was open (2 s of wall clock)");
	check(w.sim->stateHash() == frozenHash, "the sim hash moved while the menu was open");
	std::stringstream saved;
	w.sim->save(saved);
	c.send(replyOrder(w.player, speaker, menu.seq, 0));
	test::require(c.pump([&]() { return c.answer(w.player) != nullptr; }, 5000), "the reply was never spoken by the player");
	check(c.dec.str(c.answer(w.player)->str) == c.dec.str(menu.options[0].text),
		"the reply speaks the menu's first option: \"" + c.dec.str(c.answer(w.player)->str) + "\"");
	test::require(c.pump([&]() { return (c.dec.clock.pauseBits & 2) == 0; }, 5000), "the freeze did not lift after the reply");
	test::require(c.pump([&]() { return w.sim->clockTick() > frozen + 40; }, 5000), "the sim did not resume after the reply");
	// Freeze the wire run at a known tick, then replay the frozen state plus the reply to the same tick.
	c.send(rawOrder(93, 40, [](auto& v) { put<std::uint8_t>(v, 1); })); // the admin pause
	test::require(c.pump([&]() { return c.dec.clock.paused; }, 5000), "the admin pause was not applied");
	server.stopNow();
	const std::uint64_t wireTick = w.sim->clockTick();
	const std::uint64_t wireHash = w.sim->stateHash();
	World twin(data, nav, true);
	saved.seekg(0);
	twin.sim->reload(saved);
	check(twin.sim->clockTick() == frozen && twin.sim->stateHash() == frozenHash, "the twin loads the frozen state (tick and hash)");
	ksim::Command answer = ksim::Command::dialogueReply(Id{w.player}, Id{speaker}, menu.seq, 0);
	answer.tick = frozen + 1;
	twin.sim->enqueue(answer);
	twin.sim->run(wireTick - frozen);
	check(twin.sim->clockTick() == wireTick, "the twin ran to the wire's tick");
	check(twin.sim->stateHash() == wireHash, "the pause is invisible: the twin answered at the same tick hashes the same");
	std::printf("  freeze: %llu ticks of wall clock held, reply applied, hash %llx at tick %llu\n",
		static_cast<unsigned long long>(wireTick - frozen), static_cast<unsigned long long>(wireHash),
		static_cast<unsigned long long>(wireTick));
}

// 13.8 row 1's end_talk half: the order lifts the freeze and the menu ends with CONVERSATION_END 4.
void endTalk(const std::shared_ptr<const Data>& data, std::shared_ptr<ksim::MeshNavigator> nav)
{
	World w(data, nav, true);
	Server server(w);
	Client c(server.port);
	const std::uint64_t speaker = openMenu(w, c);
	const std::uint64_t frozen = w.sim->clockTick();
	c.send(endTalkOrder(w.player, speaker));
	test::require(c.pump([&]() { return c.talkEnd(speaker) != nullptr; }, 5000), "end_talk raised no CONVERSATION_END");
	check(c.talkEnd(speaker)->result == 4, "end_talk ends the conversation with reason 4 (walked away)");
	test::require(c.pump([&]() { return (c.dec.clock.pauseBits & 2) == 0; }, 5000), "end_talk did not lift the freeze");
	test::require(c.pump([&]() { return w.sim->clockTick() > frozen + 20; }, 5000), "the sim did not resume after end_talk");
	check(w.context.menuHolds.empty(), "the server holds no speaker after end_talk");
	std::cout << "end_talk: reason 4, the freeze lifted, the tick resumed\n";
}

// 13.8 row 3: a reconnect inside the grace gets the menu again, with the resend flag and the same seq, and freezes again.
void reconnect(const std::shared_ptr<const Data>& data, std::shared_ptr<ksim::MeshNavigator> nav)
{
	World w(data, nav, true);
	Server server(w, true); // no session idle pause: the tick between the drop and the reconnect is the hold's clearing
	auto c = std::make_unique<Client>(server.port);
	const std::uint64_t speaker = openMenu(w, *c);
	const simlink::SimLinkDecoder::ReplyMenu first = *c->menu(w.player);
	const std::uint64_t frozen = w.sim->clockTick();
	c.reset(); // the drop
	sleepMs(400);
	test::require(w.sim->clockTick() > frozen + 2, "the sim did not tick between the drop and the reconnect (Hold)");
	c = std::make_unique<Client>(server.port);
	c->send(helloFrame(w.context.dataHash));
	test::require(c->pump([&]() { return c->menu(w.player) != nullptr; }, 5000), "the reconnect snapshot carried no REPLY_MENU");
	const simlink::SimLinkDecoder::ReplyMenu again = *c->menu(w.player);
	check(again.speaker == speaker && again.seq == first.seq, "the resend is the same menu (speaker and menuSeq)");
	check((again.flags & simlink::SimLinkDecoder::ReplyMenu::kResend) != 0, "the resend carries the resend flag (bit 0)");
	check((again.flags & simlink::SimLinkDecoder::ReplyMenu::kFreezes) != 0, "the resend still says it freezes");
	test::require(c->pump([&]() { return (c->dec.clock.pauseBits & 2) != 0; }, 5000), "the resend did not re-freeze the tick");
	const std::uint64_t refrozen = w.sim->clockTick();
	sleepMs(500);
	check(w.sim->clockTick() == refrozen, "the tick ran after the resend re-froze it");
	c->send(replyOrder(w.player, speaker, again.seq, 0));
	test::require(c->pump([&]() { return c->answer(w.player) != nullptr; }, 5000), "the reply to the resent menu was not spoken");
	test::require(c->pump([&]() { return c->menu(w.player) == nullptr; }, 5000), "the resent menu stayed open after the reply");
	std::cout << "reconnect: the resend flag and the same seq, frozen again, the reply applies\n";
}

// 13.8 row 2: the dropped session clears the hold (the world ticks under Hold), the menu survives 5 s, and the 30 s grace ends
// it with the server's own logged EndConversation (reason 5). The grace is wall clock: the test sleeps it out.
void grace(const std::shared_ptr<const Data>& data, std::shared_ptr<ksim::MeshNavigator> nav)
{
	World w(data, nav, true);
	Server server(w, true);
	auto c = std::make_unique<Client>(server.port);
	const std::uint64_t speaker = openMenu(w, *c);
	check((c->dec.clock.pauseBits & 2) != 0, "CLOCK says the dialogue pause before the drop");
	const std::uint64_t frozen = w.sim->clockTick();
	c.reset();
	const std::uint64_t atDrop = w.sim->clockTick();
	check(atDrop <= frozen + 2, "the sim ticked after the menu opened");
	sleepMs(1500);
	check(w.sim->clockTick() > atDrop + 10,
		"the tick resumed when the session went (Hold): " + std::to_string(w.sim->clockTick() - atDrop) + " ticks");
	check(w.context.menuHolds.empty(), "the gone session cleared the hold");
	sleepMs(3500); // 5 s since the drop: inside the grace, the menu is still open
	c = std::make_unique<Client>(server.port);
	c->send(helloFrame(w.context.dataHash));
	test::require(c->pump([&]() { return c->menu(w.player) != nullptr; }, 5000), "the reconnect at 5 s did not resend the menu (it ended early)");
	check((c->menu(w.player)->flags & simlink::SimLinkDecoder::ReplyMenu::kResend) != 0, "the 5 s resend carries the resend flag");
	c.reset();
	const std::uint64_t atSecondDrop = w.sim->clockTick();
	sleepMs(30500); // the 30 s session grace
	server.stopNow();
	check(w.sim->conversation(speaker) == nullptr, "the menu is still open past the grace");
	std::uint64_t ended = 0;
	for (const ks::OrderLogEntry& entry : w.context.orderLog)
	{
		if (entry.kind != 89)
			continue;
		check(entry.command.type == ksim::CommandType::EndConversation, "the grace's entry is a sim command");
		check(entry.command.subject == w.player && entry.command.object == speaker, "it names the player and the speaker");
		check(entry.command.value == 5, "it is reason 5 (no client)");
		check(entry.text.find("reason=5 (no client)") != std::string::npos, "the order log says reason 5 (no client)");
		check(entry.tick > atSecondDrop, "the EndConversation was enqueued after the drop");
		++ended;
	}
	check(ended == 1, "the grace left exactly one EndConversation in the order log (found " + std::to_string(ended) + ")");
	std::cout << "grace: the tick resumed at the drop, the menu survived 5 s, one EndConversation 5 after 30 s\n";
}

// DIALOGUE.md 13.4, the V1 default since 2026-11-12 (kenshi-k16.9.10): Hold. The menu does not stop the clock (CLOCK bit 1 clear, REPLY_MENU's
// freezes bit clear, the server holds no speaker), another character's order is carried out with the menu open, and the player character's own
// move order ends the talk with reason 4 (walked away) and is then applied.
void holdRealTime(const std::shared_ptr<const Data>& data, std::shared_ptr<ksim::MeshNavigator> nav)
{
	World w(data, nav, false);
	const std::uint64_t other = w.spawn("", data->nameless, 9, ks::kHubX - 30.0, ks::kHubZ - 30.0, 0.0, true);
	Server server(w);
	Client c(server.port);
	const std::uint64_t speaker = openMenu(w, c);
	const simlink::SimLinkDecoder::ReplyMenu menu = *c.menu(w.player);
	check((menu.flags & simlink::SimLinkDecoder::ReplyMenu::kFreezes) == 0, "under Hold the REPLY_MENU does not say it freezes (flags bit 2)");
	check((c.dec.clock.pauseBits & 2) == 0, "under Hold CLOCK has no dialogue pause bit");
	check(w.context.menuHolds.empty(), "under Hold the server holds no speaker");
	const std::uint64_t open = w.sim->clockTick();
	test::require(c.pump([&]() { return w.sim->clockTick() > open + 40; }, 5000), "the clock stopped with the menu open under Hold");
	check(c.menu(w.player) != nullptr && c.talkEnd(speaker) == nullptr, "the menu is still open after two seconds of game time");
	// A third character's move order does not touch the talk.
	c.send(moveOrder(other, ks::kHubX + 60.0, ks::kHubZ - 30.0));
	sleepMs(1500);
	check(c.talkEnd(speaker) == nullptr, "another character's move order left the menu open");
	// The player character's own move order ends it (reason 4) and is applied.
	c.send(moveOrder(w.player, ks::kHubX + 40.0, ks::kHubZ + 40.0));
	test::require(c.pump([&]() { return c.talkEnd(speaker) != nullptr; }, 5000), "the player's move order did not end the talk");
	check(c.talkEnd(speaker)->result == 4, "the move order ends the talk with reason 4 (walked away)");
	sleepMs(1500);
	server.stopNow();
	const auto moved = [&](std::uint64_t id, double fromX, double fromZ) { const auto* f = w.sim->combat()->find(id); return std::hypot(f->x - fromX, f->z - fromZ); };
	check(moved(other, ks::kHubX - 30.0, ks::kHubZ - 30.0) > 10.0, "the third character walked while the menu was open");
	check(moved(w.player, ks::kHubX, ks::kHubZ) > 5.0, "the player's move order was applied after it ended the talk");
	std::cout << "hold: the clock ran with the menu open, a third character walked, the player's move order ended the talk with reason 4 and ran\n";
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc == 2, "usage: ksim_dialogue_menu HUB.knav");
		const auto data = std::make_shared<const Data>();
		const std::shared_ptr<ksim::MeshNavigator> nav = ksim::MeshNavigator::load(argv[1]);
		holdRealTime(data, nav);
		freezeAndReply(data, nav);
		endTalk(data, nav);
		reconnect(data, nav);
		grace(data, nav); // last: it sleeps out the 30 s session grace
		std::cout << "ksim_dialogue_menu: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_dialogue_menu: FAILED: " << e.what() << "\n";
		return 1;
	}
}
