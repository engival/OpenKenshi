// The debug view's weather, time and option commands (docs/formats/SIMLINK.md "DEBUG VIEW"): DEBUG_SET_WEATHER (213),
// DEBUG_SET_TIME (214) and DEBUG_SET_OPTION (215) over a real session, answered with DEBUG_ACK (204), and the new-game
// options read back with DEBUG_QUERY kind 2 (World). A pin holds its weather past a season end and its own lifetime, a
// release picks fresh, a time set jumps forward and its send carries CLOCK, and every refusal carries its reason. The hold
// is not saved; the options are (state::WorldOptions).

#include "ksim/core/clock.hpp"
#include "ksim/combat/combat.hpp"
#include "ksim/combat/combat_data.hpp"
#include "ksim/debugview/enums.hpp"
#include "ksim/net/simlink.hpp"
#include "ksim/core/simulation.hpp"
#include "ksim/core/state.hpp"
#include "ksim/world/weather.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"
#include "sim_access.hpp"

#include "test_net.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

namespace ks = ksim::simlink;
using Wall = std::chrono::steady_clock;


struct Frame
{
	std::uint16_t type = 0;
	std::vector<std::uint8_t> payload;
};

std::vector<std::uint8_t> frameBytes(std::uint16_t type, const std::vector<std::uint8_t>& payload);
std::vector<std::uint8_t> helloBytes(std::uint64_t dataHash);

struct ClockFrame
{
	std::uint64_t tick = 0;
	std::uint32_t day = 0;
	float hour = 0.0f;
};

struct AckReply
{
	bool answered = false;
	bool accepted = false;
	std::string reason;
};

// A raw client: DEBUG_ACK has no decoder yet, and only CLOCK needs its fields read here.
struct Client
{
	int fd = -1;
	std::vector<std::uint8_t> in;
	std::map<std::uint16_t, std::size_t> seen; // frames received by type, for a timeout's report
	std::vector<ClockFrame> clocks; // every CLOCK, in the order it arrived

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
	~Client()
	{
		if (fd >= 0)
			test_net::close(fd);
	}
	Client(const Client&) = delete;
	Client& operator=(const Client&) = delete;

	void send(const std::vector<std::uint8_t>& bytes)
	{
		std::size_t at = 0;
		while (at < bytes.size())
		{
			const auto n = test_net::send(fd, bytes.data() + at, bytes.size() - at, test_net::kMsgNoSignal);
			test::require(n > 0, "client send");
			at += static_cast<std::size_t>(n);
		}
	}

	// One frame; false on timeout.
	bool next(Frame& frame, int timeoutMs)
	{
		const auto fill = [&]() {
			pollfd p{ fd, POLLIN, 0 };
			if (test_net::poll(&p, 1, timeoutMs) <= 0)
				return false;
			std::uint8_t block[65536];
			const auto n = test_net::recv(fd, block, sizeof block, 0);
			test::require(n > 0, "client recv");
			in.insert(in.end(), block, block + n);
			return true;
		};
		while (in.size() < 6)
			if (!fill())
				return false;
		std::uint32_t length = 0;
		std::memcpy(&length, in.data(), sizeof length);
		test::require(length >= 2 && length <= (1u << 20), "frame length");
		while (in.size() < 4 + length)
			if (!fill())
				return false;
		std::memcpy(&frame.type, in.data() + 4, sizeof frame.type);
		frame.payload.assign(in.begin() + 6, in.begin() + 4 + length);
		in.erase(in.begin(), in.begin() + static_cast<std::ptrdiff_t>(4 + length));
		++seen[frame.type];
		if (frame.type == ks::kClock)
			readClock(frame.payload);
		return true;
	}

	// tick u64, day u32, hourOfDay f32, gameSpeed f32, paused u8.
	void readClock(const std::vector<std::uint8_t>& p)
	{
		test::require(p.size() >= 16, "CLOCK is shorter than its header");
		ClockFrame clock;
		std::memcpy(&clock.tick, p.data(), sizeof clock.tick);
		std::memcpy(&clock.day, p.data() + 8, sizeof clock.day);
		std::memcpy(&clock.hour, p.data() + 12, sizeof clock.hour);
		clocks.push_back(clock);
	}

	// Reads frames until `match` takes one, with a PING every 4 s of silence (the server drops a silent client after
	// pingTimeoutMs, 30 s).
	bool waitFor(int seconds, const std::function<bool(const Frame&)>& match)
	{
		Frame frame;
		const auto deadline = Wall::now() + std::chrono::seconds(seconds);
		auto pinged = Wall::now();
		while (Wall::now() < deadline)
		{
			if (!next(frame, 200))
			{
				if (Wall::now() - pinged >= std::chrono::seconds(4))
				{
					pinged = Wall::now();
					send(frameBytes(ks::kPing, {}));
				}
				continue;
			}
			if (match(frame))
				return true;
		}
		return false;
	}

