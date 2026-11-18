// The debug view's spawn order (docs/formats/SIMLINK.md "DEBUG VIEW"): DEBUG_SPAWN_SQUAD (212) over a real session, answered
// with DEBUG_SPAWNED (203). A successful order places the squad in the world, so it appears in SQUADS and, with a town at the
// goal, walks toward it (the raid order its own planner gives, the group layer's Raid candidate). An unknown template, a spawn
// off the navmesh and the view switched off all answer squad 0 with a reason.

#include "ksim/ai/ai.hpp"
#include "ksim/ai/behaviours.hpp"
#include "ksim/combat/combat.hpp"
#include "ksim/combat/combat_data.hpp"
#include "ksim/debugview/enums.hpp"
#include "ksim/world/factions.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/net/simlink.hpp"
#include "ksim/net/simlink_scenario.hpp"
#include "ksim/core/simulation.hpp"
#include "ksim/core/state.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"
#include "sim_access.hpp"

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
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif
#ifndef KSIM_AI_TABLE
#error KSIM_AI_TABLE must be defined
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

// A raw client: DEBUG_SPAWNED (203) has no decoder yet, and SQUADS only needs its positions read here.
struct Client
{
	int fd = -1;
	std::vector<std::uint8_t> in;
	std::map<std::uint64_t, std::pair<double, double>> squads; // the latest SQUADS position by squad id
	std::map<std::uint16_t, std::size_t> seen; // frames received by type, for a timeout's report

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
			::close(fd);
	}
	Client(const Client&) = delete;
	Client& operator=(const Client&) = delete;

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

	// One frame; false on timeout.
	bool next(Frame& frame, int timeoutMs)
	{
		const auto fill = [&]() {
			pollfd p{ fd, POLLIN, 0 };
			if (::poll(&p, 1, timeoutMs) <= 0)
				return false;
			std::uint8_t block[65536];
			const ssize_t n = ::recv(fd, block, sizeof block, 0);
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
		if (frame.type == ks::kSquads)
			readSquads(frame.payload);
		return true;
	}

	// tick u64, list (u32 count) of: id u64, faction strid u32, x f32, z f32, members u16.
	void readSquads(const std::vector<std::uint8_t>& p)
	{
		std::uint32_t count = 0;
		test::require(p.size() >= 12, "SQUADS header");
		std::memcpy(&count, p.data() + 8, sizeof count);
		std::size_t at = 12;
		for (std::uint32_t i = 0; i < count; ++i)
		{
			test::require(p.size() - at >= 22, "SQUADS row");
			std::uint64_t id = 0;
			float x = 0.0f;
			float z = 0.0f;
			std::memcpy(&id, p.data() + at, sizeof id);
			std::memcpy(&x, p.data() + at + 12, sizeof x);
			std::memcpy(&z, p.data() + at + 16, sizeof z);
			squads[id] = { x, z };
			at += 22;
		}
	}

	// Reads frames until `match` takes one, with a PING every 4 s of silence (the server drops a silent client after
	// pingTimeoutMs, 30 s). The deadline is generous: the reply waits for the next send tick, and a cold box's first
	// snapshot can take a while, but the order of the frames is the protocol's, never the clock's.
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

	// What arrived, for a timeout's message.
	std::string report() const
	{
		std::string text;
		for (const auto& [type, count] : seen)
			text += " " + std::to_string(type) + "x" + std::to_string(count);
		return seen.empty() ? std::string("no frames received") : "frames by type:" + text;
	}

	// The DEBUG_SPAWNED answering `clientSeq`. A spawn changes the world, so one order goes out per call.
	bool spawnReply(std::uint32_t clientSeq, std::uint64_t& squad, std::string& reason)
	{
		const bool arrived = waitFor(60, [&](const Frame& frame)
		{
			if (frame.type != ksim::debugview::kDebugSpawned)
				return false;
			test::require(frame.payload.size() >= 14, "DEBUG_SPAWNED is shorter than its header");
			std::uint32_t seq = 0;
			std::memcpy(&seq, frame.payload.data(), sizeof seq);
			if (seq != clientSeq)
				return false;
			std::memcpy(&squad, frame.payload.data() + 4, sizeof squad);
			std::uint16_t length = 0;
			std::memcpy(&length, frame.payload.data() + 12, sizeof length);
			test::require(frame.payload.size() >= 14 + static_cast<std::size_t>(length),
				"DEBUG_SPAWNED reason runs past the frame");
			reason.assign(reinterpret_cast<const char*>(frame.payload.data() + 14), length);
			return true;
		});
		if (!arrived)
			std::cout << "  seq " << clientSeq << " unanswered: " << report() << "\n";
		return arrived;
	}
};