	// The DEBUG_TREE answering DEBUG_QUERY kind 2 (the world's options): key -> value, flattened.
	bool worldTree(std::map<std::string, std::string>& rows)
	{
		return waitFor(60, [&](const Frame& frame)
		{
			if (frame.type != ksim::debugview::kDebugTree)
				return false;
			test::require(frame.payload.size() >= 19, "DEBUG_TREE is shorter than its header");
			if (frame.payload[8] != static_cast<std::uint8_t>(ksim::debugview::QueryKind::World))
				return false; // another query's tree
			std::uint16_t count = 0;
			std::memcpy(&count, frame.payload.data() + 17, sizeof count);
			std::size_t at = 19;
			const auto text = [&](std::string& out)
			{
				test::require(at + 2 <= frame.payload.size(), "DEBUG_TREE string overrun");
				std::uint16_t length = 0;
				std::memcpy(&length, frame.payload.data() + at, sizeof length);
				at += 2;
				test::require(at + length <= frame.payload.size(), "DEBUG_TREE string overrun");
				out.assign(reinterpret_cast<const char*>(frame.payload.data() + at), length);
				at += length;
			};
			for (std::uint16_t i = 0; i < count; ++i)
			{
				test::require(at + 2 <= frame.payload.size(), "DEBUG_TREE row overrun");
				std::uint16_t depth = 0;
				std::memcpy(&depth, frame.payload.data() + at, sizeof depth);
				at += 2;
				std::string key, value;
				text(key);
				text(value);
				rows[key] = value;
			}
			test::require(at == frame.payload.size(), "DEBUG_TREE has trailing bytes");
			return true;
		});
	}

	// What arrived, for a timeout's message.
	std::string report() const
	{
		std::string text;
		for (const auto& [type, count] : seen)
			text += " " + std::to_string(type) + "x" + std::to_string(count);
		return seen.empty() ? std::string("no frames received") : "frames by type:" + text;
	}

	// The DEBUG_ACK answering `clientSeq` / `frameType`.
	bool ack(std::uint32_t clientSeq, std::uint16_t frameType, AckReply& reply)
	{
		return waitFor(60, [&](const Frame& frame)
		{
			if (frame.type != ksim::debugview::kDebugAck)
				return false;
			test::require(frame.payload.size() >= 9, "DEBUG_ACK is shorter than its header");
			std::uint32_t seq = 0;
			std::uint16_t type = 0;
			std::memcpy(&seq, frame.payload.data(), sizeof seq);
			std::memcpy(&type, frame.payload.data() + 4, sizeof type);
			if (seq != clientSeq || type != frameType)
				return false;
			reply.accepted = frame.payload[6] != 0;
			std::uint16_t length = 0;
			std::memcpy(&length, frame.payload.data() + 7, sizeof length);
			test::require(frame.payload.size() >= 9 + static_cast<std::size_t>(length),
				"DEBUG_ACK reason runs past the frame");
			reply.reason.assign(reinterpret_cast<const char*>(frame.payload.data() + 9), length);
			reply.answered = true;
			return true;
		});
	}

	// One command and its answer. `answered` is false when no DEBUG_ACK for it arrived.
	AckReply run(std::uint16_t type, const std::vector<std::uint8_t>& payload, std::uint32_t clientSeq)
	{
		send(frameBytes(type, payload));
		AckReply reply;
		if (!ack(clientSeq, type, reply))
			std::cout << "  " << type << " seq " << clientSeq << " unanswered: " << report() << "\n";
		return reply;
	}

	bool sawWeather(std::size_t before, int seconds)
	{
		return waitFor(seconds, [&](const Frame&) { return seen[ks::kWeather] > before; });
	}
};

// The client's HELLO (SIMLINK.md section 3), then the server's HELLO and the initial snapshot: only then can a command
// expect its answer in the next tick's send.
void handshake(Client& c, std::uint64_t dataHash)
{
	c.send(helloBytes(dataHash));
	test::require(c.waitFor(180, [](const Frame& f) { return f.type == ks::kHello; }),
		"no HELLO from the server: " + c.report());
	test::require(c.waitFor(180, [](const Frame& f) { return f.type == ks::kSnapEnd; }),
		"no initial snapshot: " + c.report());
}

struct Server
{
	ks::ServerContext context;
	ks::SimLinkServer server;
	std::promise<int> portPromise;
	std::thread thread;
	std::atomic<bool> stop{ false };
	int port = 0;

	Server(ksim::Simulation& sim, std::shared_ptr<const kdata::GameData> gd, std::uint64_t dataHash, bool debugView)
		: server(sim, context, options())
	{
		context.gameData = std::move(gd);
		context.dataHash = dataHash;
		context.squadLimit = 0;
		context.debugView = debugView;
		std::future<int> ready = portPromise.get_future();
		server.listen();
		port = ready.get();
		thread = std::thread([this]() { server.run(stop); });
	}
	~Server()
	{
		stop.store(true);
		if (thread.joinable())
			thread.join();
	}
	Server(const Server&) = delete;
	Server& operator=(const Server&) = delete;

private:
	ks::ServerOptions options()
	{
		ks::ServerOptions result;
		result.onListening = [this](int value) { portPromise.set_value(value); };
		// The sim always ticks: the reply rides a send tick, and this test must not depend on the ready-session pause policy.
		result.runWithoutClients = true;
		return result;
	}
};

std::vector<std::uint8_t> frameBytes(std::uint16_t type, const std::vector<std::uint8_t>& payload)
{
	std::vector<std::uint8_t> out(6);
	const std::uint32_t length = static_cast<std::uint32_t>(payload.size() + 2);
	std::memcpy(out.data(), &length, sizeof length);
	std::memcpy(out.data() + 4, &type, sizeof type);
	out.insert(out.end(), payload.begin(), payload.end());
	return out;
}

// The client's HELLO (SIMLINK.md section 3): magic, protocol, role 0 (client), the data hash, the name.
std::vector<std::uint8_t> helloBytes(std::uint64_t dataHash)
{
	std::vector<std::uint8_t> payload;
	const auto put = [&payload](const auto& value) {
		const std::size_t at = payload.size();
		payload.resize(at + sizeof value);
		std::memcpy(payload.data() + at, &value, sizeof value);
	};
	put(ks::kMagic);
	put(ks::kProtocol);
	put(static_cast<std::uint8_t>(0));
	put(dataHash);
	const std::string name = "debug-weather-time-test";
	put(static_cast<std::uint16_t>(name.size()));
	payload.insert(payload.end(), name.begin(), name.end());
	return frameBytes(ks::kHello, payload);
}

std::vector<std::uint8_t> weatherPayload(std::uint32_t seq, const std::string& weather, const std::string& group, float strength)
{
	std::vector<std::uint8_t> out;
	const auto put = [&out](const auto& value) {
		const std::size_t at = out.size();
		out.resize(at + sizeof value);
		std::memcpy(out.data() + at, &value, sizeof value);
	};
	put(seq);
	for (const std::string& text : { weather, group })
	{
		put(static_cast<std::uint16_t>(text.size()));
		out.insert(out.end(), text.begin(), text.end());
	}
	put(strength);
	return out;
}

std::vector<std::uint8_t> timePayload(std::uint32_t seq, float hour, std::uint8_t extraDays)
{
	std::vector<std::uint8_t> out;
	const auto put = [&out](const auto& value) {
		const std::size_t at = out.size();
		out.resize(at + sizeof value);
		std::memcpy(out.data() + at, &value, sizeof value);
	};
	put(seq);
	put(hour);
	put(extraDays);
	return out;
}

// One command with the next clientSeq.
AckReply setWeather(Client& c, std::uint32_t& seq, const std::string& weather, const std::string& group, float strength)
{
	const AckReply reply = c.run(ksim::debugview::kDebugSetWeather, weatherPayload(seq, weather, group, strength), seq);
	++seq;
	return reply;
}

AckReply setTime(Client& c, std::uint32_t& seq, float hour, std::uint8_t extraDays)
{
	const AckReply reply = c.run(ksim::debugview::kDebugSetTime, timePayload(seq, hour, extraDays), seq);
	++seq;
	return reply;
}

// u32 clientSeq, str key, f32 value (DEBUG_SET_OPTION).
std::vector<std::uint8_t> optionPayload(std::uint32_t seq, const std::string& key, float value)
{
	std::vector<std::uint8_t> out;
	const auto put = [&out](const auto& v) {
		const std::size_t at = out.size();
		out.resize(at + sizeof v);
		std::memcpy(out.data() + at, &v, sizeof v);
	};
	put(seq);
	put(static_cast<std::uint16_t>(key.size()));
	out.insert(out.end(), key.begin(), key.end());
	put(value);
	return out;
}