// The client's HELLO (SIMLINK.md section 3), then the server's HELLO and the initial snapshot: only then can an order
// expect its answer in the next tick's send (the snapshot is built in one write, before any later frame).
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
	const std::string name = "debug-spawn-test";
	put(static_cast<std::uint16_t>(name.size()));
	payload.insert(payload.end(), name.begin(), name.end());
	return frameBytes(ks::kHello, payload);
}

std::vector<std::uint8_t> spawnPayload(std::uint32_t seq, const std::string& templateSid,
	double sx, double sz, double tx, double tz)
{
	std::vector<std::uint8_t> out;
	const auto put = [&out](const auto& value) {
		const std::size_t at = out.size();
		out.resize(at + sizeof value);
		std::memcpy(out.data() + at, &value, sizeof value);
	};
	put(seq);
	put(static_cast<std::uint16_t>(templateSid.size()));
	out.insert(out.end(), templateSid.begin(), templateSid.end());
	put(static_cast<float>(sx));
	put(static_cast<float>(sz));
	put(static_cast<float>(tx));
	put(static_cast<float>(tz));
	return out;
}

} // namespace

int main()
{
#if !KSIM_DEBUG_VIEW
	// Compiled out: frames 200-219 are accepted and dropped (SIMLINK.md "DEBUG VIEW"), so 212 is never answered.
	std::cout << "ksim_debug_spawn: SKIPPED: KSIM_DEBUG_VIEW is off (the debug frames are compiled out)\n";
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
		// The Hub's zone: the spawn point, the goal town and the route between them are inside it.
		const auto nav = ksim::MeshNavigator::bake(KENSHI_DATA,
			{ ks::kHubZoneX, ks::kHubZoneZ }, { ks::kHubZoneX, ks::kHubZoneZ }, 4);
		sim->enableNavigation(nav);
		sim->enableCombat(ksim::CombatData::build(*gd, KENSHI_DATA), ksim::CombatSettings{});
		ksim::AiSystem::Config ai;
		ai.registry = std::make_shared<ksim::BehaviourRegistry>(sim->world(),
			ksim::BehaviourRegistry::readRules(KSIM_AI_TABLE));
		ai.navigator = nav;
		ai.factions = ksim::FactionBook::build(sim->gameData());
		sim->enableAi(std::move(ai));

		// The goal town: the nearest the AI knows to the Hub (the order picks a town within 2,000 u of the goal point).
		double townX = 0.0;
		double townZ = 0.0;
		double nearestTown = 1.0e300;
		sim->world().ecs().each([&](flecs::entity, const ksim::state::TownState& town)
		{
			const double far = std::hypot(town.x - kHubX, town.z - kHubZ);
			if (far < nearestTown)
			{
				nearestTown = far;
				townX = town.x;
				townZ = town.z;
			}
		});
		test::require(nearestTown < 2000.0, "no town within 2,000 u of the Hub in the new world");

		// The gate off (the server without --debug-view): the order is answered with a reason, not the world changed.
		{
			Server server(*sim, gd, dataHash, false);
			Client c(server.port);
			handshake(c, dataHash);
			std::uint64_t squad = 1;
			std::string reason;
			c.send(frameBytes(ksim::debugview::kDebugSpawnSquad, spawnPayload(1, "", -53000.0, 1000.0, townX, townZ)));
			test::require(c.spawnReply(1, squad, reason), "no DEBUG_SPAWNED with the view off");
			test::require(squad == 0 && reason.find("debug view is off") != std::string::npos, "view off: " + reason);
			std::cout << "view off refused: " << reason << "\n";
		}

		Server server(*sim, gd, dataHash, true);
		Client c(server.port);
		handshake(c, dataHash);

		std::uint64_t squad = 1;
		std::string reason;

		// An unknown template.
		c.send(frameBytes(ksim::debugview::kDebugSpawnSquad,
			spawnPayload(2, "999999-nope.mod", -53000.0, 1000.0, townX, townZ)));
		test::require(c.spawnReply(2, squad, reason), "no DEBUG_SPAWNED for the unknown template");
		test::require(squad == 0 && reason.find("unknown squad template") != std::string::npos,
			"unknown template: " + reason);
		std::cout << "unknown template refused: " << reason << "\n";

		// A spawn off the navmesh.
		c.send(frameBytes(ksim::debugview::kDebugSpawnSquad, spawnPayload(3, "", 0.0, 0.0, townX, townZ)));
		test::require(c.spawnReply(3, squad, reason), "no DEBUG_SPAWNED off the navmesh");
		test::require(squad == 0 && reason.find("no navmesh at the spawn") != std::string::npos,
			"off-navmesh spawn: " + reason);
		std::cout << "off-navmesh spawn refused: " << reason << "\n";

		// The order: the default template ("" = 1302-gamedata.base, the Dust Bandits) in the Hub's zone, the Hub as the goal.
		// The spawn point has to be in a wilderness slot; the first candidate the spawner takes is the one used.
		const std::pair<double, double> candidates[] = {
			{ -52000.0, 4000.0 }, { -52000.0, 3000.0 }, { -51200.0, 3800.0 }, { -51800.0, 2500.0 },
		};
		double spawnX = 0.0;
		double spawnZ = 0.0;
		std::uint32_t seq = 4;
		for (const auto& point : candidates)
		{
			c.send(frameBytes(ksim::debugview::kDebugSpawnSquad,
				spawnPayload(seq, "", point.first, point.second, townX, townZ)));
			test::require(c.spawnReply(seq, squad, reason), "no DEBUG_SPAWNED for a spawn");
			++seq;
			if (squad != 0)
			{
				spawnX = point.first;
				spawnZ = point.second;
				break;
			}
			std::cout << "spawn at (" << point.first << ", " << point.second << ") refused: " << reason << "\n";
		}
		test::require(squad != 0, "every spawn point was refused");
		std::cout << "spawned squad " << squad << " at (" << spawnX << ", " << spawnZ << "), goal town (" << townX
			<< ", " << townZ << "), " << nearestTown << " u from the Hub\n";

		// SQUADS carries it, and the raid order walks it toward the town.
		const auto townDistance = [&](double x, double z) { return std::hypot(x - townX, z - townZ); };
		bool seen = false;
		double first = 0.0;
		double last = 0.0;
		c.waitFor(60, [&](const Frame&)
		{
			const auto found = c.squads.find(squad);
			if (found == c.squads.end())
				return false;
			const double far = townDistance(found->second.first, found->second.second);
			if (!seen)
			{
				seen = true;
				first = far;
			}
			last = far;
			return first - last > 5.0;
		});
		test::require(seen, "the spawned squad never appeared in SQUADS (" + c.report() + ")");
		test::require(first - last > 5.0, "the squad did not walk toward the goal town ("
			+ std::to_string(first) + " -> " + std::to_string(last) + " u)");
		std::cout << "squad " << squad << " heading to the town: " << first << " -> " << last << " u\n";
		std::cout << "ksim_debug_spawn: ok\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << "ksim_debug_spawn: FAIL: " << error.what() << "\n";
		return 1;
	}
}