// u8 kind, u64 id (DEBUG_QUERY).
std::vector<std::uint8_t> queryPayload(ksim::debugview::QueryKind kind, std::uint64_t id)
{
	std::vector<std::uint8_t> out;
	const auto put = [&out](const auto& v) {
		const std::size_t at = out.size();
		out.resize(at + sizeof v);
		std::memcpy(out.data() + at, &v, sizeof v);
	};
	put(static_cast<std::uint8_t>(kind));
	put(id);
	return out;
}

// u32 clientSeq, u64 character, u8 op (DEBUG_CHEAT).
AckReply cheat(Client& c, std::uint32_t& seq, std::uint64_t character, std::uint8_t op)
{
	std::vector<std::uint8_t> out(13);
	std::memcpy(out.data(), &seq, sizeof seq);
	std::memcpy(out.data() + 4, &character, sizeof character);
	out[12] = op;
	const AckReply reply = c.run(ksim::debugview::kDebugCheat, out, seq);
	++seq;
	return reply;
}

AckReply setOption(Client& c, std::uint32_t& seq, const std::string& key, float value)
{
	const AckReply reply = c.run(ksim::debugview::kDebugSetOption, optionPayload(seq, key, value), seq);
	++seq;
	return reply;
}

const std::vector<ksim::state::WeatherRegion>& regionsOf(const ksim::Simulation& sim)
{
	const ksim::state::WeatherRegions* regions = sim.world().ecs().get<ksim::state::WeatherRegions>();
	test::require(regions != nullptr, "no WeatherRegions singleton");
	return regions->regions;
}

const ksim::state::WeatherRegion* regionOf(const ksim::Simulation& sim, const std::string& group)
{
	for (const ksim::state::WeatherRegion& region : regionsOf(sim))
		if (region.group == group)
			return &region;
	return nullptr;
}

// The first CLOCK at or after `from` that reads `hour` (the jump the set made, in the same send as its ACK).
std::optional<ClockFrame> clockAt(const Client& c, std::size_t from, float hour)
{
	for (std::size_t i = from; i < c.clocks.size(); ++i)
		if (std::fabs(c.clocks[i].hour - hour) < 0.01f)
			return c.clocks[i];
	return std::nullopt;
}

// The first CLOCK at or after `from` that reads `hour` on `day`, waiting for later sends. A day jump's send can be preceded
// by a routine 1 Hz CLOCK taken before the set applied (the same hour, the old day) when the box is busy; that one is not the
// answer, so the day is part of the match.
std::optional<ClockFrame> clockDayAt(Client& c, std::size_t from, float hour, std::uint32_t day)
{
	const auto deadline = Wall::now() + std::chrono::seconds(10);
	for (;;)
	{
		for (std::size_t i = from; i < c.clocks.size(); ++i)
			if (std::fabs(c.clocks[i].hour - hour) < 0.01f && c.clocks[i].day == day)
				return c.clocks[i];
		if (Wall::now() >= deadline)
			return std::nullopt;
		Frame frame;
		c.next(frame, 100); // a timeout here is normal: keep waiting for the frame
	}
}

} // namespace

int main()
{
#if !KSIM_DEBUG_VIEW
	// Compiled out: frames 200-219 are accepted and dropped (SIMLINK.md "DEBUG VIEW"), so 213/214 are never answered.
	std::cout << "ksim_debug_weather_time: SKIPPED: KSIM_DEBUG_VIEW is off (the debug frames are compiled out)\n";
	return 77;
#endif
	try
	{
		const kworld::LoadOrder order = kworld::resolveLoadOrder(KENSHI_DATA);
		const auto gd = std::make_shared<const kdata::GameData>(kworld::loadMergedGameData(order));
		const std::uint64_t dataHash = ks::dataHash(order);
		ksim::SpawnOptions spawnOptions;
		spawnOptions.seed = 7;
		auto sim = std::make_unique<ksim::Simulation>(ksim::Simulation::newGame(KENSHI_DATA, order, spawnOptions));
		const std::shared_ptr<const ksim::CombatData> combat = ksim::CombatData::build(*gd, KENSHI_DATA);
		sim->enableCombat(combat, ksim::CombatSettings{});
		test::require(sim->weather() != nullptr, "the new game has no weather");
		const ksim::WeatherModel& model = sim->weather()->model();

		// The group under the Hub (the player character's group later) and two real WEATHER records.
		const std::string groupSid = sim->weather()->groupAt(kHubX, kHubZ);
		test::require(!groupSid.empty(), "the Hub is in no biome group (no areas map?)");
		std::vector<std::string> weathers;
		for (const kdata::Record* record : gd->byType(kworld::itemTypeByName("WEATHER")))
			if (std::find(weathers.begin(), weathers.end(), record->stringID) == weathers.end())
				weathers.push_back(record->stringID);
		test::require(weathers.size() >= 2, "fewer than two WEATHER records in the data");
		const std::string weatherA = weathers[0];
		const std::string weatherB = weathers[1];

		// One tick so the regions have their season and end day (the pin changes neither).
		sim->run(1);
		const ksim::state::WeatherRegion* started = regionOf(*sim, groupSid);
		test::require(started != nullptr && started->started == 1, "the group's region did not start");
		const std::uint32_t seasonAtPin = started->season;
		const std::uint64_t seasonEndAtPin = started->seasonEndDay;
		const std::uint64_t pinMinute = ksim::gameMinuteAt(sim->clockTick());

		// ---- the view off: both commands answer with a reason, the world untouched ----
		{
			Server server(*sim, gd, dataHash, false);
			Client c(server.port);
			handshake(c, dataHash);
			AckReply a = c.run(ksim::debugview::kDebugSetWeather, weatherPayload(1, weatherA, groupSid, 0.5f), 1);
			test::require(a.answered && !a.accepted && a.reason.find("debug view is off") != std::string::npos,
				"view off, 213: " + a.reason);
			a = c.run(ksim::debugview::kDebugSetTime, timePayload(2, 3.0f, 0), 2);
			test::require(a.answered && !a.accepted && a.reason.find("debug view is off") != std::string::npos,
				"view off, 214: " + a.reason);
			a = c.run(ksim::debugview::kDebugSetOption, optionPayload(3, "cod", 1.5f), 3);
			test::require(a.answered && !a.accepted && a.reason.find("debug view is off") != std::string::npos,
				"view off, 215: " + a.reason);
			test::require(sim->worldOptions() == nullptr, "a refused option made the singleton");
			std::uint32_t cheatSeq = 4;
			a = cheat(c, cheatSeq, 0, 3);
			test::require(a.answered && !a.accepted && a.reason.find("debug view is off") != std::string::npos,
				"view off, 216: " + a.reason);
			std::cout << "view off refused all four: " << a.reason << "\n";
		}

		// ---- the view on, no player character: refusals ----
		{
			Server server(*sim, gd, dataHash, true);
			Client c(server.port);
			handshake(c, dataHash);
			AckReply a = c.run(ksim::debugview::kDebugSetWeather, weatherPayload(3, weatherA, "", 0.5f), 3);
			test::require(a.answered && !a.accepted && a.reason.find("no player character") != std::string::npos,
				"empty group without a player character: " + a.reason);
			a = c.run(ksim::debugview::kDebugSetWeather, weatherPayload(4, "999999-nope.mod", groupSid, 0.5f), 4);
			test::require(a.answered && !a.accepted && a.reason.find("unknown weather") != std::string::npos,
				"unknown weather: " + a.reason);
			a = c.run(ksim::debugview::kDebugSetWeather, weatherPayload(5, weatherA, "999999-nope.mod", 0.5f), 5);
			test::require(a.answered && !a.accepted && a.reason.find("unknown biome group") != std::string::npos,
				"unknown biome group: " + a.reason);
			a = c.run(ksim::debugview::kDebugSetTime, timePayload(6, 24.0f, 0), 6);
			test::require(a.answered && !a.accepted && a.reason.find("hour out of range") != std::string::npos,
				"hour 24: " + a.reason);
			a = c.run(ksim::debugview::kDebugSetTime, timePayload(7, -1.0f, 0), 7);
			test::require(a.answered && !a.accepted && a.reason.find("hour out of range") != std::string::npos,
				"hour -1: " + a.reason);
			a = c.run(ksim::debugview::kDebugSetTime, timePayload(8, 3.0f, 8), 8);
			test::require(a.answered && !a.accepted && a.reason.find("extraDays out of range") != std::string::npos,
				"extraDays 8: " + a.reason);
			std::cout << "refusals: " << a.reason << "\n";
		}

		// A player character at the Hub, so biomeGroup "" resolves.
		std::string bodySid;
		for (const kdata::Record* record : gd->byType(kworld::itemTypeByName("CHARACTER")))
			if (record->name == "Escaped Servant")
				bodySid = record->stringID;
		std::string factionSid;
		for (const kdata::Record* record : gd->byType(kworld::itemTypeByName("FACTION")))
			if (record->name == "Nameless")
				factionSid = record->stringID;
		test::require(!bodySid.empty() && !factionSid.empty(), "no Escaped Servant / Nameless faction in the data");
		sim->createPlayerFaction(factionSid);
		const std::uint64_t squad = sim->addPlayerSquad();
		std::uint64_t rng = 12345;
		ksim::state::Fighter player = combat->makeFighter(*gd, bodySid, "", rng);
		player.playerControlled = true;
		player.team = 0;
		player.x = kHubX;
		player.z = kHubZ;
		const std::uint64_t playerId = sim->spawnFighter(std::move(player)).get<ksim::state::Id>()->value;
		sim->addPlayerMember(squad, playerId);

		// ---- the pin (group "" = the player's group), then the clock jumps past its season end and lifetime ----
		std::uint32_t seq = 10;
		std::uint64_t pinStartMinute = 0;
		{
			Server server(*sim, gd, dataHash, true);
			Client c(server.port);
			handshake(c, dataHash);
			const std::size_t weatherBefore = c.seen[ks::kWeather];
			AckReply a = setWeather(c, seq, weatherA, "", 0.4f);
			test::require(a.answered && a.accepted, "the pin was refused: " + a.reason);
			// The pin is a pick: a WEATHER frame goes out at once.
			test::require(c.seen[ks::kWeather] > weatherBefore || c.sawWeather(weatherBefore, 60),
				"the pin sent no WEATHER frame");

			// Forward to 03:00 (the send carries CLOCK), then +2 more days.
			const std::size_t clocksBefore = c.clocks.size();
			a = setTime(c, seq, 3.0f, 0);
			test::require(a.answered && a.accepted, "the first time set was refused: " + a.reason);
			const std::optional<ClockFrame> first = clockAt(c, clocksBefore, 3.0f);
			test::require(first.has_value(), "the time set's send carried no CLOCK at 03:00");
			const std::size_t clocksJumped = c.clocks.size();
			a = setTime(c, seq, 3.0f, 2);
			test::require(a.answered && a.accepted, "the extraDays set was refused: " + a.reason);
			const std::optional<ClockFrame> second = clockDayAt(c, clocksJumped, 3.0f, first->day + 3);
			test::require(second.has_value(),
				"extraDays 2 from just past 03:00 did not land 3 days on (no CLOCK at 03:00 on day "
					+ std::to_string(first->day + 3) + ")");
			std::cout << "time: day " << first->day << " -> " << second->day << " at 03:00\n";

			// Jump past the season end and the pin's ten-day lifetime (8 days per call from 03:00).
			const std::uint64_t needDay = std::max(seasonEndAtPin, pinMinute / 1440 + 10) + 2;
			std::size_t calls = 0;
			while (second->day + 8 * calls < needDay || calls == 0)
			{
				a = setTime(c, seq, 3.0f, 7);
				test::require(a.answered && a.accepted, "a long jump was refused: " + a.reason);
				++calls;
			}
			std::cout << "jumped " << calls << " x 8 days past season end " << seasonEndAtPin << "\n";
		}
		{
			const ksim::state::WeatherRegion* pinned = regionOf(*sim, groupSid);
			test::require(pinned != nullptr, "the group's region vanished");
			test::require(pinned->weather == weatherA && pinned->held == 1,
				"the pin did not hold past its season end and lifetime (" + pinned->weather + ")");
			pinStartMinute = pinned->startMinute;
			const std::uint64_t nowMinute = ksim::gameMinuteAt(sim->clockTick());
			test::require(nowMinute > pinned->endMinute, "the pin's lifetime did not pass");
			// The end day was passed and the region moved on to the next season (a group with one season keeps index 0, so
			// the end-day bookkeeping is what tells).
			test::require(nowMinute / 1440 >= seasonEndAtPin, "the jump did not pass the season end day");
			test::require(pinned->seasonEndDay != seasonEndAtPin, "the season did not advance at its end day");
			std::cout << "pin held: season " << seasonAtPin << " -> " << pinned->season << " (end day "
				<< seasonEndAtPin << " -> " << pinned->seasonEndDay << "), "
				<< nowMinute - pinned->endMinute << " min past its lifetime\n";
		}

		// ---- the release: a fresh pick next tick ----
		{
			Server server(*sim, gd, dataHash, true);
			Client c(server.port);
			handshake(c, dataHash);
			const std::size_t weatherBefore = c.seen[ks::kWeather];
			AckReply a = setWeather(c, seq, "", groupSid, 0.0f);
			test::require(a.answered && a.accepted, "the release was refused: " + a.reason);
			test::require(c.seen[ks::kWeather] > weatherBefore || c.sawWeather(weatherBefore, 60),
				"the release sent no WEATHER frame");
		}
		{
			const ksim::state::WeatherRegion* released = regionOf(*sim, groupSid);
			test::require(released != nullptr && released->held == 0 && released->started == 1,
				"the release did not clear the hold and re-start the region");
			test::require(!released->weather.empty(), "the release left the region without a weather");
			test::require(released->startMinute > pinStartMinute, "the release did not make a fresh pick");
			std::cout << "released: fresh weather " << released->weather << "\n";
		}

		// ---- "*": every group with a region is pinned, with the frame's strength ----
		{
			Server server(*sim, gd, dataHash, true);
			Client c(server.port);
			handshake(c, dataHash);
			AckReply a = setWeather(c, seq, weatherB, "*", 0.25f);
			test::require(a.answered && a.accepted, "the every-group pin was refused: " + a.reason);
		}
		{
			const std::vector<ksim::state::WeatherRegion>& regions = regionsOf(*sim);
			test::require(regions.size() == model.groups().size(), "a region per group expected");
			for (const ksim::state::WeatherRegion& region : regions)
			{
				test::require(region.weather == weatherB && region.held == 1,
					"group " + region.group + " was not pinned by '*'");
				test::require(std::fabs(region.strength - 0.25) < 1e-6, "group " + region.group + " kept another strength");
			}
			std::cout << "'*' pinned " << regions.size() << " groups\n";
		}

		// ---- a save keeps the weather but not the hold ----
		{
			std::ostringstream saved;
			sim->save(saved);
			std::istringstream in(saved.str());
			ksim::Simulation loaded = ksim::Simulation::restore(ksim::World::load(KENSHI_DATA), in, {}, false, nullptr, gd);
			test::require(loaded.weather() != nullptr, "the loaded save has no weather");
			const std::vector<ksim::state::WeatherRegion>& regions = regionsOf(loaded);
			test::require(regions.size() == model.groups().size(), "a region per group after the load");
			for (const ksim::state::WeatherRegion& region : regions)
			{
				test::require(region.weather == weatherB && region.started == 1,
					"the save did not keep group " + region.group + "'s pinned weather");
				test::require(region.held == 0, "the save kept the hold of group " + region.group);
			}
			std::cout << "save/load: the weather stayed, the hold did not\n";
		}

		// ---- the every-group release: every region picks again ----
		{
			Server server(*sim, gd, dataHash, true);
			Client c(server.port);
			handshake(c, dataHash);
			const std::size_t weatherBefore = c.seen[ks::kWeather];
			AckReply a = setWeather(c, seq, "", "*", 0.0f);
			test::require(a.answered && a.accepted, "the every-group release was refused: " + a.reason);
			test::require(c.sawWeather(weatherBefore, 60), "the every-group release sent no WEATHER frame");
		}
		{
			for (const ksim::state::WeatherRegion& region : regionsOf(*sim))
			{
				test::require(region.held == 0 && region.started == 1 && !region.weather.empty(),
					"group " + region.group + " did not pick fresh after the every-group release");
			}
			std::cout << "'*' released " << regionsOf(*sim).size() << " groups\n";
		}

		// ---- the new-game options (ADVANCED_OPTIONS_STATIC.md "Ours", task BT): DEBUG_SET_OPTION 215, read back with
		// DEBUG_QUERY kind 2 (World) ----
		{
			Server server(*sim, gd, dataHash, true);
			Client c(server.port);
			handshake(c, dataHash);
			AckReply a = setOption(c, seq, "bs", 1.0f);
			test::require(a.answered && !a.accepted && a.reason == "unknown option 'bs'",
				"an unimplemented knob is refused: " + a.reason);
			a = setOption(c, seq, "cod", 4.5f);
			test::require(a.answered && !a.accepted && a.reason == "'cod' out of range 0.5..4", "cod 4.5: " + a.reason);
			test::require(sim->worldOptions() == nullptr, "a refused option made the singleton");

			a = setOption(c, seq, "cod", 1.5f);
			test::require(a.answered && a.accepted, "cod 1.5: " + a.reason);
			a = setOption(c, seq, "gdm", 0.5f);
			test::require(a.answered && a.accepted, "gdm 0.5: " + a.reason);
			a = setOption(c, seq, "ht", 6.0f);
			test::require(a.answered && a.accepted, "ht 6: " + a.reason);
			a = setOption(c, seq, "medicalClock", 1.25f);
			test::require(a.answered && a.accepted, "the clock override 1.25: " + a.reason);
			const ksim::state::WorldOptions* options = sim->worldOptions();
			test::require(options != nullptr && options->chanceOfDeath == 1.5f && options->damageScale == 0.5f
				&& options->hungerTime == 6.0f, "the world's options took cod / gdm / ht");
			const ksim::CombatSettings& live = sim->combat()->rules().settings();
			test::require(live.chanceOfDeath == 1.5f && live.damageScale == 0.5f && live.hungerTime == 6.0f
				&& live.medicalClock == 1.25f, "the arena's copy took all four");

			c.send(frameBytes(ksim::debugview::kDebugQuery, queryPayload(ksim::debugview::QueryKind::World, 0)));
			std::map<std::string, std::string> rows;
			test::require(c.worldTree(rows), "no DEBUG_TREE for the world query: " + c.report());
			test::require(rows["cod"] == "1.5" && rows["gdm"] == "0.5" && rows["ht"] == "6" && rows["saved"] == "yes",
				"the world tree reads the saved options (cod " + rows["cod"] + ", ht " + rows["ht"] + ", saved " + rows["saved"] + ")");
			test::require(rows["medicalClock"] == "1.25", "the world tree reads the clock override (" + rows["medicalClock"] + ")");
			std::cout << "options: cod 1.5, gdm 0.5, ht 6, medicalClock 1.25 (DEBUG_QUERY kind 2)\n";
		}

		// ---- DEBUG_CHEAT 216: heal / knock out / wake / kill one character (the first player character with id 0) ----
		{
			const auto fighter = [&]() -> ksim::state::Fighter* { return const_cast<ksim::state::Fighter*>(sim->combat()->find(playerId)); };
			const auto cheatOnce = [&](std::uint64_t character, std::uint8_t op)
			{
				AckReply a;
				{
					Server server(*sim, gd, dataHash, true);
					Client c(server.port);
					handshake(c, dataHash);
					a = cheat(c, seq, character, op);
				}
				sim->run(3); // the arena's next updates: the KO or the death lands
				return a;
			};
			AckReply a = cheatOnce(0, 9);
			test::require(a.answered && !a.accepted && a.reason == "unknown op 9", "op 9: " + a.reason);
			a = cheatOnce(999999999, 0);
			test::require(a.answered && !a.accepted && a.reason == "no such character", "unknown character: " + a.reason);
			a = cheatOnce(0, 2);
			test::require(a.answered && !a.accepted && a.reason == "not down", "wake an upright character: " + a.reason);

			a = cheatOnce(0, 1);
			test::require(a.answered && a.accepted, "knock out: " + a.reason);
			test::require(fighter()->unconscious && !fighter()->dead && fighter()->koTimer > 0.0, "the KO did not take");
			a = cheatOnce(playerId, 1);
			test::require(a.answered && !a.accepted && a.reason == "already down", "knock out a downed character: " + a.reason);

			fighter()->parts.front().health = fighter()->parts.front().maxHealth * 0.5;
			a = cheatOnce(0, 2);
			test::require(a.answered && a.accepted, "wake: " + a.reason);
			test::require(!fighter()->unconscious && fighter()->koTimer <= 0.0, "the wake left it down");
			test::require(fighter()->parts.front().health < fighter()->parts.front().maxHealth, "the wake healed a part (wounds are kept)");

			a = cheatOnce(0, 1);
			test::require(a.answered && a.accepted && fighter()->unconscious, "second knock out");
			a = cheatOnce(0, 0);
			test::require(a.answered && a.accepted, "heal: " + a.reason);
			{
				const ksim::state::Fighter* f = fighter();
				bool full = true;
				for (const ksim::state::FighterPart& part : f->parts)
					full = full && part.health >= part.maxHealth - 1e-6 && part.stun == 0.0;
				test::require(full && !f->unconscious && !f->down && f->koTimer <= 0.0 && f->blood >= sim->combat()->rules().maxBlood(*f) - 1e-6
					&& f->wounds.empty(), "the heal left a part, the blood, a wound or the KO behind");
			}

			a = cheatOnce(0, 3);
			test::require(a.answered && a.accepted, "kill: " + a.reason);
			test::require(fighter()->dead, "the kill did not kill");
			a = cheatOnce(playerId, 3);
			test::require(a.answered && !a.accepted && a.reason == "already dead", "kill a dead character: " + a.reason);
			a = cheatOnce(playerId, 0);
			test::require(a.answered && !a.accepted && a.reason == "already dead", "heal a dead character: " + a.reason);
			std::cout << "cheat 216: refusals, knock out, wake, heal, kill\n";
		}

		std::cout << "ksim_debug_weather_time: ok\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << "ksim_debug_weather_time: FAIL: " << error.what() << "\n";
		return 1;
	}
}
